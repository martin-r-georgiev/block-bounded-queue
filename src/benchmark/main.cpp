#include <queue/bbq.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
#include <thread>
#include <type_traits>
#include <vector>

#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/thread/sync_bounded_queue.hpp>

#ifdef ENABLE_FOLLY_BENCHMARKS
    #include <folly/MPMCQueue.h>
    #include <folly/ProducerConsumerQueue.h>
#endif

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    #include <sched.h>
    #define THREAD_YIELD() sched_yield() // Use POSIX sched_yield to do a direct system call
#else
    #define THREAD_YIELD() std::this_thread::yield() // Fallback to C++ STL thread yield
#endif

#if defined(__linux__) || defined(__unix__)
    #include <time.h>
constexpr struct timespec NANOSECOND_SLEEP{.tv_sec = 0, .tv_nsec = 1};
    #define THREAD_SLEEP() nanosleep(&NANOSECOND_SLEEP, nullptr)
#else
    #define THREAD_SLEEP() std::this_thread::sleep_for(std::chrono::nanoseconds(1))
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

// Do additional correctness checks in benchmarks; Slows down the benchmarks significantly
#ifndef CORR_CHECKS
    #define CORR_CHECKS 0
#endif

#if CORR_CHECKS
    #include <unordered_set>
#endif

// Percentage of results to trim from both ends
constexpr double TRIM_PERCENTAGE = 0.1;
constexpr uint64_t SAMPLE_RATE = 1000; // Record a sample very SAMPLE_RATE operations
constexpr uint64_t SAMPLE_COUNT = (BENCHMARK_ITEM_COUNT / SAMPLE_RATE);

constexpr double NS_PER_SEC = 1'000'000'000.0;

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
    uint8_t thread_count = 0;
    uint64_t items_per_prod = 0;
    std::optional<uint64_t> enq_count = std::nullopt;
    std::optional<uint64_t> deq_count = std::nullopt;
    std::vector<q_val_t>* deq_items;
    double elapsed_total_ns = 0;
    double stddev_elapsed_total_ns = 0.0;
    double mean_prod_ns = 0.0;
    double stddev_prod_ns = 0.0;
    double mean_cons_ns = 0.0;
    double stddev_cons_ns = 0.0;
    double mean_enq_latency_ns = 0.0;
    double stddev_enq_latency_ns = 0.0;
    double mean_enq_full_latency_ns = 0.0;
    double stddev_enq_full_latency_ns = 0.0;
    double mean_deq_latency_ns = 0.0;
    double stddev_deq_latency_ns = 0.0;
    double mean_deq_empty_latency_ns = 0.0;
    double stddev_deq_empty_latency_ns = 0.0;
    bool is_bbq = false;
    queues::QueueMode queue_mode = queues::QueueMode::RETRY_NEW;
};

/// @brief Parameters for running a benchmark via the template benchmark runner.
struct BenchmarkParams
{
    std::string_view label;
    const uint32_t thread_count;
    const uint64_t item_count;
    const uint8_t num_prod;
    const uint8_t num_cons;
    std::atomic<uint64_t>& enq_counter;
    std::atomic<uint64_t>& deq_counter;
    std::vector<q_val_t>* deq_items = nullptr;
    std::atomic<bool>* producers_done = nullptr;
    bool is_bbq = false;
    uint32_t curr_iter = -1;  // Used only for printing
    uint32_t total_iters = 1; // Used only for printing
};

enum class TimeUnit : uint8_t
{
    NANOSECONDS = 0,
    MICROSECONDS = 1,
    MILLISECONDS = 2,
    SECONDS = 3,
    MINUTES = 4
};

std::string time_unit_to_string(TimeUnit unit)
{
    switch (unit)
    {
        case TimeUnit::NANOSECONDS:
            return "ns";
        case TimeUnit::MICROSECONDS:
            return "µs";
        case TimeUnit::MILLISECONDS:
            return "ms";
        case TimeUnit::SECONDS:
            return "s";
        case TimeUnit::MINUTES:
            return "min";
        default:
            return "unknown";
    }
}

template<typename T, typename = std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>>>
std::string round_up_measurement(T val, TimeUnit val_unit, std::string_view suffix = "")
{
    double calculation = val;

    using underlying = std::underlying_type_t<TimeUnit>;
    while (calculation >= 1000.0 && val_unit != TimeUnit::MINUTES)
    {
        calculation /= 1000.0;
        val_unit = static_cast<TimeUnit>(static_cast<underlying>(val_unit) + 1);
    }

    // If the value is an integer, format it without decimal places
    if constexpr (std::is_integral_v<T>)
        return std::format("{} {}{}", calculation, time_unit_to_string(val_unit), suffix);
    else if (std::abs(std::round(calculation) - calculation) < 0.0001f)
        return std::format("{:.0f} {}{}", calculation, time_unit_to_string(val_unit), suffix);
    else
        return std::format("{:.4f} {}{}", calculation, time_unit_to_string(val_unit), suffix);
}

// Source: https://gist.github.com/JBlond/2fea43a3049b38287e5e9cefc87b2124#file-bash-colors-md
constexpr char ANSI_CLR_RESET[] = "\033[0m";
constexpr char ANSI_CLR_GREEN[] = "\033[1;32m";
constexpr char ANSI_CLR_BRIGHT_BLACK[] = "\033[1;30m";
constexpr char ANSI_CLR_BOLD_WHITE[] = "\033[1;37m";
constexpr char PROG_BAR_CHAR[] = "▪";
constexpr int PROG_BAR_WIDTH = 32;

/// @brief Displays a progress bar in the terminal.
/// @remark Based on the following implementation: https://stackoverflow.com/a/14539953
void display_progress_bar(const size_t current, size_t total, std::string_view prefix = "", int width = 40)
{
    if (total == 0)
        total = 1;

    const float progress = static_cast<float>(current) / total;
    const int pos = static_cast<int>(width * progress);

    std::cout << "\r" << prefix << " " << ANSI_CLR_GREEN;
    for (int i = 0; i < width; ++i)
        if (i < pos)
            std::cout << PROG_BAR_CHAR;
        else
            std::cout << ANSI_CLR_BRIGHT_BLACK << PROG_BAR_CHAR;

    std::cout << ANSI_CLR_RESET << std::format(" {:<6.1f}% ({}/{})", progress * 100.0, current, total) << std::flush;

    if (current == total)
        std::cout << std::endl;
}

/// @brief Pretty-prints the (aggregate) benchmark results to the console.
/// @param res The set of benchmark results to print.
void print_benchmark_results(const BenchmarkResult& res, const bool show_throughput = true,
                             const bool show_latency = true)
{
    auto print_msg = [&](const std::string& msg, const char* ansi_color = ANSI_CLR_RESET)
    { std::cout << "[" << res.label << "] " << ansi_color << msg << ANSI_CLR_RESET << std::endl; };
    auto print_err = [&](const std::string& msg) { std::cerr << "[" << res.label << "] " << msg << std::endl; };

    uint64_t total_items = res.num_prod * res.items_per_prod;

    std::cout << std::endl;
    print_msg("========================================================", ANSI_CLR_BOLD_WHITE);
    print_msg(" Benchmark Results (Avg.)", ANSI_CLR_BOLD_WHITE);
    print_msg("========================================================", ANSI_CLR_BOLD_WHITE);
    std::cout << std::endl;
    print_msg("== Summary =============================================", ANSI_CLR_BOLD_WHITE);
    print_msg(std::format("Producers: {} | Consumers: {} | Threads: {}", res.num_prod, res.num_cons, res.thread_count));
    print_msg(
        std::format(std::locale(""), "Enqueued items: {:L} ({:L} per producer)", total_items, res.items_per_prod));
    if (res.is_bbq)
        print_msg(
            std::format("Queue mode: {}", res.queue_mode == queues::QueueMode::DROP_OLD ? "DROP OLD" : "RETRY NEW"));

    // Show correctness information only where applicable
    if (show_throughput && (!res.is_bbq || (res.is_bbq && res.queue_mode == queues::QueueMode::RETRY_NEW)))
    {
        std::cout << std::endl;
        print_msg("== Correctness =========================================", ANSI_CLR_BOLD_WHITE);
        print_msg(std::format(std::locale(""), "Expected items: {:L}", total_items));
        print_msg(std::format(std::locale(""), "Actual(ENQ): {:L} | Actual(DEQ): {:L}", res.enq_count.value_or(0),
                              res.deq_count.value_or(0)));

        if (res.enq_count == res.deq_count)
            print_msg("Success: Matching enqueued and dequeued item counts.");
        else
            print_err("Error: Mismatched enqueued and dequeued item counts!");

#if CORR_CHECKS
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
                    print_err(std::format("Error: Duplicate item detected: {}", v));
                    duplicate_found = true;
                }
            }

            for (q_val_t i = 0; i < total_items; ++i)
            {
                if (seen.find(i) == seen.end())
                {
                    print_err(std::format("Error: Item \"{}\" was not found in the dequeued items!", i));
                    missing_found = true;
                }
            }

            if (!duplicate_found && !missing_found)
                print_msg("Success: All expected items were found exactly once.");
            else
                print_err("Error: Issues detected in dequeued items (see above).");
        }
#endif
    }

    std::cout << std::endl;
    print_msg("== Statistics ==========================================", ANSI_CLR_BOLD_WHITE);
    print_msg("Elapsed time (MEAN [SD]):");
    print_msg(std::format(":: Total:       {:<14} [{}]",
                          round_up_measurement(res.elapsed_total_ns, TimeUnit::NANOSECONDS),
                          round_up_measurement(res.stddev_elapsed_total_ns, TimeUnit::NANOSECONDS)));
    print_msg(std::format(":: Producer(s): {:<14} [{}]", round_up_measurement(res.mean_prod_ns, TimeUnit::NANOSECONDS),
                          round_up_measurement(res.stddev_prod_ns, TimeUnit::NANOSECONDS)));
    print_msg(std::format(":: Consumer(s): {:<14} [{}]", round_up_measurement(res.mean_cons_ns, TimeUnit::NANOSECONDS),
                          round_up_measurement(res.stddev_cons_ns, TimeUnit::NANOSECONDS)));

    if (show_throughput)
    {
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

            print_msg("Throughput (MEAN):");

            print_msg(std::format(":: Total:        {:e} op/s", (res.enq_count.value() + res.deq_count.value()) /
                                                                    (res.elapsed_total_ns / NS_PER_SEC)));

            double mean_prod_throughput = res.enq_count.value() / (res.mean_prod_ns / NS_PER_SEC);
            double mean_cons_throughput = res.deq_count.value() / (res.mean_cons_ns / NS_PER_SEC);

            print_msg(std::format(":: Producer(s):  {:e} op/s", mean_prod_throughput));
            print_msg(std::format(":: Consumer(s):  {:e} op/s", mean_cons_throughput));
            print_msg(
                std::format(":: Fairness (MAX/MIN): {:.6f}", std::max(mean_prod_throughput, mean_cons_throughput) /
                                                                 std::min(mean_prod_throughput, mean_cons_throughput)));
        }
    }

    if (show_latency)
    {
        if (res.mean_enq_latency_ns == 0.0 || res.mean_deq_latency_ns == 0.0)
        {
            print_err("Warning: Enqueue and dequeue latencies are not available for latency calculation!");
            print_msg("Use the complex benchmark mode to get these values.");
        }
        else
        {
            print_msg("Latencies (MEAN [SD]):");
            print_msg(std::format(":: ENQ (OK):    {:<14} [{}]",
                                  round_up_measurement(res.mean_enq_latency_ns, TimeUnit::NANOSECONDS, "/op"),
                                  round_up_measurement(res.stddev_enq_latency_ns, TimeUnit::NANOSECONDS, "/op")));
            print_msg(std::format(":: ENQ (FULL):  {:<14} [{}]",
                                  round_up_measurement(res.mean_enq_full_latency_ns, TimeUnit::NANOSECONDS, "/op"),
                                  round_up_measurement(res.stddev_enq_full_latency_ns, TimeUnit::NANOSECONDS, "/op")));
            print_msg(std::format(":: DEQ (OK):    {:<14} [{}]",
                                  round_up_measurement(res.mean_deq_latency_ns, TimeUnit::NANOSECONDS, "/op"),
                                  round_up_measurement(res.stddev_deq_latency_ns, TimeUnit::NANOSECONDS, "/op")));
            print_msg(std::format(":: DEQ (EMPTY): {:<14} [{}]",
                                  round_up_measurement(res.mean_deq_empty_latency_ns, TimeUnit::NANOSECONDS, "/op"),
                                  round_up_measurement(res.stddev_deq_empty_latency_ns, TimeUnit::NANOSECONDS, "/op")));
        }
    }

    std::cout << std::endl;
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
 * @brief Calculates the standard deviation of the provided vector.
 *
 * @tparam T The type of elements stored in the vector. Must be either integral or floating-point type.
 * @param v The target vector to calculate the standard deviation of.
 * @return double The resulting standard deviation value.
 */
template<typename T, typename = std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>>>
double calculate_std_dev(const std::vector<T>& v)
{
    if (v.empty())
        return 0.0;

    double mean = calculate_mean(v);
    double accum = 0.0;
    for (const auto& val : v)
    {
        double diff = static_cast<double>(val) - mean;
        accum += diff * diff;
    }

    return std::sqrt(accum / v.size());
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
    std::vector<uint64_t> prod_times_ns(params.num_prod, 0);
    std::vector<uint64_t> cons_times_ns(params.num_cons, 0);

    if (params.curr_iter != -1 && params.total_iters != -1)
        display_progress_bar(params.curr_iter, params.total_iters, "Progress", PROG_BAR_WIDTH);

    // Start producer and consumer threads
    uint32_t prod_idx = 0, cons_idx = 0;
    uint32_t total_threads = params.num_prod + params.num_cons;
    for (uint32_t t = 0; t < total_threads; ++t)
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
                    prod_times_ns[prod_idx] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
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
                    cons_times_ns[cons_idx] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
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
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();

    if (params.curr_iter != -1 && params.total_iters != -1)
        display_progress_bar(params.curr_iter + 1, params.total_iters, "Progress", PROG_BAR_WIDTH);

    // Calculate mean producer and consumer times
    double mean_prod_ns = calculate_mean(prod_times_ns);
    double mean_cons_ns = calculate_mean(cons_times_ns);

    // Read final enqueue/dequeue counts (updated once per thread at the end)
    uint64_t enq_count = params.enq_counter.load(std::memory_order_relaxed);
    uint64_t deq_count = params.deq_counter.load(std::memory_order_relaxed);

    BenchmarkResult res;
    res.label = params.label;
    res.num_prod = params.num_prod;
    res.num_cons = params.num_cons;
    res.thread_count = std::min(static_cast<unsigned int>(params.thread_count), std::thread::hardware_concurrency());
    res.items_per_prod = params.item_count / params.num_prod;
    res.enq_count = enq_count != 0 ? std::optional<uint64_t>(enq_count) : std::nullopt;
    res.deq_count = deq_count != 0 ? std::optional<uint64_t>(deq_count) : std::nullopt;
    res.deq_items = params.deq_items;
    res.elapsed_total_ns = elapsed_ns;
    res.mean_prod_ns = mean_prod_ns;
    res.mean_cons_ns = mean_cons_ns;
    res.is_bbq = params.is_bbq;
    res.queue_mode = QUEUE_MODE;

    return res;
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
    std::vector<double> elapsed_times_ns, prod_times_ns, cons_times_ns;
    std::vector<double> enq_counts, deq_counts;
    std::vector<double> enq_latency_ns, enq_full_latency_ns;
    std::vector<double> deq_latency_ns, deq_empty_latency_ns;

    for (const auto& r : results)
    {
        elapsed_times_ns.push_back(r.elapsed_total_ns);
        prod_times_ns.push_back(r.mean_prod_ns);
        cons_times_ns.push_back(r.mean_cons_ns);

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
    std::size_t size_before_trim = elapsed_times_ns.size();
    trim_vector(elapsed_times_ns, TRIM_PERCENTAGE);
    trim_vector(prod_times_ns, TRIM_PERCENTAGE);
    trim_vector(cons_times_ns, TRIM_PERCENTAGE);
    trim_vector(enq_counts, TRIM_PERCENTAGE);
    trim_vector(deq_counts, TRIM_PERCENTAGE);
    trim_vector(enq_latency_ns, TRIM_PERCENTAGE);
    trim_vector(enq_full_latency_ns, TRIM_PERCENTAGE);
    trim_vector(deq_latency_ns, TRIM_PERCENTAGE);
    trim_vector(deq_empty_latency_ns, TRIM_PERCENTAGE);
    std::cout << std::format("Trimmed {}/{} results.", size_before_trim - elapsed_times_ns.size(), size_before_trim)
              << std::endl;

    // Calculate the mean of the trimmed results
    BenchmarkResult agg = results.front();
    agg.elapsed_total_ns = calculate_mean(elapsed_times_ns);
    agg.stddev_elapsed_total_ns = calculate_std_dev(elapsed_times_ns);
    agg.mean_prod_ns = calculate_mean(prod_times_ns);
    agg.stddev_prod_ns = calculate_std_dev(prod_times_ns);
    agg.mean_cons_ns = calculate_mean(cons_times_ns);
    agg.stddev_cons_ns = calculate_std_dev(cons_times_ns);
    agg.enq_count = enq_counts.empty() ? std::nullopt : std::optional<uint64_t>(calculate_mean(enq_counts));
    agg.deq_count = deq_counts.empty() ? std::nullopt : std::optional<uint64_t>(calculate_mean(deq_counts));
    agg.mean_enq_latency_ns = calculate_mean(enq_latency_ns);
    agg.stddev_enq_latency_ns = calculate_std_dev(enq_latency_ns);
    agg.mean_enq_full_latency_ns = calculate_mean(enq_full_latency_ns);
    agg.stddev_enq_full_latency_ns = calculate_std_dev(enq_full_latency_ns);
    agg.mean_deq_latency_ns = calculate_mean(deq_latency_ns);
    agg.stddev_deq_latency_ns = calculate_std_dev(deq_latency_ns);
    agg.mean_deq_empty_latency_ns = calculate_mean(deq_empty_latency_ns);
    agg.stddev_deq_empty_latency_ns = calculate_std_dev(deq_empty_latency_ns);

    return agg;
}

// Note: This is based on the simple SPSC benchmark outlined in the academic paper.
// The main purpose of this benchmark is to determine the raw performance of the queue.
BenchmarkResult run_simple_benchmark(const uint32_t iters, const uint32_t thread_count)
{
    std::atomic<uint64_t> enq_counter{0};
    std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producer_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

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
                           .total_iters = iters};

    std::cout << "[SMPL] " << ANSI_CLR_BOLD_WHITE
              << "== Logs ================================================" << ANSI_CLR_RESET << std::endl;

    std::vector<BenchmarkResult> results;
    results.reserve(iters);
    for (uint32_t i = 0; i < iters; ++i)
    {
        queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> bbq(BBQ_NUM_BLOCKS, BBQ_ENTRIES_PER_BLOCK);
        producer_done.store(false, std::memory_order_release);
        params.curr_iter = i;

        res = run_benchmark(
            params,
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                for (uint64_t j = 0; j < BENCHMARK_ITEM_COUNT; ++j)
                    while (bbq.enqueue(j) != queues::OpStatus::OK)
                        THREAD_SLEEP();
            },
            [&]([[maybe_unused]] uint8_t thread_idx)
            {
                std::pair<std::optional<uint64_t>, queues::OpStatus> buf;
                if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
                {
                    while (true)
                    {
                        buf = bbq.dequeue();

#if CORR_CHECKS
                        if (buf.second == queues::OpStatus::OK && !buf.first.has_value()) [[unlikely]]
                            abort();
#endif

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
                        // clang-format off
                        if (buf.second == queues::OpStatus::OK)
                        {
#if CORR_CHECKS
                            // Verify that the dequeued value matches the expected value
                            if (!buf.first.has_value() || buf.first.value() != i) [[unlikely]]
                                abort();
#endif

                            ++i;
                        }
                        else
                        {
                            THREAD_SLEEP();
                        }
                        // clang-format on
                    }
                }
            });

        results.emplace_back(std::move(res));
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr);
    return aggr;
}

BenchmarkResult run_complex_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                                      const uint32_t thread_count, const bool run_tp, const bool run_lat)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

#if CORR_CHECKS
    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    deq_items.reserve(BENCHMARK_ITEM_COUNT);
#endif

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "CPLX",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
#if CORR_CHECKS
                           .deq_items = &deq_items,
#else
                           .deq_items = nullptr,
#endif
                           .producers_done = &producers_done,
                           .is_bbq = true,
                           .total_iters = iters};

    std::cout << "[CPLX] " << ANSI_CLR_BOLD_WHITE
              << "== Logs ================================================" << ANSI_CLR_RESET << std::endl;

    std::vector<BenchmarkResult> results;
    results.reserve(iters);

    if (run_tp)
    {
        std::cout << "Running throughput benchmarks..." << std::endl;
        for (uint32_t i = 0; i < iters; ++i)
        {
            queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> bbq(BBQ_NUM_BLOCKS, BBQ_ENTRIES_PER_BLOCK);
            producers_done.store(false, std::memory_order_release);
            enq_counter.store(0, std::memory_order_release);
            deq_counter.store(0, std::memory_order_release);

            params.curr_iter = i;

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    uint64_t local_counter = 0;
                    for (uint64_t j = 0; j < items_per_prod; ++j)
                    {
                        const q_val_t item_val = (thread_idx * items_per_prod) + j;
                        while (bbq.enqueue(item_val) != queues::OpStatus::OK)
                            THREAD_YIELD();

                        ++local_counter;
                    }

                    enq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    std::pair<std::optional<uint64_t>, queues::OpStatus> buf;
                    uint64_t local_counter = 0;

                    while (true)
                    {
                        buf = bbq.dequeue();
                        // clang-format off
                    if (buf.second == queues::OpStatus::OK)
                    {
                        ++local_counter;

#if CORR_CHECKS
                        if (!buf.first.has_value())
                            abort(); // This should never happen in a well-formed queue

                        {
                            std::scoped_lock lock(queue_mutex);
                            deq_items.emplace_back(buf.first.value());
                        }
#endif
                    }
                    else if (buf.second == queues::OpStatus::EMPTY && producers_done.load(std::memory_order_acquire))
                        break; // Exit: producers are done and queue is empty
                    else
                    {
                        THREAD_YIELD();
                    }
                        // clang-format on
                    }

                    deq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                });

            results.emplace_back(std::move(res));
        }
    }

    if (run_lat)
    {
        std::cout << "Running enqueue(OK) and dequeue(OK) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> bbq(BBQ_NUM_BLOCKS, BBQ_ENTRIES_PER_BLOCK);
            producers_done.store(false, std::memory_order_release);

            std::vector<uint64_t> enq_latencies_ns;
            enq_latencies_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_ns;
            deq_latencies_ns.reserve(SAMPLE_COUNT);

            std::mutex op_enq_lat_mutex;
            std::mutex op_deq_lat_mutex;

            params.curr_iter = i;

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    queues::OpStatus status;
                    uint64_t j = 0;
                    auto start = std::chrono::steady_clock::time_point{};
                    auto end = std::chrono::steady_clock::time_point{};

                    while (j < items_per_prod)
                    {
                        const q_val_t item_val = (thread_idx * items_per_prod) + j;
                        const bool do_sample = (j % SAMPLE_RATE == 0);

                        if (do_sample)
                        {
                            start = std::chrono::steady_clock::now();
                            status = bbq.enqueue(item_val);
                            end = std::chrono::steady_clock::now();
                        }
                        else
                        {
                            status = bbq.enqueue(item_val);
                        }

                        if (status == queues::OpStatus::OK)
                        {
                            ++j;

                            if (do_sample)
                            {
                                std::scoped_lock lock(op_enq_lat_mutex);
                                enq_latencies_ns.emplace_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }
                        }
                        else
                        {
                            THREAD_YIELD();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    std::pair<std::optional<uint64_t>, queues::OpStatus> buf;
                    auto start = std::chrono::steady_clock::time_point{};
                    auto end = std::chrono::steady_clock::time_point{};
                    uint64_t local_counter = 0;

                    while (true)
                    {
                        const bool do_sample = (local_counter % SAMPLE_RATE == 0);

                        if (do_sample)
                        {
                            start = std::chrono::steady_clock::now();
                            buf = bbq.dequeue();
                            end = std::chrono::steady_clock::now();
                        }
                        else
                        {
                            buf = bbq.dequeue();
                        }

                        if (buf.second == queues::OpStatus::OK)
                        {
                            ++local_counter;

                            if (do_sample)
                            {
                                std::scoped_lock lock(op_deq_lat_mutex);
                                deq_latencies_ns.emplace_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }
                        }
                        else if (buf.second == queues::OpStatus::EMPTY &&
                                 producers_done.load(std::memory_order_acquire))
                        {
                            break; // Exit: producers are done and queue is empty
                        }
                        else
                        {
                            THREAD_YIELD();
                        }
                    }
                });

            // If the throughput benchmark was not run, add results to the vector
            if (!run_tp)
                results.emplace_back(std::move(res));

            if (i < results.size())
            {
                results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
                results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }
        }

        std::cout << "Running enqueue(FULL) and dequeue(EMPTY) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            display_progress_bar(i, iters, "Progress", PROG_BAR_WIDTH);

            queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> bbq(BBQ_NUM_BLOCKS, BBQ_ENTRIES_PER_BLOCK);
            producers_done.store(false, std::memory_order_release);

            std::vector<uint64_t> enq_latencies_full_ns;
            enq_latencies_full_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_empty_ns;
            deq_latencies_empty_ns.reserve(SAMPLE_COUNT);

            auto start = std::chrono::steady_clock::time_point{};
            auto end = std::chrono::steady_clock::time_point{};

            for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                bbq.enqueue(j); // Fill the queue to ensure it is full

            for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
            {
                start = std::chrono::steady_clock::now();
                auto status = bbq.enqueue(j);
                end = std::chrono::steady_clock::now();

                if (status == queues::OpStatus::FULL)
                {
                    enq_latencies_full_ns.emplace_back(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                }
                else if (status != queues::OpStatus::OK)
                {
                    std::cerr << "Error: Unexpected status during enqueue in full queue: " << static_cast<int>(status)
                              << std::endl;
                    abort();
                }
            }

            for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                bbq.dequeue(); // Empty the queue to ensure it is empty

            for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
            {
                start = std::chrono::steady_clock::now();
                auto buf = bbq.dequeue();
                end = std::chrono::steady_clock::now();

                if (buf.second == queues::OpStatus::EMPTY)
                {
                    deq_latencies_empty_ns.emplace_back(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                }
                else if (buf.second != queues::OpStatus::OK)
                {
                    std::cerr << "Error: Unexpected status during dequeue in empty queue: "
                              << static_cast<int>(buf.second) << std::endl;
                    abort();
                }
            }

            if (i < results.size())
            {
                results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
                results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }

            display_progress_bar(i + 1, iters, "Progress", PROG_BAR_WIDTH);
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr, run_tp, run_lat);
    return aggr;
}

BenchmarkResult run_stl_queue_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                                        const uint32_t thread_count, const bool run_tp, const bool run_lat)
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
                           .total_iters = iters};

    std::cout << "[STL] " << ANSI_CLR_BOLD_WHITE
              << "== Logs ================================================" << ANSI_CLR_RESET << std::endl;

    std::vector<BenchmarkResult> results;
    results.reserve(iters);

    if (run_tp)
    {
        std::cout << "Running throughput benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            std::queue<q_val_t> stl_queue;
            enq_counter.store(0, std::memory_order_release);
            deq_counter.store(0, std::memory_order_release);

            params.curr_iter = i;

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    uint64_t local_counter = 0;
                    for (uint32_t j = 0; j < items_per_prod; ++j)
                    {
                        const q_val_t item_val = (thread_idx * items_per_prod) + j;
                        {
                            std::unique_lock<std::mutex> lock(stl_mtx);
                            stl_queue.push(item_val);
                        }

                        ++local_counter;
                        stl_cv.notify_one();
                    }

                    enq_counter.fetch_add(local_counter, std::memory_order_relaxed);
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
    }

    if (run_lat)
    {
        std::cout << "Running enqueue(OK) and dequeue(OK) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            std::queue<q_val_t> stl_queue;
            deq_counter.store(0, std::memory_order_release);

            std::vector<uint64_t> enq_latencies_ns;
            enq_latencies_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_ns;
            deq_latencies_ns.reserve(SAMPLE_COUNT);

            params.curr_iter = i;

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    auto start = std::chrono::steady_clock::time_point{};
                    auto end = std::chrono::steady_clock::time_point{};

                    for (uint32_t j = 0; j < items_per_prod; ++j)
                    {
                        const q_val_t item_val = (thread_idx * items_per_prod) + j;
                        const bool do_sample = (j % SAMPLE_RATE == 0);

                        if (do_sample)
                        {
                            std::unique_lock<std::mutex> lock(stl_mtx);
                            start = std::chrono::steady_clock::now();
                            stl_queue.push(item_val);
                            end = std::chrono::steady_clock::now();
                            enq_latencies_ns.emplace_back(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                        }
                        else
                        {
                            std::unique_lock<std::mutex> lock(stl_mtx);
                            stl_queue.push(item_val);
                        }

                        stl_cv.notify_one();
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    auto start = std::chrono::steady_clock::time_point{};
                    auto end = std::chrono::steady_clock::time_point{};

                    while (deq_counter.load(std::memory_order_acquire) < BENCHMARK_ITEM_COUNT)
                    {
                        std::unique_lock<std::mutex> lock(stl_mtx);
                        stl_cv.wait(lock,
                                    [&]
                                    {
                                        return !stl_queue.empty() ||
                                               deq_counter.load(std::memory_order_acquire) >= BENCHMARK_ITEM_COUNT;
                                    });

                        const bool do_sample = (deq_counter.load(std::memory_order_acquire) % SAMPLE_RATE == 0);

                        if (do_sample)
                        {
                            start = std::chrono::steady_clock::now();
                            if (!stl_queue.empty())
                            {
                                stl_queue.pop();
                                end = std::chrono::steady_clock::now();
                                deq_latencies_ns.emplace_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                deq_counter.fetch_add(1, std::memory_order_acq_rel);
                            }
                        }
                        else if (!stl_queue.empty())
                        {
                            stl_queue.pop();
                            deq_counter.fetch_add(1, std::memory_order_acq_rel);
                        }
                    }
                });

            // If the throughput benchmark was not run, add results to the vector
            if (!run_tp)
                results.emplace_back(std::move(res));

            if (i < results.size())
            {
                results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
                results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }
        }

        std::cout << "Running dequeue(EMPTY) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            display_progress_bar(i, iters, "Progress", PROG_BAR_WIDTH);

            std::queue<q_val_t> stl_queue;
            std::vector<uint64_t> deq_latencies_empty_ns;
            deq_latencies_empty_ns.reserve(SAMPLE_COUNT);

            auto start = std::chrono::steady_clock::time_point{};
            auto end = std::chrono::steady_clock::time_point{};

            for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
            {
                start = std::chrono::steady_clock::now();
                if (stl_queue.empty())
                    ; // std::queue's pop() has undefined behavior so the only check we can do is on empty()
                end = std::chrono::steady_clock::now();
                deq_latencies_empty_ns.emplace_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            }

            if (i < results.size())
            {
                results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }

            display_progress_bar(i + 1, iters, "Progress", PROG_BAR_WIDTH);
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr, run_tp, run_lat);
    return aggr;
}

// Based on example from Boost documentation:
// https://www.boost.org/doc/libs/master/doc/html/lockfree/examples.html
// Note: Boost's queue seems to work the same as the "RETRY_NEW" mode of BlockBoundedQueue
BenchmarkResult run_boost_lockfree_queue_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                                                   const uint32_t thread_count, const bool run_tp, const bool run_lat,
                                                   bool disable_optimized_spsc = false)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

#if CORR_CHECKS
    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    deq_items.reserve(BENCHMARK_ITEM_COUNT);
#endif

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "BST-LFQ",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
#if CORR_CHECKS
                           .deq_items = &deq_items,
#else
                           .deq_items = nullptr,
#endif
                           .producers_done = &producers_done,
                           .total_iters = iters};

    // Note(s):
    // - Boost::lockfree: freelist size is limited to a maximum of 65535 objects.
    // - Boost is the only queue allocated on the heap as it triggers a stack overflow if an attempt is made
    //   to allocatea queue on the stack. This is likely because the other queues internally allocate on the
    //   heap whereas boost tries to fully allocated on the stack.
    constexpr std::size_t Q_CAPACITY = std::min(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK,
                                                static_cast<uint64_t>(std::numeric_limits<uint16_t>::max() - 1));

    std::cout << "[BST-LFQ] " << ANSI_CLR_BOLD_WHITE
              << "== Logs ================================================" << ANSI_CLR_RESET << std::endl;

    if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        std::cout << "Using wait-free single-producer/single-consumer (SPSC) queue." << std::endl;
    else
        std::cout << "Using lock-free multi-producer/multi-consumer (MPMC) queue." << std::endl;

    std::vector<BenchmarkResult> results;
    results.reserve(iters);

    if (run_tp)
    {
        std::cout << "Running throughput benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            producers_done.store(false, std::memory_order_release);
            enq_counter.store(0, std::memory_order_release);
            deq_counter.store(0, std::memory_order_release);

            params.curr_iter = i;

            if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
            {
                auto boost_spsc_q =
                    std::make_unique<boost::lockfree::spsc_queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

                res = run_benchmark(
                    params,
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        uint64_t local_counter = 0;
                        for (uint64_t j = 0; j < items_per_prod; ++j)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            while (!boost_spsc_q->push(item_val))
                                THREAD_YIELD();

                            ++local_counter;
                        }

                        enq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            // clang-format off
                        while (boost_spsc_q->pop(item_val))
                        {
#if CORR_CHECKS
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }
#endif

                            ++local_counter;
                        }
                            // clang-format on

                            if (boost_spsc_q->empty() && producers_done.load(std::memory_order_acquire))
                                break;

                            THREAD_YIELD();
                        }

                        deq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                    });
            }
            else
            {
                auto boost_q =
                    std::make_unique<boost::lockfree::queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

                res = run_benchmark(
                    params,
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        uint64_t local_counter = 0;
                        for (uint64_t j = 0; j < items_per_prod; ++j)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            while (!boost_q->push(item_val))
                                THREAD_YIELD();

                            ++local_counter;
                        }

                        enq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            // clang-format off
                        while (boost_q->pop(item_val))
                        {
#if CORR_CHECKS
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }
#endif

                            ++local_counter;
                        }
                            // clang-format on

                            if (boost_q->empty() && producers_done.load(std::memory_order_acquire))
                                break;

                            THREAD_YIELD();
                        }

                        deq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                    });
            }

            results.emplace_back(std::move(res));
        }
    }

    if (run_lat)
    {
        std::cout << "Running enqueue(OK) and dequeue(OK) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            producers_done.store(false, std::memory_order_release);

            std::vector<uint64_t> enq_latencies_ns;
            enq_latencies_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_ns;
            deq_latencies_ns.reserve(SAMPLE_COUNT);

            std::mutex op_enq_lat_mutex;
            std::mutex op_deq_lat_mutex;

            params.curr_iter = i;

            if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
            {
                auto boost_spsc_q =
                    std::make_unique<boost::lockfree::spsc_queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

                res = run_benchmark(
                    params,
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        bool success = false;
                        uint64_t j = 0;
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};

                        while (j < items_per_prod)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            const bool do_sample = (j % SAMPLE_RATE == 0);

                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = boost_spsc_q->push(item_val);
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = boost_spsc_q->push(item_val);
                            }

                            if (success)
                            {
                                ++j;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_enq_lat_mutex);
                                    enq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        bool success = false;
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            const bool do_sample = (local_counter % SAMPLE_RATE == 0);
                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = boost_spsc_q->pop(item_val);
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = boost_spsc_q->pop(item_val);
                            }

                            if (success)
                            {
                                ++local_counter;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_deq_lat_mutex);
                                    deq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else if (boost_spsc_q->empty() && producers_done.load(std::memory_order_acquire))
                            {
                                break;
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }
                    });
            }
            else
            {
                auto boost_q =
                    std::make_unique<boost::lockfree::queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

                res = run_benchmark(
                    params,
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        bool success = false;
                        uint64_t j = 0;
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};

                        while (j < items_per_prod)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            const bool do_sample = (j % SAMPLE_RATE == 0);

                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = boost_q->push(item_val);
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = boost_q->push(item_val);
                            }

                            if (success)
                            {
                                ++j;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_enq_lat_mutex);
                                    enq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        bool success = false;
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            const bool do_sample = (local_counter % SAMPLE_RATE == 0);

                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = boost_q->pop(item_val);
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = boost_q->pop(item_val);
                            }

                            if (success)
                            {
                                ++local_counter;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_deq_lat_mutex);
                                    deq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else if (boost_q->empty() && producers_done.load(std::memory_order_acquire))
                            {
                                break; // Exit: producers are done and queue is empty
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }
                    });
            }

            // If the throughput benchmark was not run, add results to the vector
            if (!run_tp)
                results.emplace_back(std::move(res));

            if (i < results.size())
            {
                results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
                results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }
        }

        std::cout << "Running enqueue(FULL) and dequeue(EMPTY) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            display_progress_bar(i, iters, "Progress", PROG_BAR_WIDTH);

            std::vector<uint64_t> enq_latencies_full_ns;
            enq_latencies_full_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_empty_ns;
            deq_latencies_empty_ns.reserve(SAMPLE_COUNT);

            auto start = std::chrono::steady_clock::time_point{};
            auto end = std::chrono::steady_clock::time_point{};

            if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
            {
                auto boost_q =
                    std::make_unique<boost::lockfree::spsc_queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

                for (uint64_t j = 0; j < Q_CAPACITY; ++j)
                    boost_q->push(j); // Fill the queue to ensure it is full

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = boost_q->push(j);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        enq_latencies_full_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during enqueue in full queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }

                q_val_t item_val;

                for (uint64_t j = 0; j < Q_CAPACITY; ++j)
                    boost_q->pop(item_val); // Empty the queue to ensure it is empty

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = boost_q->pop(item_val);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        deq_latencies_empty_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during dequeue in empty queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }
            }
            else
            {
                auto boost_q =
                    std::make_unique<boost::lockfree::queue<q_val_t, boost::lockfree::capacity<Q_CAPACITY>>>();

                for (uint64_t j = 0; j < Q_CAPACITY; ++j)
                    boost_q->push(j); // Fill the queue to ensure it is full

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = boost_q->push(j);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        enq_latencies_full_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during enqueue in full queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }

                q_val_t item_val;

                for (uint64_t j = 0; j < Q_CAPACITY; ++j)
                    boost_q->pop(item_val); // Empty the queue to ensure it is empty

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = boost_q->pop(item_val);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        deq_latencies_empty_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during dequeue in empty queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }
            }

            // If the throughput benchmark was not run, add results to the vector

            if (i < results.size())
            {
                results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
                results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }

            display_progress_bar(i + 1, iters, "Progress", PROG_BAR_WIDTH);
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr, run_tp, run_lat);
    return aggr;
}

// https://www.boost.org/doc/libs/1_88_0/doc/html/thread/sds.html
BenchmarkResult run_boost_sync_bounded_queue_benchmark(const uint8_t num_prod, const uint8_t num_cons,
                                                       const uint32_t iters, const uint32_t thread_count,
                                                       const bool run_tp, const bool run_lat)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

#if CORR_CHECKS
    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    deq_items.reserve(BENCHMARK_ITEM_COUNT);
#endif

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "BST-BSYNCQ",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
#if CORR_CHECKS
                           .deq_items = &deq_items,
#else
                           .deq_items = nullptr,
#endif
                           .producers_done = &producers_done,
                           .total_iters = iters};

    std::cout << "[BST-BSYNCQ] " << ANSI_CLR_BOLD_WHITE
              << "== Logs ================================================" << ANSI_CLR_RESET << std::endl;

    std::vector<BenchmarkResult> results;
    results.reserve(iters);

    if (run_tp)
    {
        std::cout << "Running throughput benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            boost::concurrent::sync_bounded_queue<q_val_t> boost_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);
            producers_done.store(false, std::memory_order_release);
            enq_counter.store(0, std::memory_order_release);
            deq_counter.store(0, std::memory_order_release);

            params.curr_iter = i;

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    boost::concurrent::queue_op_status status;
                    uint64_t j = 0;
                    uint64_t local_counter = 0;

                    while (j < items_per_prod)
                    {
                        const q_val_t item_val = (thread_idx * items_per_prod) + j;
                        status = boost_q.try_push_back(item_val);
                        if (status == boost::concurrent::queue_op_status::success)
                        {
                            ++local_counter;
                            ++j;
                        }
                        else
                        {
                            THREAD_YIELD();
                        }
                    }

                    enq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    boost::concurrent::queue_op_status status;
                    uint64_t local_counter = 0;

                    while (true)
                    {
                        status = boost_q.try_pull_front(item_val);
                        // clang-format off
                    if (status == boost::concurrent::queue_op_status::success)
                    {
#if CORR_CHECKS
                        {
                            std::scoped_lock lock(queue_mutex);
                            deq_items.emplace_back(item_val);
                        }
#endif

                        ++local_counter;
                    }
                    else if (status == boost::concurrent::queue_op_status::empty &&
                             producers_done.load(std::memory_order_acquire))
                    {
                        break; // Exit: producers are done and queue is empty
                    }
                    else
                    {
                        THREAD_YIELD();
                    }
                        // clang-format on
                    }

                    deq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                });

            results.emplace_back(std::move(res));
        }
    }

    if (run_lat)
    {
        std::cout << "Running enqueue(OK) and dequeue(OK) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            boost::concurrent::sync_bounded_queue<q_val_t> boost_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);
            producers_done.store(false, std::memory_order_release);

            std::vector<uint64_t> enq_latencies_ns;
            enq_latencies_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_ns;
            deq_latencies_ns.reserve(SAMPLE_COUNT);

            std::mutex op_enq_lat_mutex;
            std::mutex op_deq_lat_mutex;

            params.curr_iter = i;

            res = run_benchmark(
                params,
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    boost::concurrent::queue_op_status status;
                    uint64_t j = 0;
                    auto start = std::chrono::steady_clock::time_point{};
                    auto end = std::chrono::steady_clock::time_point{};

                    while (j < items_per_prod)
                    {
                        const q_val_t item_val = (thread_idx * items_per_prod) + j;
                        const bool do_sample = (j % SAMPLE_RATE == 0);

                        if (do_sample)
                        {
                            start = std::chrono::steady_clock::now();
                            status = boost_q.try_push_back(item_val);
                            end = std::chrono::steady_clock::now();
                        }
                        else
                        {
                            status = boost_q.try_push_back(item_val);
                        }

                        if (status == boost::concurrent::queue_op_status::success)
                        {
                            ++j;

                            if (do_sample)
                            {
                                std::scoped_lock lock(op_enq_lat_mutex);
                                enq_latencies_ns.emplace_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }
                        }
                        else
                        {
                            THREAD_YIELD();
                        }
                    }
                },
                [&]([[maybe_unused]] uint8_t thread_idx)
                {
                    q_val_t item_val;
                    boost::concurrent::queue_op_status status;
                    auto start = std::chrono::steady_clock::time_point{};
                    auto end = std::chrono::steady_clock::time_point{};
                    uint64_t local_counter = 0;

                    while (true)
                    {
                        const bool do_sample = (local_counter % SAMPLE_RATE == 0);

                        if (do_sample)
                        {
                            start = std::chrono::steady_clock::now();
                            status = boost_q.try_pull_front(item_val);
                            end = std::chrono::steady_clock::now();
                        }
                        else
                        {
                            status = boost_q.try_pull_front(item_val);
                        }

                        if (status == boost::concurrent::queue_op_status::success)
                        {
                            ++local_counter;

                            if (do_sample)
                            {
                                std::scoped_lock lock(op_deq_lat_mutex);
                                deq_latencies_ns.emplace_back(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                            }
                        }
                        else if (status == boost::concurrent::queue_op_status::empty &&
                                 producers_done.load(std::memory_order_acquire))
                        {
                            break; // Exit: producers are done and queue is empty
                        }
                        else
                        {
                            THREAD_YIELD();
                        }
                    }
                });

            // If the throughput benchmark was not run, add results to the vector
            if (!run_tp)
                results.emplace_back(std::move(res));

            if (i < results.size())
            {
                results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
                results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }
        }

        std::cout << "Running enqueue(FULL) and dequeue(EMPTY) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            display_progress_bar(i, iters, "Progress", PROG_BAR_WIDTH);

            boost::concurrent::sync_bounded_queue<q_val_t> boost_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);
            std::vector<uint64_t> enq_latencies_full_ns;
            enq_latencies_full_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_empty_ns;
            deq_latencies_empty_ns.reserve(SAMPLE_COUNT);

            auto start = std::chrono::steady_clock::time_point{};
            auto end = std::chrono::steady_clock::time_point{};

            for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                boost_q.push(j); // Fill the queue to ensure it is full

            for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
            {
                start = std::chrono::steady_clock::now();
                auto status = boost_q.try_push_back(j);
                end = std::chrono::steady_clock::now();

                if (status == boost::concurrent::queue_op_status::full)
                {
                    enq_latencies_full_ns.emplace_back(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                }
                else
                {
                    std::cerr << "Error: Unexpected status during enqueue in full queue: " << static_cast<int>(status)
                              << std::endl;
                    abort();
                }
            }

            q_val_t item_val;

            for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                boost_q.pull(item_val); // Empty the queue to ensure it is empty

            for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
            {
                start = std::chrono::steady_clock::now();
                auto status = boost_q.try_pull_front(item_val);
                end = std::chrono::steady_clock::now();

                if (status == boost::concurrent::queue_op_status::empty)
                {
                    deq_latencies_empty_ns.emplace_back(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                }
                else
                {
                    std::cerr << "Error: Unexpected status during dequeue in empty queue: " << static_cast<int>(status)
                              << std::endl;
                    abort();
                }
            }

            if (i < results.size())
            {
                results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
                results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }

            display_progress_bar(i + 1, iters, "Progress", PROG_BAR_WIDTH);
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr, run_tp, run_lat);
    return aggr;
}

#ifdef ENABLE_FOLLY_BENCHMARKS
BenchmarkResult run_follyq_benchmark(const uint8_t num_prod, const uint8_t num_cons, const uint32_t iters,
                                     const uint32_t thread_count, const bool run_tp, const bool run_lat,
                                     bool disable_optimized_spsc = false)
{
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> enq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<uint64_t> deq_counter{0};
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done{false};
    alignas(CACHELINE_SIZE) BenchmarkResult res;

    #if CORR_CHECKS
    std::mutex queue_mutex;
    std::vector<q_val_t> deq_items;
    deq_items.reserve(BENCHMARK_ITEM_COUNT);
    #endif

    alignas(CACHELINE_SIZE) const uint64_t items_per_prod = BENCHMARK_ITEM_COUNT / num_prod;

    BenchmarkParams params{.label = "FOLLYQ",
                           .thread_count = thread_count,
                           .item_count = BENCHMARK_ITEM_COUNT,
                           .num_prod = num_prod,
                           .num_cons = num_cons,
                           .enq_counter = enq_counter,
                           .deq_counter = deq_counter,
    #if CORR_CHECKS
                           .deq_items = &deq_items,
    #else
                           .deq_items = nullptr,
    #endif
                           .producers_done = &producers_done,
                           .total_iters = iters};

    std::cout << "[FOLLYQ] " << ANSI_CLR_BOLD_WHITE
              << "== Logs ================================================" << ANSI_CLR_RESET << std::endl;

    if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
        std::cout << "Using single-producer/single-consumer ProducerConsumerQueue." << std::endl;
    else
        std::cout << "Using multi-producer/multi-consumer MPMCQueue." << std::endl;

    std::vector<BenchmarkResult> results;
    results.reserve(iters);

    if (run_tp)
    {
        std::cout << "Running throughput benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            producers_done.store(false, std::memory_order_release);
            enq_counter.store(0, std::memory_order_release);
            deq_counter.store(0, std::memory_order_release);

            params.curr_iter = i;

            if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
            {
                folly::ProducerConsumerQueue<q_val_t> folly_q{BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK};

                res = run_benchmark(
                    params,
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        bool success;
                        uint64_t j = 0;
                        uint64_t local_counter = 0;

                        while (j < items_per_prod)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            success = folly_q.write(std::move(item_val));
                            if (success)
                            {
                                ++local_counter;
                                ++j;
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }

                        enq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        bool success;
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            success = folly_q.read(item_val);
                            // clang-format off
                        if (success)
                        {
#if CORR_CHECKS
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }
#endif

                            ++local_counter;
                        }
                        else
                        {
                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty

                            THREAD_YIELD();
                        }
                            // clang-format on
                        }

                        deq_counter.fetch_add(local_counter, std::memory_order_relaxed);
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
                        uint64_t local_counter = 0;

                        while (j < items_per_prod)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            success = folly_q.write(std::move(item_val));
                            if (success)
                            {
                                ++local_counter;
                                ++j;
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }

                        enq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        bool success;
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            success = folly_q.read(item_val);
                            // clang-format off
                        if (success)
                        {
#if CORR_CHECKS
                            {
                                std::scoped_lock lock(queue_mutex);
                                deq_items.emplace_back(item_val);
                            }
#endif

                            ++local_counter;
                        }
                        else
                        {
                            if (producers_done.load(std::memory_order_acquire))
                                break; // Exit: producers are done and queue is empty

                            THREAD_YIELD();
                        }
                            // clang-format on
                        }

                        deq_counter.fetch_add(local_counter, std::memory_order_relaxed);
                    });
            }

            results.emplace_back(std::move(res));
        }
    }

    if (run_lat)
    {
        std::cout << "Running enqueue(OK) and dequeue(OK) latency benchmarks..." << std::endl;

        for (uint32_t i = 0; i < iters; ++i)
        {
            producers_done.store(false, std::memory_order_release);

            std::vector<uint64_t> enq_latencies_ns;
            enq_latencies_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_ns;
            deq_latencies_ns.reserve(SAMPLE_COUNT);

            std::mutex op_enq_lat_mutex;
            std::mutex op_deq_lat_mutex;

            params.curr_iter = i;

            if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
            {
                folly::ProducerConsumerQueue<q_val_t> folly_q{BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK};

                res = run_benchmark(
                    params,
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        bool success;
                        uint64_t j = 0;
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};

                        while (j < items_per_prod)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            const bool do_sample = (j % SAMPLE_RATE == 0);

                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = folly_q.write(std::move(item_val));
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = folly_q.write(std::move(item_val));
                            }

                            if (success)
                            {
                                ++j;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_enq_lat_mutex);
                                    enq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        bool success;
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            const bool do_sample = (local_counter % SAMPLE_RATE == 0);

                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = folly_q.read(item_val);
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = folly_q.read(item_val);
                            }

                            if (success)
                            {
                                ++local_counter;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_deq_lat_mutex);
                                    deq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else
                            {
                                if (producers_done.load(std::memory_order_acquire))
                                    break; // Exit: producers are done and queue is empty

                                THREAD_YIELD();
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
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};

                        while (j < items_per_prod)
                        {
                            const q_val_t item_val = (thread_idx * items_per_prod) + j;
                            const bool do_sample = (j % SAMPLE_RATE == 0);

                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = folly_q.write(std::move(item_val));
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = folly_q.write(std::move(item_val));
                            }

                            if (success)
                            {
                                ++j;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_enq_lat_mutex);
                                    enq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else
                            {
                                THREAD_YIELD();
                            }
                        }
                    },
                    [&]([[maybe_unused]] uint8_t thread_idx)
                    {
                        q_val_t item_val;
                        bool success;
                        auto start = std::chrono::steady_clock::time_point{};
                        auto end = std::chrono::steady_clock::time_point{};
                        uint64_t local_counter = 0;

                        while (true)
                        {
                            const bool do_sample = (local_counter % SAMPLE_RATE == 0);

                            if (do_sample)
                            {
                                start = std::chrono::steady_clock::now();
                                success = folly_q.read(item_val);
                                end = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                success = folly_q.read(item_val);
                            }

                            if (success)
                            {
                                ++local_counter;

                                if (do_sample)
                                {
                                    std::scoped_lock lock(op_deq_lat_mutex);
                                    deq_latencies_ns.emplace_back(
                                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                                }
                            }
                            else
                            {
                                if (producers_done.load(std::memory_order_acquire))
                                    break; // Exit: producers are done and queue is empty

                                THREAD_YIELD();
                            }
                        }
                    });
            }

            // If the throughput benchmark was not run, add results to the vector
            if (!run_tp)
                results.emplace_back(std::move(res));

            if (i < results.size())
            {
                results[i].mean_enq_latency_ns = calculate_mean(enq_latencies_ns);
                results[i].mean_deq_latency_ns = calculate_mean(deq_latencies_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }
        }

        for (uint32_t i = 0; i < iters; ++i)
        {
            display_progress_bar(i, iters, "Progress", PROG_BAR_WIDTH);

            std::vector<uint64_t> enq_latencies_full_ns;
            enq_latencies_full_ns.reserve(SAMPLE_COUNT);
            std::vector<uint64_t> deq_latencies_empty_ns;
            deq_latencies_empty_ns.reserve(SAMPLE_COUNT);

            auto start = std::chrono::steady_clock::time_point{};
            auto end = std::chrono::steady_clock::time_point{};

            if (num_prod == 1 && num_cons == 1 && !disable_optimized_spsc)
            {
                folly::ProducerConsumerQueue<q_val_t> folly_q{BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK};

                for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                    folly_q.write(j); // Fill the queue to ensure it is full

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = folly_q.write(j);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        enq_latencies_full_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during enqueue in full queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }

                q_val_t item_val;

                for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                    folly_q.read(item_val); // Empty the queue to ensure it is empty

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = folly_q.read(item_val);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        deq_latencies_empty_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during dequeue in empty queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }
            }
            else
            {
                folly::MPMCQueue<q_val_t, std::atomic, false> folly_q(BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK);

                for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                    folly_q.write(j); // Fill the queue to ensure it is full

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = folly_q.write(j);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        enq_latencies_full_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during enqueue in full queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }

                q_val_t item_val;

                for (uint64_t j = 0; j < BBQ_NUM_BLOCKS * BBQ_ENTRIES_PER_BLOCK; ++j)
                    folly_q.read(item_val); // Empty the queue to ensure it is empty

                for (uint64_t j = 0; j < SAMPLE_COUNT; ++j)
                {
                    start = std::chrono::steady_clock::now();
                    auto success = folly_q.read(item_val);
                    end = std::chrono::steady_clock::now();

                    if (!success)
                    {
                        deq_latencies_empty_ns.emplace_back(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                    }
                    else
                    {
                        std::cerr << "Error: Unexpected status during dequeue in empty queue: "
                                  << static_cast<int>(success) << std::endl;
                        abort();
                    }
                }
            }

            if (i < results.size())
            {
                results[i].mean_enq_full_latency_ns = calculate_mean(enq_latencies_full_ns);
                results[i].mean_deq_empty_latency_ns = calculate_mean(deq_latencies_empty_ns);
            }
            else
            {
                std::cout
                    << "Warning: Misalignment in benchmark results. Reported data may be inaccurate or incomplete."
                    << std::endl;
            }

            display_progress_bar(i + 1, iters, "Progress", PROG_BAR_WIDTH);
        }
    }

    auto aggr = aggregate_results(results);
    print_benchmark_results(aggr, run_tp, run_lat);
    return aggr;
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

/// @brief A structure to hold range specifications for command line arguments.
/// @note Used for the producer and consumer arguments.
struct RangeSpec
{
    /// @brief The start value of the range.
    uint8_t start = 1;

    /// @brief The end value of the range. (Optional)
    uint8_t max = 1;

    /// @brief The step value for the range. (Optional)
    uint8_t step = 1;
};

bool parse_range_arg(std::string_view arg, RangeSpec& out)
{
    std::vector<std::string> parts;
    size_t pos = 0;

    while ((pos = arg.find(':')) != std::string_view::npos)
    {
        parts.push_back(std::string(arg.substr(0, pos)));
        arg.remove_prefix(pos + 1);
    }
    parts.push_back(std::string(arg));

    if (parts.empty() || parts.size() > 3)
        return false;

    for (const auto& part : parts)
        if (part.empty() || !std::isdigit(static_cast<unsigned char>(part[0])))
            return false;

    out.start = static_cast<uint8_t>(std::atoi(parts[0].c_str()));

    // Process optional max and step values
    if (parts.size() > 1)
        out.max = static_cast<uint8_t>(std::atoi(parts[1].c_str()));
    else
        out.max = out.start; // Default to start if max is not provided

    if (parts.size() > 2)
        out.step = static_cast<uint8_t>(std::atoi(parts[2].c_str()));
    else
        out.step = 1; // Default step value

    return true;
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

void export_to_csv(const std::string& filename, const std::vector<BenchmarkResult>& results)
{
    std::ofstream file(filename);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open file " << filename << " for writing." << std::endl;
        return;
    }

    file << "label,producers,consumers,threads,total_elapsed_time_ns,total_elapsed_time_sd,"
         << "mean_producer_elapsed_time_ns,producer_elapsed_time_sd,mean_consumer_elapsed_time_ns,"
         << "consumer_elapsed_time_sd,total_throughput_op_s,mean_producer_throughput_op_s,"
         << "mean_consumer_throughput_op_s,mean_enqueue_latency_ns_op,mean_enqueue_latency_sd,"
         << "mean_enqueue_full_latency_ns_op,mean_enqueue_full_latency_sd,mean_dequeue_latency_ns_op,"
         << "mean_dequeue_latency_sd,mean_dequeue_empty_latency_ns_op,mean_dequeue_empty_latency_sd\n";

    for (const auto& result : results)
    {
        uint64_t total_ops = result.enq_count.value() + result.deq_count.value();

        double total_throughput = 0.0;
        double prod_throughput = 0.0;
        double cons_throughput = 0.0;
        if (total_ops != 0)
        {
            total_throughput = total_ops / (result.elapsed_total_ns / NS_PER_SEC);
            prod_throughput = result.enq_count.value() / (result.mean_prod_ns / NS_PER_SEC);
            cons_throughput = result.deq_count.value() / (result.mean_cons_ns / NS_PER_SEC);
        }

        // clang-format off
        file << result.label << ","
             << static_cast<int>(result.num_prod) << ","
             << static_cast<int>(result.num_cons) << ","
             << static_cast<int>(result.thread_count) << ","
             << result.elapsed_total_ns << ","
             << result.stddev_elapsed_total_ns << ","
             << result.mean_prod_ns << ","
             << result.stddev_prod_ns << ","
             << result.mean_cons_ns << ","
             << result.stddev_cons_ns << ","
             << total_throughput << ","
             << prod_throughput << ","
             << cons_throughput << ","
             << result.mean_enq_latency_ns << ","
             << result.stddev_enq_latency_ns << ","
             << result.mean_enq_full_latency_ns << ","
             << result.stddev_enq_full_latency_ns << ","
             << result.mean_deq_latency_ns << ","
             << result.stddev_deq_latency_ns << ","
             << result.mean_deq_empty_latency_ns << ","
             << result.stddev_deq_empty_latency_ns << "\n";
        // clang-format on
    }

    file.close();
    std::cout << "Benchmark results exported to " << filename << std::endl;
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
    std::cout << "    -p <N[:m[:st]]>,           Set the number of producers (default: 1)." << std::endl;
    std::cout << "    --producers <N[:m[:st]]>   You can provide optional max value and" << std::endl;
    std::cout << "                               step value to define a range of producers." << std::endl;
    std::cout << "    -c <N[:m[:st]]>,           Set the number of consumers (default: 1)." << std::endl;
    std::cout << "    --consumers <N[:m[:st]]>   You can provide optional max value and" << std::endl;
    std::cout << "                               step value to define a range of consumers." << std::endl;
    std::cout << "    -i <N>, --iterations <N>   Set the number of iterations to run (default: 1)." << std::endl;
    std::cout << "    -t <N>, --threads <N>      Set the number of threads to use (default: 2)." << std::endl;
    std::cout << "    --disable-optimized-spsc   Disable the use of SPSC queue variants." << std::endl;
    std::cout << "                               Some libraries offer faster SPSC queue data" << std::endl;
    std::cout << "                               structures, which may skew results." << std::endl;
    std::cout << "                               Use this option to disable their use." << std::endl;
    std::cout << "    -o <path>, --output <path> Export benchmark results to a CSV file at target path." << std::endl;
    std::cout << "                               If not specified, results are only printed to stdout." << std::endl;
    std::cout << "    --throughput               A flag to run the throughput subset of benchmarks." << std::endl;
    std::cout << "                               If no other scope flags are specified, all benchmarks" << std::endl;
    std::cout << "                               are run by default." << std::endl;
    std::cout << "    --latency                  A flag to run the latency subset of benchmarks." << std::endl;
    std::cout << "                               If no other scope flags are specified, all benchmarks" << std::endl;
    std::cout << "                               are run by default." << std::endl;
}
// clang-format on

int main(int argc, char** argv)
{
    std::string benchmark_mode;
    RangeSpec prod_range{1, 1, 1};
    RangeSpec cons_range{1, 1, 1};
    uint32_t num_iter = 1;
    uint32_t num_threads = -1;
    bool explicit_thread_count = false;
    bool show_help = false;
    bool disable_optimized_spsc = false;
    std::string output_file_path = "";
    bool export_results = false;
    bool run_throughput = false;
    bool run_latency = false;

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
            if (!parse_range_arg(argv[++i], prod_range))
            {
                std::cerr << "Error: Invalid number of producers specified." << std::endl;
                return 1;
            }

            if (prod_range.start == 0 || prod_range.max == 0 || prod_range.step == 0)
            {
                std::cout << "Warning: Number of producers set to 0. Defaulting to 1." << std::endl;
                prod_range = RangeSpec{1, 1, 1}; // Reset to default values
            }
        }
        else if (arg == "-c" || arg == "--consumers")
        {
            if (!parse_range_arg(argv[++i], cons_range))
            {
                std::cerr << "Error: Invalid number of consumers specified." << std::endl;
                return 1;
            }

            if (cons_range.start == 0 || cons_range.max == 0 || cons_range.step == 0)
            {
                std::cout << "Warning: Number of consumers set to 0. Defaulting to 1." << std::endl;
                cons_range = RangeSpec{1, 1, 1}; // Reset to default values
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
            if (!parse_arg(argc, argv, i, num_threads, static_cast<uint32_t>(1),
                           static_cast<uint32_t>(std::thread::hardware_concurrency())))
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

            explicit_thread_count = true; // User explicitly set the thread count
        }
        else if (arg == "--disable-optimized-spsc")
        {
            disable_optimized_spsc = true;
        }
        else if (arg == "-o" || arg == "--output")
        {
            if (i + 1 < argc)
            {
                std::filesystem::path output_path(argv[++i]);

                // Validate the output path
                if (!output_file_path.empty() &&
                    (!std::filesystem::exists(output_path) || !std::filesystem::is_directory(output_path)))
                {
                    std::cerr << "Error: Output file path does not exist: " << output_path << std::endl;
                    return 1;
                }

                // Ensure the output path ends with a separator
                output_file_path = output_path.string();
                if (!output_file_path.empty() && output_file_path.back() != std::filesystem::path::preferred_separator)
                    output_file_path += std::filesystem::path::preferred_separator;

                export_results = true;
            }
            else
            {
                std::cerr << "Error: No output file path specified." << std::endl;
                return 1;
            }
        }
        else if (arg == "--throughput")
        {
            run_throughput = true;
        }
        else if (arg == "--latency")
        {
            run_latency = true;
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

    // If neither of the scope flags were provided, default to running all benchmarks.
    if (!run_throughput && !run_latency)
    {
        run_throughput = true;
        run_latency = true;
    }

    std ::vector<BenchmarkResult> results;
    results.reserve(prod_range.max * cons_range.max);

    for (uint8_t p = prod_range.start; p <= prod_range.max; p += prod_range.step)
    {
        for (uint8_t c = cons_range.start; c <= cons_range.max; c += cons_range.step)
        {
            // If the user hasn't set a number of threads, default to a value that covers
            // all producers and consumers.
            if (!explicit_thread_count)
                num_threads = std::min(static_cast<unsigned int>(p + c), std::thread::hardware_concurrency());

            if (num_threads < p + c)
                std::cout << "Warning: Thread oversubscription detected.\n"
                             "Multiple producers/consumers will share threads. Expect reduced performance.\n"
                             "Target thread count: "
                          << num_threads << std::endl;

            if (num_threads > std::thread::hardware_concurrency())
                std::cout << "Warning: Target thread count exceeds hardware concurrency. "
                          << "Limiting to " << std::thread::hardware_concurrency() << "." << std::endl;

            switch (parse_mode(benchmark_mode))
            {
                case TestMode::SIMPLE:
                    results.emplace_back(run_simple_benchmark(num_iter, num_threads));
                    break;
                case TestMode::COMPLEX:
                    results.emplace_back(
                        run_complex_benchmark(p, c, num_iter, num_threads, run_throughput, run_latency));
                    break;
                case TestMode::STL:
                    results.emplace_back(
                        run_stl_queue_benchmark(p, c, num_iter, num_threads, run_throughput, run_latency));
                    break;
                case TestMode::BOOST_LFQ:
                    results.emplace_back(run_boost_lockfree_queue_benchmark(p, c, num_iter, num_threads, run_throughput,
                                                                            run_latency, disable_optimized_spsc));
                    break;
                case TestMode::BOOST_SYNC_BOUNDED:
                    results.emplace_back(run_boost_sync_bounded_queue_benchmark(p, c, num_iter, num_threads,
                                                                                run_throughput, run_latency));
                    break;
#ifdef ENABLE_FOLLY_BENCHMARKS
                case TestMode::FOLLYQ:
                    results.emplace_back(run_follyq_benchmark(p, c, num_iter, num_threads, run_throughput, run_latency,
                                                              disable_optimized_spsc));
                    break;
#endif
                default:
                    std::cerr << "Unknown mode provided: " << argv[1] << std::endl;
                    return 1;
            }
        }
    }

    // Export results to CSV
    if (export_results)
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        const auto local_time = std::localtime(&now);

        std::string mode_suffix = "_" + benchmark_mode;
        if (benchmark_mode == "simple" || benchmark_mode == "complex")
            mode_suffix += QUEUE_MODE == queues::QueueMode::RETRY_NEW ? "_rn" : "_do";

        std::ostringstream oss;
        oss << "benchmark_results_" << std::put_time(local_time, "%Y-%m-%d-%H-%M") << mode_suffix << ".csv";
        std::string csv_filename = oss.str();

        export_to_csv(output_file_path + csv_filename, results);
    }

    return 0;
}