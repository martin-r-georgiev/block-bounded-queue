#include <queue/bbq.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/thread/sync_bounded_queue.hpp>

#ifdef ENABLE_FOLLY_BENCHMARKS
    #include <folly/MPMCQueue.h>
    #include <folly/ProducerConsumerQueue.h>
#endif

constexpr std::size_t CACHELINE_SIZE = std::hardware_destructive_interference_size;

using q_val_t = uint64_t;

// Total queue size: 8 blocks x 10'000 entries x 8 bytes (sizeof q_val_t) = 640 KB (625 KiB)
// At the moment of writing, modern CPUs have L1 cache sizes around 80-96 KB per core, so this
// will intentionally cause cache misses on the queue to simulate a more realistic scenario.
constexpr uint64_t BBQ_NUM_BLOCKS = 8;
constexpr uint64_t BBQ_ENTRIES_PER_BLOCK = 10'000;
constexpr uint64_t BENCHMARK_ITEM_COUNT = 100'000'000;
constexpr queues::QueueMode QUEUE_MODE = queues::QueueMode::RETRY_NEW;

// Enable correctness checks for complex benchmarks; Slows down the benchmark significantly
constexpr bool CORR_CHECKS = false;

// Percentage of results to trim from both ends
constexpr double TRIM_PERCENTAGE = 0.1;

#if defined(__linux__)
    #include <sched.h>

/**
 * @brief Set the cpu affinity of the current thread
 *
 * @param cpu The CPU index to set affinity to.
 * @param max_threads The maximum number of threads to consider for CPU affinity.
 * @return int Returns 0 on success, non-zero on failure.
 */
static inline int set_cpu_affinity(int cpu, int max_threads = -1)
{
    int num_cpus = std::thread::hardware_concurrency();
    if (max_threads > 0 && max_threads < num_cpus)
        num_cpus = max_threads;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % num_cpus, &cpuset); // wrap around if oversubscribed
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#elif defined(_WIN32) || defined(WIN32) || defined(_WIN64)
    #include <windows.h>

// Source: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask

/**
 * @brief Set the cpu affinity of the current thread
 *
 * @param cpu The CPU index to set affinity to.
 * @param max_threads The maximum number of threads to consider for CPU affinity.
 * @return int Returns 0 on success, -1 on failure.
 */
static inline int set_cpu_affinity(int cpu, int max_threads = -1)
{
    int num_cpus = std::thread::hardware_concurrency();
    if (max_threads > 0 && max_threads < num_cpus)
        num_cpus = max_threads;

    DWORD_PTR mask = ((DWORD_PTR)1) << (cpu % num_cpus); // wrap around if oversubscribed
    HANDLE hProcess = GetCurrentProcess();
    return SetProcessAffinityMask(hProcess, mask) ? 0 : -1;
}
#else

/// @brief Set the cpu affinity of the current thread (Not implemented for non-Linux/Windows platforms).
static inline int set_cpu_affinity(int cpu, int max_threads = -1)
{
    return 0; // NOP for non-Linux/Windows platforms
}
#endif

/// @brief Structure to hold the results of a benchmark run.
struct BenchmarkResult
{
    std::string_view label = "UNKNOWN";
    uint8_t num_prod = 0;
    uint8_t num_cons = 0;
    uint64_t items_per_prod = 0;
    std::optional<uint64_t> enq_count = std::nullopt;
    std::optional<uint64_t> deq_count = std::nullopt;
    std::vector<q_val_t>* deq_items;
    uint64_t elapsed_total_us = 0;
    double mean_prod_us = 0.0;
    double mean_cons_us = 0.0;
    double mean_enq_latency_ns = 0.0;
    double mean_enq_full_latency_ns = 0.0;
    double mean_deq_latency_ns = 0.0;
    double mean_deq_empty_latency_ns = 0.0;
    bool is_bbq = false;
    queues::QueueMode queue_mode = queues::QueueMode::RETRY_NEW;
};

/// @brief Parameters for running a benchmark via the template benchmark runner.
struct BenchmarkParams
{
    std::string_view label;
    const uint8_t thread_count;
    const uint64_t item_count;
    const uint8_t num_prod;
    const uint8_t num_cons;
    std::atomic<uint64_t>& enq_counter;
    std::atomic<uint64_t>& deq_counter;
    std::vector<q_val_t>* deq_items = nullptr;
    std::atomic<bool>* producers_done = nullptr;
    bool is_bbq = false;
    int iteration = -1;
};

/// @brief Pretty-prints the (aggregate) benchmark results to the console.
/// @param res The set of benchmark results to print.
void print_benchmark_results(const BenchmarkResult& res)
{
    auto print_msg = [&](const std::string& msg) { std::cout << "[" << res.label << "] " << msg << std::endl; };
    auto print_err = [&](const std::string& msg) { std::cerr << "[" << res.label << "] " << msg << std::endl; };

    uint64_t total_items = res.num_prod * res.items_per_prod;

    std::cout << std::endl;
    print_msg("========================================================");
    print_msg(" Benchmark Results (Avg.)");
    print_msg("========================================================");
    std::cout << std::endl;
    print_msg("== Information =========================================");
    print_msg(std::format("Number of producers: {}", res.num_prod));
    print_msg(std::format("Number of consumers: {}", res.num_cons));
    print_msg(std::format("Items per producer: {}. Total items: {}", res.items_per_prod, total_items));
    if (res.is_bbq)
        print_msg(
            std::format("Queue mode: {}", res.queue_mode == queues::QueueMode::DROP_OLD ? "DROP OLD" : "RETRY NEW"));

    // Show correctness information only where applicable
    if (!res.is_bbq || (res.is_bbq && res.queue_mode == queues::QueueMode::RETRY_NEW))
    {
        std::cout << std::endl;
        print_msg("== Correctness =========================================");
        print_msg(std::format("Expected items: {}", total_items));
        print_msg(
            std::format("Actual(ENQ): {} | Actual(DEQ): {}", res.enq_count.value_or(0), res.deq_count.value_or(0)));

        if (res.enq_count == res.deq_count)
            print_msg("Success: Matching enqueued and dequeued item counts.");
        else
            print_err("Error: Mismatched enqueued and dequeued item counts!");

        if constexpr (CORR_CHECKS)
        {
            std::unordered_set<q_val_t> seen;
            bool duplicate_found = false;
            bool missing_found = false;

            if (res.deq_items != nullptr)
            {
                for (q_val_t v : *(res.deq_items))
                {
                    auto [it, inserted] = seen.insert(v);
                    if (!inserted)
                    {
                        std::cerr << "[CPLX] Error: Duplicate item detected: " << v << std::endl;
                        duplicate_found = true;
                    }
                }

                for (q_val_t i = 0; i < total_items; ++i)
                {
                    if (seen.find(i) == seen.end())
                    {
                        std::cerr << "[CPLX] Error: Item " << i << " was not found in the dequeued items!" << std::endl;
                        missing_found = true;
                    }
                }

                if (!duplicate_found && !missing_found)
                    std::cout << "[CPLX] Success: All expected items were found exactly once." << std::endl;
                else
                    std::cerr << "[CPLX] Error: Issues detected in dequeued items (see above)." << std::endl;
            }
        }
    }

    std::cout << std::endl;
    print_msg("== Durations ===========================================");
    print_msg(
        std::format("Total elapsed time: {} µs ({:.4f} ms)", res.elapsed_total_us, res.elapsed_total_us / 1000.0));
    print_msg(std::format("Mean producer time: {:.4f} µs ({:.4f} ms)", res.mean_prod_us, res.mean_prod_us / 1000.0));
    print_msg(std::format("Mean consumer time: {:.4f} µs ({:.4f} ms)", res.mean_cons_us, res.mean_cons_us / 1000.0));

    print_msg("== Statistics ==========================================");
    if (!res.enq_count.has_value() || !res.deq_count.has_value())
    {
        print_err("Warning: Enqueue and dequeue counts are not available for throughput calculation!");
        print_msg("Use the complex benchmark mode to get these values.");
    }
    else
    {
        // Check for overflow
        if (res.deq_count.value() > std::numeric_limits<uint64_t>::max() - res.enq_count.value())
        {
            print_err("Error: Overflow in total operations calculation!");
            return;
        }

        uint64_t stl_total_ops = res.enq_count.value() + res.deq_count.value();

        print_msg(std::format("Throughput (TOTAL) = {:e} op/s", stl_total_ops / (res.elapsed_total_us / 1'000'000.0f)));

        double mean_prod_throughput = res.enq_count.value() / (res.mean_prod_us / 1'000'000.0);
        double mean_cons_throughput = res.deq_count.value() / (res.mean_cons_us / 1'000'000.0);

        print_msg(std::format(":: Throughput (PROD) = {:e} op/s", mean_prod_throughput));
        print_msg(std::format(":: Throughput (CONS) = {:e} op/s", mean_cons_throughput));
        print_msg(
            std::format(":: Fairness (MAX/MIN) = {:.6f}", std::max(mean_prod_throughput, mean_cons_throughput) /
                                                              std::min(mean_prod_throughput, mean_cons_throughput)));
    }

    if (res.mean_enq_latency_ns == 0.0 || res.mean_deq_latency_ns == 0.0)
    {
        print_err("Warning: Enqueue and dequeue latencies are not available for latency calculation!");
        print_msg("Use the complex benchmark mode to get these values.");
    }
    else
    {
        print_msg(std::format("Mean enqueue latency (OK): {:.4f} ns/op ({:.4f} µs/op)", res.mean_enq_latency_ns,
                              res.mean_enq_latency_ns / 1000.0));
        print_msg(std::format("Mean enqueue latency (FULL): {:.4f} ns/op ({:.4f} µs/op)", res.mean_enq_full_latency_ns,
                              res.mean_enq_full_latency_ns / 1000.0));
        print_msg(std::format("Mean dequeue latency (OK): {:.4f} ns/op ({:.4f} µs/op)", res.mean_deq_latency_ns,
                              res.mean_deq_latency_ns / 1000.0));
        print_msg(std::format("Mean dequeue latency (EMPTY): {:.4f} ns/op ({:.4f} µs/op)",
                              res.mean_deq_empty_latency_ns, res.mean_deq_empty_latency_ns / 1000.0));
    }

    std::cout << std::endl;
}

/**
 * @brief Runs a single benchmark iteration.
 *
 * @tparam ProdFunc Type of the producer function.
 * @tparam ConsFunc Type of the consumer function.
 * @param params Miscellaneous parameters for the benchmark.
 * @param prod_func The producer function to run in each thread.
 * @param cons_func The consumer function to run in each thread.
 * @return BenchmarkResult The result of the benchmark run, containing various statistics.
 */
template<typename ProdFunc, typename ConsFunc>
BenchmarkResult run_benchmark(const BenchmarkParams& params, ProdFunc prod_func, ConsFunc cons_func)
{
    if (params.num_prod <= 0 || params.num_cons <= 0 || params.item_count == 0)
    {
        std::cerr << "[" << params.label << "] Error: Invalid parameters provided to benchmark runner." << std::endl;
        return {};
    }

    std::vector<std::thread> producers, consumers;
    std::barrier sync_point(params.num_prod + params.num_cons + 1);
    std::vector<uint64_t> prod_times_us(params.num_prod, 0);
    std::vector<uint64_t> cons_times_us(params.num_cons, 0);

    if (params.iteration >= 0)
        std::cout << "[" << params.label << "] Iteration " << params.iteration + 1 << " started." << std::endl;

    // Start producer and consumer threads
    uint8_t prod_idx = 0, cons_idx = 0;
    uint8_t total_threads = params.num_prod + params.num_cons;
    for (uint8_t t = 0; t < total_threads; ++t)
    {
        // Alternate between producer and consumer threads, starting with a producer on CPU 0.
        if ((t % 2 == 0 && prod_idx < params.num_prod) || cons_idx >= params.num_cons) // Producer
        {
            producers.emplace_back(
                [&, prod_idx, t]()
                {
                    set_cpu_affinity(t, params.thread_count);
                    sync_point.arrive_and_wait();
                    auto start = std::chrono::steady_clock::now();
                    prod_func(prod_idx);
                    auto end = std::chrono::steady_clock::now();
                    prod_times_us[prod_idx] =
                        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                });
            ++prod_idx;
        }
        else // Consumer
        {
            consumers.emplace_back(
                [&, cons_idx, t]()
                {
                    set_cpu_affinity(t, params.thread_count);
                    sync_point.arrive_and_wait();
                    auto start = std::chrono::steady_clock::now();
                    cons_func(cons_idx);
                    auto end = std::chrono::steady_clock::now();
                    cons_times_us[cons_idx] =
                        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                });
            ++cons_idx;
        }
    }

    // Give some time for threads to start and then notify them to begin
    sync_point.arrive_and_wait();
    const auto start{std::chrono::steady_clock::now()};

    // Join producer threads
    for (auto& thread : producers)
        thread.join();

    // In drop-old mode, signal to the consumers that producers are done enqueuing items
    if (params.producers_done != nullptr)
        params.producers_done->store(true, std::memory_order_release);

    // Join consumer threads
    for (auto& thread : consumers)
        thread.join();

    const auto finish{std::chrono::steady_clock::now()};
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();

    if (params.iteration >= 0)
        std::cout << "[" << params.label << "] Iteration " << params.iteration + 1 << " completed." << std::endl;

    // Calculate mean producer and consumer times
    uint64_t total_prod_us = 0;
    for (uint8_t i = 0; i < params.num_prod; i++)
        total_prod_us += prod_times_us[i];

    uint64_t total_cons_us = 0;
    for (uint8_t i = 0; i < params.num_cons; i++)
        total_cons_us += cons_times_us[i];

    double mean_prod_us = static_cast<double>(total_prod_us) / params.num_prod;
    double mean_cons_us = static_cast<double>(total_cons_us) / params.num_cons;

    // Read final enqueue/dequeue counts (updated once per thread at the end)
    uint64_t enq_count = params.enq_counter.load(std::memory_order_relaxed);
    uint64_t deq_count = params.deq_counter.load(std::memory_order_relaxed);

    BenchmarkResult res;
    res.label = params.label;
    res.num_prod = params.num_prod;
    res.num_cons = params.num_cons;
    res.items_per_prod = params.item_count / params.num_prod;
    res.enq_count = enq_count != 0 ? std::optional<uint64_t>(enq_count) : std::nullopt;
    res.deq_count = deq_count != 0 ? std::optional<uint64_t>(deq_count) : std::nullopt;
    res.deq_items = params.deq_items;
    res.elapsed_total_us = elapsed_us;
    res.mean_prod_us = mean_prod_us;
    res.mean_cons_us = mean_cons_us;
    res.is_bbq = params.is_bbq;
    res.queue_mode = QUEUE_MODE;

    return res;
}

/**
 * @brief Trims the provided vector by removing a specified percentage of elements
 *
 * @tparam T The type of elements stored in the vector. Must be either integral or floating-point type.
 * @param v The target vector to trim.
 * @param trim_pct The percentage of elements to trim from both ends of the vector.
 */
template<typename T, typename = std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>>>
void trim_vector(std::vector<T>& v, double trim_pct)
{
    if (v.empty())
        return;

    if (trim_pct <= 0.0 || trim_pct >= 0.5)
    {
        std::cout << "Warning: The provided trim percentage is out of bounds (0.0 <= N <= 0.5). "
                     "Skipping trimming."
                  << std::endl;
    }

    std::sort(v.begin(), v.end());
    std::size_t trim_num = static_cast<std::size_t>(std::ceil(v.size() * trim_pct));
    if (trim_num * 2 >= v.size())
    {
        std::cout << "Warning: The number of elements to trim is too large. "
                     "Skipping trimming."
                  << std::endl;
        return;
    }

    v.erase(v.begin(), v.begin() + trim_num);
    v.erase(v.end() - trim_num, v.end());
}

/**
 * @brief Calculates the mean of the provided vector.
 *
 * @tparam T The type of elements stored in the vector. Must be either integral or floating-point type.
 * @param v The target vector to calculate the mean of.
 * @return double The resulting mean value.
 */
template<typename T, typename = std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>>>
double calculate_mean(const std::vector<T>& v)
{
    if (v.empty())
        return 0.0;

    T sum = std::accumulate(v.begin(), v.end(), static_cast<T>(0));
    return static_cast<double>(sum) / v.size();
}

/**
 * @brief Trims and aggregates results multiple benchmark runs.
 *
 * @param results The vector of BenchmarkResult objects to trim and aggregate.
 * @return BenchmarkResult The formatted result.
 */
BenchmarkResult aggregate_results(const std::vector<BenchmarkResult>& results)
{
    if (results.empty())
    {
        std::cerr << "Error: No results to aggregate." << std::endl;
        return {};
    }

    std::cout << "Aggregating results from " << results.size() << " iterations..." << std::endl;

    // Move data to separate vector arrays to simplify trimming
    std::vector<uint64_t> elapsed_times_us, prod_times_us, cons_times_us;
    std::vector<uint64_t> enq_counts, deq_counts;
    std::vector<uint64_t> enq_latency_ns, enq_full_latency_ns;
    std::vector<uint64_t> deq_latency_ns, deq_empty_latency_ns;

    for (const auto& r : results)
    {
        elapsed_times_us.push_back(r.elapsed_total_us);
        prod_times_us.push_back(r.mean_prod_us);
        cons_times_us.push_back(r.mean_cons_us);

        if (r.enq_count.has_value())
            enq_counts.push_back(r.enq_count.value());

        if (r.deq_count.has_value())
            deq_counts.push_back(r.deq_count.value());

        enq_latency_ns.push_back(r.mean_enq_latency_ns);
        enq_full_latency_ns.push_back(r.mean_enq_full_latency_ns);
        deq_latency_ns.push_back(r.mean_deq_latency_ns);
        deq_empty_latency_ns.push_back(r.mean_deq_empty_latency_ns);
    }

    // Trim the results to remove outliers
    std::cout << "Trimming results by " << (TRIM_PERCENTAGE * 100.0) << "% from both ends to remove outliers..."
              << std::endl;
    trim_vector(elapsed_times_us, TRIM_PERCENTAGE);
    trim_vector(prod_times_us, TRIM_PERCENTAGE);
    trim_vector(cons_times_us, TRIM_PERCENTAGE);
    trim_vector(enq_counts, TRIM_PERCENTAGE);
    trim_vector(deq_counts, TRIM_PERCENTAGE);
    trim_vector(enq_latency_ns, TRIM_PERCENTAGE);
    trim_vector(enq_full_latency_ns, TRIM_PERCENTAGE);
    trim_vector(deq_latency_ns, TRIM_PERCENTAGE);
    trim_vector(deq_empty_latency_ns, TRIM_PERCENTAGE);

    // Calculate the mean of the trimmed results
    BenchmarkResult agg = results.front();
    agg.elapsed_total_us = static_cast<uint64_t>(calculate_mean(elapsed_times_us));
    agg.mean_prod_us = calculate_mean(prod_times_us);
    agg.mean_cons_us = calculate_mean(cons_times_us);
    agg.enq_count = enq_counts.empty() ? std::nullopt : std::optional<uint64_t>(calculate_mean(enq_counts));
    agg.deq_count = deq_counts.empty() ? std::nullopt : std::optional<uint64_t>(calculate_mean(deq_counts));
    agg.mean_enq_latency_ns = calculate_mean(enq_latency_ns);
    agg.mean_enq_full_latency_ns = calculate_mean(enq_full_latency_ns);
    agg.mean_deq_latency_ns = calculate_mean(deq_latency_ns);
    agg.mean_deq_empty_latency_ns = calculate_mean(deq_empty_latency_ns);

    return agg;
}

// Note: This is based on the simple SPSC benchmark outlined in the academic paper.
// The main purpose of this benchmark is to determine the raw performance of the queue.
void run_simple_benchmark(const uint32_t iters, const uint8_t thread_count)
{
    std::atomic<uint64_t> enq_counter{0};
    std::atomic<uint64_t> deq_counter{0};
    std::atomic<bool> producer_done{false};

    // In retry-new mode, we can calculate the throughput
    if constexpr (QUEUE_MODE == queues::QueueMode::RETRY_NEW)
    {
        enq_counter.store(BENCHMARK_ITEM_COUNT, std::memory_order_relaxed);
        deq_counter.store(BENCHMARK_ITEM_COUNT, std::memory_order_relaxed);
    }

    BenchmarkParams params{.label = "SMPL",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = 1,
                           .num_cons = 1,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
                           .deq_items = nullptr,
                           .producers_done = &producer_done,
                           .is_bbq = true,
                           .iteration = -1};

    std::cout << "[SMPL] == Logs ================================================" << std::endl;

    std::vector<BenchmarkResult> results;
    for (uint32_t i = 0; i < iters; ++i)
    {
        queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> bbq(BBQ_NUM_BLOCKS, BBQ_ENTRIES_PER_BLOCK);
        producer_done.store(false, std::memory_order_release);
        params.iteration = i;

        BenchmarkResult res = run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                uint64_t i = 0;
                while (i < BENCHMARK_ITEM_COUNT)
                {
                    if (bbq.enqueue(i) == queues::OpStatus::OK)
                    {
                        ++i;
                        continue;
                    }

                    std::this_thread::yield();
                }
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                std::pair<std::optional<uint64_t>, queues::OpStatus> buf;
                if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
                {
                    while (true)
                    {
                        buf = bbq.dequeue();

                        if constexpr (CORR_CHECKS)
                        {
                            if (buf.second == queues::OpStatus::OK)
                            {
                                if (!buf.first.has_value()) [[unlikely]]
                                    abort();
                            }
                        }

                        if (buf.second == queues::OpStatus::EMPTY && producer_done.load(std::memory_order_acquire))
                            return nullptr;
                    }
                }
                else
                {
                    uint64_t i = 0;
                    while (i < BENCHMARK_ITEM_COUNT)
                    {
                        buf = bbq.dequeue();
                        if (buf.second == queues::OpStatus::OK)
                        {
                            if constexpr (CORR_CHECKS)
                            {
                                // Verify that the dequeued value matches the expected value
                                if (!buf.first.has_value() || buf.first.value() != i) [[unlikely]]
                                    abort();
                            }

                            ++i;
                        }
                    }
                }
            });

        results.emplace_back(std::move(res));
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr);
}

void run_complex_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                           const uint8_t thread_count)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    if constexpr (CORR_CHECKS)
        deq_items.reserve(BENCHMARK_ITEM_COUNT);

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "CPLX",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
                           .deq_items = &deq_items,
                           .producers_done = &producers_done,
                           .is_bbq = true,
                           .iteration = -1};

    std::cout << "[CPLX] == Logs ================================================" << std::endl;
    std::cout << "[CPLX] Running throughput benchmarks..." << std::endl;

    std::vector<BenchmarkResult> results;
    for (uint32_t i = 0; i < iters; ++i)
    {
        queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> bbq(BBQ_NUM_BLOCKS, BBQ_ENTRIES_PER_BLOCK);
        producers_done.store(false, std::memory_order_release);
        enq_counter.store(0, std::memory_order_release);
        deq_counter.store(0, std::memory_order_release);

        params.iteration = i;

        res = run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                for (uint64_t j = 0; j < items_per_prod; j++)
                {
                    q_val_t item_val = (thread_idx * items_per_prod) + j;
                    while (bbq.enqueue(item_val) != queues::OpStatus::OK)
                        std::this_thread::yield();

                    enq_counter.fetch_add(1, std::memory_order_relaxed);
                }
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                std::pair<std::optional<uint64_t>, queues::OpStatus> buf;

                if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
                {
                    while (true)
                    {
                        buf = bbq.dequeue();
                        if (buf.second == queues::OpStatus::OK)
                        {
                            if constexpr (CORR_CHECKS)
                            {
                                if (!buf.first.has_value())
                                    abort();
                            }

                            if constexpr (CORR_CHECKS)
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(buf.first.value());
                            }

                            deq_counter.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (buf.second == queues::OpStatus::EMPTY)
                        {
                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty
                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                }
                else
                {
                    while (deq_counter.load(std::memory_order_acquire) < BENCHMARK_ITEM_COUNT)
                    {
                        buf = bbq.dequeue();
                        if (buf.second == queues::OpStatus::OK)
                        {
                            if constexpr (CORR_CHECKS)
                            {
                                if (!buf.first.has_value())
                                    abort(); // This should never happen in a well-formed queue
                            }

                            if constexpr (CORR_CHECKS)
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(buf.first.value());
                            }

                            deq_counter.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                }
            });

        results.emplace_back(std::move(res));
    }

    std::cout << "[CPLX] Running latency benchmarks..." << std::endl;

    std::mutex op_lat_mutex;
    std::mutex op_lat_full_mutex;
    std::mutex op_lat_empty_mutex;

    for (uint32_t i = 0; i < iters; ++i)
    {
        queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> bbq(BBQ_NUM_BLOCKS, BBQ_ENTRIES_PER_BLOCK);
        deq_counter.store(0, std::memory_order_release);
        producers_done.store(false, std::memory_order_release);

        std::vector<uint64_t> enq_latencies_ns(BENCHMARK_ITEM_COUNT, 0);
        std::vector<uint64_t> deq_latencies_ns;
        deq_latencies_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> enq_latencies_full_ns;
        enq_latencies_full_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> deq_latencies_empty_ns;
        deq_latencies_empty_ns.reserve(BENCHMARK_ITEM_COUNT);

        params.iteration = i;

        run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                queues::OpStatus status;
                uint64_t j = 0;
                while (j < items_per_prod)
                {
                    q_val_t item_val = (thread_idx * items_per_prod) + j;
                    auto start = std::chrono::steady_clock::now();
                    status = bbq.enqueue(item_val);
                    auto end = std::chrono::steady_clock::now();

                    if (status == queues::OpStatus::OK)
                    {
                        enq_latencies_ns[item_val] =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                        j++;
                    }
                    else if (status == queues::OpStatus::FULL)
                    {
                        std::scoped_lock lock(op_lat_full_mutex);
                        enq_latencies_full_ns.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

                        std::this_thread::yield();
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                std::pair<std::optional<uint64_t>, queues::OpStatus> buf;

                if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
                {
                    while (true)
                    {
                        auto start = std::chrono::steady_clock::now();
                        buf = bbq.dequeue();
                        auto end = std::chrono::steady_clock::now();
                        if (buf.second == queues::OpStatus::OK)
                        {
                            std::scoped_lock lock(op_lat_mutex);
                            deq_latencies_ns.push_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }
                        else if (buf.second == queues::OpStatus::EMPTY)
                        {
                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty

                            {
                                std::scoped_lock lock(op_lat_empty_mutex);
                                deq_latencies_empty_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                }
                else
                {
                    while (deq_counter.load(std::memory_order_acquire) < BENCHMARK_ITEM_COUNT)
                    {
                        auto start = std::chrono::steady_clock::now();
                        buf = bbq.dequeue();
                        auto end = std::chrono::steady_clock::now();
                        if (buf.second == queues::OpStatus::OK)
                        {
                            {
                                std::scoped_lock lock(op_lat_mutex);
                                deq_latencies_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            deq_counter.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (buf.second == queues::OpStatus::EMPTY)
                        {
                            {
                                std::scoped_lock lock(op_lat_empty_mutex);
                                deq_latencies_empty_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                }
            });

        if (i < results.size())
        {
            results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
            results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
            results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
        }
        else
        {
            std::cout << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                      << std::endl;
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr);
}

void run_stl_queue_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                             const uint8_t thread_count)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    std::mutex stl_mtx;
    std::condition_variable stl_cv;
    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;
    alignas(CACHELINE_SIZE) BenchmarkResult res;

    BenchmarkParams params{.label = "STL",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
                           .deq_items = nullptr,
                           .producers_done = nullptr,
                           .iteration = -1};

    std::cout << "[STL] == Logs ================================================" << std::endl;
    std::cout << "[STL] Running throughput benchmarks..." << std::endl;

    std::vector<BenchmarkResult> results;
    for (uint32_t i = 0; i < iters; ++i)
    {
        std::queue<q_val_t> stl_queue;
        enq_counter.store(0, std::memory_order_release);
        deq_counter.store(0, std::memory_order_release);

        params.iteration = i;

        res = run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                for (uint32_t j = 0; j < items_per_prod; j++)
                {
                    q_val_t item_val = (thread_idx * items_per_prod) + j;
                    {
                        std::unique_lock<std::mutex> lock(stl_mtx);
                        stl_queue.push(item_val);
                    }
                    enq_counter.fetch_add(1, std::memory_order_relaxed);
                    stl_cv.notify_one();
                }
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                while (deq_counter.load(std::memory_order_relaxed) < BENCHMARK_ITEM_COUNT)
                {
                    std::unique_lock<std::mutex> lock(stl_mtx);
                    stl_cv.wait(lock,
                                [&]
                                {
                                    return !stl_queue.empty() ||
                                           deq_counter.load(std::memory_order_relaxed) >= BENCHMARK_ITEM_COUNT;
                                });
                    if (!stl_queue.empty())
                    {
                        stl_queue.pop();
                        deq_counter.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });

        results.emplace_back(std::move(res));
    }

    std::cout << "[STL] Running latency benchmarks..." << std::endl;

    std::mutex op_lat_mutex;
    std::mutex op_lat_full_mutex;
    std::mutex op_lat_empty_mutex;

    for (uint32_t i = 0; i < iters; ++i)
    {
        std::queue<q_val_t> stl_queue;
        deq_counter.store(0, std::memory_order_release);

        std::vector<uint64_t> enq_latencies_ns(BENCHMARK_ITEM_COUNT, 0);
        std::vector<uint64_t> deq_latencies_ns;
        deq_latencies_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> deq_latencies_empty_ns;
        deq_latencies_empty_ns.reserve(BENCHMARK_ITEM_COUNT);

        params.iteration = i;

        run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                for (uint32_t j = 0; j < items_per_prod; j++)
                {
                    q_val_t item_val = (thread_idx * items_per_prod) + j;
                    auto start = std::chrono::steady_clock::now();
                    {
                        std::unique_lock<std::mutex> lock(stl_mtx);
                        stl_queue.push(item_val);
                    }
                    auto end = std::chrono::steady_clock::now();
                    enq_latencies_ns[item_val] =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

                    stl_cv.notify_one();
                }
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                while (deq_counter.load(std::memory_order_relaxed) < BENCHMARK_ITEM_COUNT)
                {
                    std::unique_lock<std::mutex> lock(stl_mtx);
                    stl_cv.wait(lock,
                                [&]
                                {
                                    return !stl_queue.empty() ||
                                           deq_counter.load(std::memory_order_relaxed) >= BENCHMARK_ITEM_COUNT;
                                });

                    auto start = std::chrono::steady_clock::now();
                    if (!stl_queue.empty())
                    {
                        stl_queue.pop();
                        auto end = std::chrono::steady_clock::now();
                        deq_latencies_ns.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        deq_counter.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        auto end = std::chrono::steady_clock::now();
                        deq_latencies_empty_ns.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                }
            });

        if (i < results.size())
        {
            results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
            results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
        }
        else
        {
            std::cout << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                      << std::endl;
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr);
}

// Based on example from Boost documentation:
// https://www.boost.org/doc/libs/master/doc/html/lockfree/examples.html
void run_boost_lockfree_queue_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                                        const uint8_t thread_count, bool disable_optimized_spsc = false)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    if constexpr (CORR_CHECKS)
        deq_items.reserve(BENCHMARK_ITEM_COUNT);

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "BST-LFQ",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
                           .deq_items = &deq_items,
                           .producers_done = &producers_done,
                           .iteration = -1};

    // Note(s):
    // - Boost::lockfree: freelist size is limited to a maximum of 65535 objects.
    // - Boost is the only queue allocated on the heap as it triggers a stack overflow if an attempt is made
    //   to allocatea queue on the stack. This is likely because the other queues internally allocate on the
    //   heap whereas boost tries to fully allocated on the stack.
    constexpr std::size_t Q_CAPACITY = std::min(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK,
                                                static_cast<uint64_t>(std::numeric_limits<uint16_t>::max() - 1));

    std::cout << "[BST-LFQ] == Logs ================================================" << std::endl;

    if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        std::cout << "[BST-LFQ] Using wait-free single-producer/single-consumer (SPSC) queue." << std::endl;
    else
        std::cout << "[BST-LFQ] Using lock-free multi-producer/multi-consumer (MPMC) queue." << std::endl;

    std::cout << "[BST-LFQ] Running throughput benchmarks..." << std::endl;

    std::vector<BenchmarkResult> results;
    for (uint32_t i = 0; i < iters; ++i)
    {
        producers_done.store(false, std::memory_order_release);
        enq_counter.store(0, std::memory_order_release);
        deq_counter.store(0, std::memory_order_release);

        params.iteration = i;

        if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        {
            auto boost_spsc_q =
                std::make_unique<boost::lockfree::spsc_queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    for (uint64_t j = 0; j < items_per_prod; j++)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        while (!boost_spsc_q->push(item_val))
                            std::this_thread::yield();

                        enq_counter.fetch_add(1, std::memory_order_relaxed);
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    while (true)
                    {
                        while (boost_spsc_q->pop(item_val))
                        {
                            if constexpr (CORR_CHECKS)
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }

                            deq_counter.fetch_add(1, std::memory_order_relaxed);
                        }

                        if (boost_spsc_q->empty() && producers_done.load(std::memory_order_acquire))
                            break;

                        std::this_thread::yield();
                    }
                });
        }
        else
        {
            auto boost_q = std::make_unique<boost::lockfree::queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    for (uint64_t j = 0; j < items_per_prod; j++)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        while (!boost_q->push(item_val))
                            std::this_thread::yield();

                        enq_counter.fetch_add(1, std::memory_order_relaxed);
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    while (true)
                    {
                        while (boost_q->pop(item_val))
                        {
                            if constexpr (CORR_CHECKS)
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }

                            deq_counter.fetch_add(1, std::memory_order_relaxed);
                        }

                        if (boost_q->empty() && producers_done.load(std::memory_order_acquire))
                            break;

                        std::this_thread::yield();
                    }
                });
        }

        results.emplace_back(std::move(res));
    }

    std::cout << "[BST-LFQ] Running latency benchmarks..." << std::endl;

    std::mutex op_lat_mutex;
    std::mutex op_lat_full_mutex;
    std::mutex op_lat_empty_mutex;

    for (uint32_t i = 0; i < iters; ++i)
    {
        producers_done.store(false, std::memory_order_release);

        std::vector<uint64_t> enq_latencies_ns(BENCHMARK_ITEM_COUNT, 0);
        std::vector<uint64_t> deq_latencies_ns;
        deq_latencies_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> enq_latencies_full_ns;
        enq_latencies_full_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> deq_latencies_empty_ns;
        deq_latencies_empty_ns.reserve(BENCHMARK_ITEM_COUNT);

        params.iteration = i;

        if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        {
            auto boost_spsc_q =
                std::make_unique<boost::lockfree::spsc_queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

            run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    bool success = false;
                    uint64_t j = 0;
                    while (j < items_per_prod)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        auto start = std::chrono::steady_clock::now();
                        success = boost_spsc_q->push(item_val);
                        auto end = std::chrono::steady_clock::now();

                        if (success)
                        {
                            enq_latencies_ns[item_val] =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                            j++;
                        }
                        else
                        {
                            {
                                std::scoped_lock lock(op_lat_full_mutex);
                                enq_latencies_full_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            std::this_thread::yield();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    bool success = false;
                    while (true)
                    {
                        auto start = std::chrono::steady_clock::now();
                        success = boost_spsc_q->pop(item_val);
                        auto end = std::chrono::steady_clock::now();
                        if (success)
                        {
                            std::scoped_lock lock(op_lat_mutex);
                            deq_latencies_ns.push_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }
                        else
                        {
                            if (boost_spsc_q->empty())
                            {
                                {
                                    std::scoped_lock lock(op_lat_empty_mutex);
                                    deq_latencies_empty_ns.push_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }

                                if (producers_done.load(std::memory_order_acquire))
                                    break;
                            }

                            std::this_thread::yield();
                        }
                    }
                });
        }
        else
        {
            auto boost_q = std::make_unique<boost::lockfree::queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

            run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    bool success = false;
                    uint64_t j = 0;
                    while (j < items_per_prod)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        auto start = std::chrono::steady_clock::now();
                        success = boost_q->push(item_val);
                        auto end = std::chrono::steady_clock::now();

                        if (success)
                        {
                            enq_latencies_ns[item_val] =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                            j++;
                        }
                        else
                        {
                            {
                                std::scoped_lock lock(op_lat_full_mutex);
                                enq_latencies_full_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            std::this_thread::yield();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    bool success = false;
                    while (true)
                    {
                        auto start = std::chrono::steady_clock::now();
                        success = boost_q->pop(item_val);
                        auto end = std::chrono::steady_clock::now();
                        if (success)
                        {
                            std::scoped_lock lock(op_lat_mutex);
                            deq_latencies_ns.push_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }
                        else
                        {
                            if (boost_q->empty())
                            {
                                {
                                    std::scoped_lock lock(op_lat_empty_mutex);
                                    deq_latencies_empty_ns.push_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }

                                if (producers_done.load(std::memory_order_acquire))
                                    break;
                            }

                            std::this_thread::yield();
                        }
                    }
                });
        }

        if (i < results.size())
        {
            results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
            results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
            results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
        }
        else
        {
            std::cout << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                      << std::endl;
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr);
}

// https://www.boost.org/doc/libs/1_88_0/doc/html/thread/sds.html
void run_boost_sync_bounded_queue_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                                            const uint8_t thread_count)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    if constexpr (CORR_CHECKS)
        deq_items.reserve(BENCHMARK_ITEM_COUNT);

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "BST-BSYNCQ",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
                           .deq_items = &deq_items,
                           .producers_done = &producers_done,
                           .iteration = -1};

    std::cout << "[BST-BSYNCQ] == Logs ================================================" << std::endl;
    std::cout << "[BST-BSYNCQ] Running throughput benchmarks..." << std::endl;

    std::vector<BenchmarkResult> results;
    for (uint32_t i = 0; i < iters; ++i)
    {
        producers_done.store(false, std::memory_order_release);
        enq_counter.store(0, std::memory_order_release);
        deq_counter.store(0, std::memory_order_release);

        params.iteration = i;

        boost::concurrent::sync_bounded_queue<q_val_t> boost_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);

        res = run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                boost::concurrent::queue_op_status status;
                uint64_t j = 0;
                while (j < items_per_prod)
                {
                    q_val_t item_val = (thread_idx * items_per_prod) + j;
                    status = boost_q.try_push_back(item_val);
                    if (status == boost::concurrent::queue_op_status::success)
                    {
                        enq_counter.fetch_add(1, std::memory_order_relaxed);
                        j++;
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                q_val_t item_val;
                boost::concurrent::queue_op_status status;
                while (true)
                {
                    status = boost_q.try_pull_front(item_val);
                    if (status == boost::concurrent::queue_op_status::success)
                    {
                        if constexpr (CORR_CHECKS)
                        {
                            std::scoped_lock lock(queue_mutex);
                            deq_items.emplace_back(item_val);
                        }

                        deq_counter.fetch_add(1, std::memory_order_relaxed);
                    }
                    else if (status == boost::concurrent::queue_op_status::empty)
                    {
                        if (producers_done.load(std::memory_order_acquire))
                            break; // Exit: producers are done and queue is empty

                        std::this_thread::yield();
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
            });

        results.emplace_back(std::move(res));
    }

    std::cout << "[BST-BSYNCQ] Running latency benchmarks..." << std::endl;

    std::mutex op_lat_mutex;
    std::mutex op_lat_full_mutex;
    std::mutex op_lat_empty_mutex;

    for (uint32_t i = 0; i < iters; ++i)
    {
        producers_done.store(false, std::memory_order_release);

        std::vector<uint64_t> enq_latencies_ns(BENCHMARK_ITEM_COUNT, 0);
        std::vector<uint64_t> deq_latencies_ns;
        deq_latencies_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> enq_latencies_full_ns;
        enq_latencies_full_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> deq_latencies_empty_ns;
        deq_latencies_empty_ns.reserve(BENCHMARK_ITEM_COUNT);

        params.iteration = i;

        boost::concurrent::sync_bounded_queue<q_val_t> boost_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);

        run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                boost::concurrent::queue_op_status status;
                uint64_t j = 0;
                while (j < items_per_prod)
                {
                    q_val_t item_val = (thread_idx * items_per_prod) + j;
                    auto start = std::chrono::steady_clock::now();
                    status = boost_q.try_push_back(item_val);
                    auto end = std::chrono::steady_clock::now();
                    if (status == boost::concurrent::queue_op_status::success)
                    {
                        enq_latencies_ns[item_val] =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                        j++;
                    }
                    else if (status == boost::concurrent::queue_op_status::full)
                    {
                        {
                            std::scoped_lock lock(op_lat_full_mutex);
                            enq_latencies_full_ns.push_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }

                        std::this_thread::yield();
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                q_val_t item_val;
                boost::concurrent::queue_op_status status;
                while (true)
                {
                    auto start = std::chrono::steady_clock::now();
                    status = boost_q.try_pull_front(item_val);
                    auto end = std::chrono::steady_clock::now();
                    if (status == boost::concurrent::queue_op_status::success)
                    {
                        std::scoped_lock lock(op_lat_mutex);
                        deq_latencies_ns.push_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else if (status == boost::concurrent::queue_op_status::empty)
                    {
                        {
                            std::scoped_lock lock(op_lat_empty_mutex);
                            deq_latencies_empty_ns.push_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }

                        if (producers_done.load(std::memory_order_acquire))
                            break; // Exit: producers are done and queue is empty

                        std::this_thread::yield();
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
            });

        if (i < results.size())
        {
            results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
            results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
            results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
        }
        else
        {
            std::cout << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                      << std::endl;
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr);
}

#ifdef ENABLE_FOLLY_BENCHMARKS
void run_follyq_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                          const uint8_t thread_count, bool disable_optimized_spsc = false)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    if constexpr (CORR_CHECKS)
        deq_items.reserve(BENCHMARK_ITEM_COUNT);

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "FOLLYQ",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
                           .deq_items = &deq_items,
                           .producers_done = &producers_done,
                           .iteration = -1};

    std::cout << "[FOLLYQ] == Logs ================================================" << std::endl;

    if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        std::cout << "[FOLLYQ] Using single-producer/single-consumer ProducerConsumerQueue." << std::endl;
    else
        std::cout << "[FOLLYQ] Using multi-producer/multi-consumer MPMCQueue." << std::endl;

    std::cout << "[FOLLYQ] Running throughput benchmarks..." << std::endl;

    std::vector<BenchmarkResult> results;
    for (uint32_t i = 0; i < iters; ++i)
    {
        producers_done.store(false, std::memory_order_release);
        enq_counter.store(0, std::memory_order_release);
        deq_counter.store(0, std::memory_order_release);

        params.iteration = i;

        if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        {
            folly::ProducerConsumerQueue<q_val_t> folly_q{BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK};

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    bool success;
                    uint64_t j = 0;
                    while (j < items_per_prod)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        success = folly_q.write(std::move(item_val));
                        if (success)
                        {
                            enq_counter.fetch_add(1, std::memory_order_relaxed);
                            j++;
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    bool success;
                    while (true)
                    {
                        success = folly_q.read(item_val);
                        if (success)
                        {
                            if constexpr (CORR_CHECKS)
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }

                            deq_counter.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (folly_q.isEmpty())
                        {
                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                });
        }
        else
        {
            folly::MPMCQueue<q_val_t, std::atomic, false> folly_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    bool success;
                    uint64_t j = 0;
                    while (j < items_per_prod)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        success = folly_q.write(std::move(item_val));
                        if (success)
                        {
                            enq_counter.fetch_add(1, std::memory_order_relaxed);
                            j++;
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    bool success;
                    while (true)
                    {
                        success = folly_q.read(item_val);
                        if (success)
                        {
                            if constexpr (CORR_CHECKS)
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }

                            deq_counter.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (folly_q.isEmpty())
                        {
                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                });
        }

        results.emplace_back(std::move(res));
    }

    std::cout << "[FOLLYQ] Running latency benchmarks..." << std::endl;

    std::mutex op_lat_mutex;
    std::mutex op_lat_full_mutex;
    std::mutex op_lat_empty_mutex;

    for (uint32_t i = 0; i < iters; ++i)
    {
        producers_done.store(false, std::memory_order_release);

        std::vector<uint64_t> enq_latencies_ns(BENCHMARK_ITEM_COUNT, 0);
        std::vector<uint64_t> deq_latencies_ns;
        deq_latencies_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> enq_latencies_full_ns;
        enq_latencies_full_ns.reserve(BENCHMARK_ITEM_COUNT);
        std::vector<uint64_t> deq_latencies_empty_ns;
        deq_latencies_empty_ns.reserve(BENCHMARK_ITEM_COUNT);

        params.iteration = i;

        if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        {
            folly::ProducerConsumerQueue<q_val_t> folly_q{BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK};

            run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    bool success;
                    uint64_t j = 0;
                    while (j < items_per_prod)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        auto start = std::chrono::steady_clock::now();
                        success = folly_q.write(std::move(item_val));
                        auto end = std::chrono::steady_clock::now();
                        if (success)
                        {
                            enq_latencies_ns[item_val] =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                            j++;
                        }
                        else if (folly_q.isFull())
                        {
                            {
                                std::scoped_lock lock(op_lat_full_mutex);
                                enq_latencies_full_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    bool success;
                    while (true)
                    {
                        auto start = std::chrono::steady_clock::now();
                        success = folly_q.read(item_val);
                        auto end = std::chrono::steady_clock::now();
                        if (success)
                        {
                            std::scoped_lock lock(op_lat_mutex);
                            deq_latencies_ns.push_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }
                        else if (folly_q.isEmpty())
                        {
                            {
                                std::scoped_lock lock(op_lat_empty_mutex);
                                deq_latencies_empty_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                });
        }
        else
        {
            folly::MPMCQueue<q_val_t, std::atomic, false> folly_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);

            run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    bool success;
                    uint64_t j = 0;
                    while (j < items_per_prod)
                    {
                        q_val_t item_val = (thread_idx * items_per_prod) + j;
                        auto start = std::chrono::steady_clock::now();
                        success = folly_q.write(std::move(item_val));
                        auto end = std::chrono::steady_clock::now();
                        if (success)
                        {
                            enq_latencies_ns[item_val] =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                            j++;
                        }
                        else if (folly_q.isFull())
                        {
                            {
                                std::scoped_lock lock(op_lat_full_mutex);
                                enq_latencies_full_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    bool success;
                    while (true)
                    {
                        auto start = std::chrono::steady_clock::now();
                        success = folly_q.read(item_val);
                        auto end = std::chrono::steady_clock::now();
                        if (success)
                        {
                            std::scoped_lock lock(op_lat_mutex);
                            deq_latencies_ns.push_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }
                        else if (folly_q.isEmpty())
                        {
                            {
                                std::scoped_lock lock(op_lat_empty_mutex);
                                deq_latencies_empty_ns.push_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }

                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty

                            std::this_thread::yield();
                        }
                        else
                        {
                            std::this_thread::yield();
                        }
                    }
                });
        }

        if (i < results.size())
        {
            results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
            results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
            results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
        }
        else
        {
            std::cout << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                      << std::endl;
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr);
}
#endif

enum class TestMode
{
    SIMPLE,
    COMPLEX,
    STL,
    BOOST_LFQ,
    BOOST_SYNC_BOUNDED,
#ifdef ENABLE_FOLLY_BENCHMARKS
    FOLLYQ,
#endif
    UNKNOWN
};

template<typename T>
bool parse_arg(int argc, char** argv, int& i, T& out, T min_val = 1, T max_val = std::numeric_limits<T>::max())
{
    if (i + 1 < argc && std::isdigit(argv[i + 1][0]))
    {
        T val = static_cast<T>(std::atoi(argv[++i]));
        if (val < min_val)
            val = min_val;

        if (val > max_val)
            val = max_val;

        out = val;
        return true;
    }
    return false;
}

TestMode parse_mode(const std::string& mode)
{
    std::string lower_mode = mode;
    std::transform(lower_mode.begin(), lower_mode.end(), lower_mode.begin(), ::tolower);

    if (lower_mode == "simple")
        return TestMode::SIMPLE;
    if (lower_mode == "complex")
        return TestMode::COMPLEX;
    if (lower_mode == "stl")
        return TestMode::STL;
    if (lower_mode == "boost-lfq")
        return TestMode::BOOST_LFQ;
    if (lower_mode == "boost-bsyncq")
        return TestMode::BOOST_SYNC_BOUNDED;
#ifdef ENABLE_FOLLY_BENCHMARKS
    if (lower_mode == "follyq")
        return TestMode::FOLLYQ;
#endif

    return TestMode::UNKNOWN;
}

// clang-format off
void print_help(const std::string& program_name)
{
    std::cout << "Usage: " << program_name << " simple|complex|stl|boost-lfq|boost-bsyncq|... [opts]" << std::endl;
    std::cout << std::endl << "Modes:" << std::endl;
    std::cout << "    simple          Run a minimal single-producer/single-consumer (SPSC) benchmark" << std::endl;
    std::cout << "                    on the block-based queue. The producer/consumer/thread options" << std::endl;
    std::cout << "                    are ignored. This mode always uses 1 producer and 1 consumer." << std::endl;
    std::cout << "    complex         Run a configurable multi-producer/multi-consumer benchmark" << std::endl;
    std::cout << "                    on the block-based queue. All options are supported." << std::endl;
    std::cout << "    stl             Run the same configurable benchmark as 'complex', but using" << std::endl;
    std::cout << "                    std::queue for comparison." << std::endl;
    std::cout << "    boost-lfq       Run a configurable multi-producer/multi-consumer benchmark" << std::endl;
    std::cout << "                    on the Boost lockfree queue. All options are supported." << std::endl;
    std::cout << "    boost-bsyncq    Run a configurable multi-producer/multi-consumer benchmark" << std::endl;
    std::cout << "                    on the Boost sync_bounded_queue. All options are supported." << std::endl;
    #ifdef ENABLE_FOLLY_BENCHMARKS
    std::cout << "    follyq          Run a configurable multi-producer/multi-consumer benchmark" << std::endl;
    std::cout << "                    on the Folly MPMC queue. All options are supported." << std::endl;
    #endif
    std::cout << std::endl << "Options:" << std::endl;
    std::cout << "    -h, --help                 Show this help message.\n" << std::endl;
    std::cout << "    -p <N>, --producers <N>    Set the number of producers (default: 1)." << std::endl;
    std::cout << "    -c <N>, --consumers <N>    Set the number of consumers (default: 1)." << std::endl;
    std::cout << "    -i <N>, --iterations <N>   Set the number of iterations to run (default: 1)." << std::endl;
    std::cout << "    -t <N>, --threads <N>      Set the number of threads to use (default: 2)." << std::endl;
    std::cout << "    --disable-optimized-spsc   Disable the use of SPSC queue variants." << std::endl;
    std::cout << "                               Some libraries offer faster SPSC queue data" << std::endl;
    std::cout << "                               structures, which may skew results." << std::endl;
    std::cout << "                               Use this option to disable their use." << std::endl;
}
// clang-format on

int main(int argc, char** argv)
{
    std::string benchmark_mode;
    uint8_t num_prod = 1;
    uint8_t num_cons = 1;
    uint32_t num_iter = 1;
    uint8_t num_threads = 2;
    bool show_help = false;
    bool disable_optimized_spsc = false;

    // Parse command line args
    // Note(s):
    // - The number-based options use isdigit, which implicitly filters out negative numbers.
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help")
        {
            show_help = true;
            break;
        }
        else if (arg == "-p" || arg == "--producers")
        {
            if (!parse_arg(argc, argv, i, num_prod))
            {
                std::cerr << "Error: Invalid number of producers specified." << std::endl;
                return 1;
            }

            if (num_prod == 0)
            {
                std::cout << "Warning: Number of producers set to 0. Defaulting to 1." << std::endl;
                num_prod = 1; // Ensure at least one producer
            }
        }
        else if (arg == "-c" || arg == "--consumers")
        {
            if (!parse_arg(argc, argv, i, num_cons))
            {
                std::cerr << "Error: Invalid number of consumers specified." << std::endl;
                return 1;
            }

            if (num_cons == 0)
            {
                std::cout << "Warning: Number of consumers set to 0. Defaulting to 1." << std::endl;
                num_cons = 1; // Ensure at least one consumer
            }
        }
        else if (arg == "-i" || arg == "--iterations")
        {
            if (!parse_arg(argc, argv, i, num_iter) && num_iter == 0)
            {
                std::cerr << "Error: Invalid number of iterations specified." << std::endl;
                return 1;
            }

            if (num_iter == 0)
            {
                std::cout << "Warning: Number of iterations set to 0. Defaulting to 1." << std::endl;
                num_iter = 1; // Ensure at least one iteration
            }
        }
        else if (arg == "-t" || arg == "--threads")
        {
            if (!parse_arg(argc, argv, i, num_threads, static_cast<uint8_t>(1),
                           static_cast<uint8_t>(std::thread::hardware_concurrency())))
            {
                std::cerr << "Error: Invalid number of threads specified." << std::endl;
                return 1;
            }

            if (num_threads == 0)
            {
                std::cout << "Warning: Number of threads set to 0. Defaulting to 2." << std::endl;
                num_threads = 2; // Ensure at least two threads
            }
            else if (num_threads > std::thread::hardware_concurrency())
            {
                std::cout << "Warning: Number of threads exceeds hardware concurrency. "
                          << "Limiting to " << std::thread::hardware_concurrency() << "." << std::endl;
            }
        }
        else if (arg == "--disable-optimized-spsc")
        {
            disable_optimized_spsc = true;
        }
        else
        {
            benchmark_mode = arg; // Assume the first non-option argument is the mode
        }
    }

    if (argc < 2 || show_help || benchmark_mode.empty())
    {
        print_help(argv[0]);
        return 0;
    }

    switch (parse_mode(benchmark_mode))
    {
        case TestMode::SIMPLE:
            run_simple_benchmark(num_iter, num_threads);
            break;
        case TestMode::COMPLEX:
            run_complex_benchmark(num_prod, num_cons, num_iter, num_threads);
            break;
        case TestMode::STL:
            run_stl_queue_benchmark(num_prod, num_cons, num_iter, num_threads);
            break;
        case TestMode::BOOST_LFQ:
            run_boost_lockfree_queue_benchmark(num_prod, num_cons, num_iter, num_threads, disable_optimized_spsc);
            break;
        case TestMode::BOOST_SYNC_BOUNDED:
            run_boost_sync_bounded_queue_benchmark(num_prod, num_cons, num_iter, num_threads);
            break;
#ifdef ENABLE_FOLLY_BENCHMARKS
        case TestMode::FOLLYQ:
            run_follyq_benchmark(num_prod, num_cons, num_iter, num_threads, disable_optimized_spsc);
            break;
#endif
        default:
            std::cerr << "Unknown mode provided: " << argv[1] << std::endl;
            return 1;
    }

    return 0;
}