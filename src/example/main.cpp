#include <queue/bbq.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>
#include <vector>

std::string status_to_string(queues::OpStatus status)
{
    switch (status)
    {
        case queues::OpStatus::OK:
            return "OK";
        case queues::OpStatus::FULL:
            return "FULL";
        case queues::OpStatus::EMPTY:
            return "EMPTY";
        case queues::OpStatus::BUSY:
            return "BUSY";
        default:
            return "UNKNOWN";
    }
}

int64_t get_time_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr std::size_t CACHELINE_SIZE = std::hardware_destructive_interference_size;

using q_val_t = uint64_t;

constexpr queues::QueueMode QUEUE_MODE = queues::QueueMode::DROP_OLD;

constexpr uint64_t SPSC_ITERS = 100000000;
constexpr uint64_t SPSC_CAPACITY = 10000;
constexpr uint64_t SPSC_NUM_BLOCKS = 8;

queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> spsc_queue(SPSC_NUM_BLOCKS, SPSC_CAPACITY);

void* spsc_writer(void* arg)
{
    std::atomic<bool>* writer_done = static_cast<std::atomic<bool>*>(arg);

    for (uint64_t i = 0; i < SPSC_ITERS; i++)
        while (spsc_queue.enqueue(i) != queues::OpStatus::OK)
            ; // busy-wait

    if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
    {
        // Signal that the writer is done
        writer_done->store(true, std::memory_order_release);
    }

    return nullptr;
}

void* spsc_reader(void* arg)
{
    std::atomic<bool>* writer_done = static_cast<std::atomic<bool>*>(arg);

    if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
    {
        while (true)
        {
            std::optional<uint64_t> buf;
            queues::OpStatus status;
            do
            {
                auto result = spsc_queue.dequeue();
                buf = result.first;
                status = result.second;

                if (status == queues::OpStatus::EMPTY && writer_done->load(std::memory_order_acquire))
                    return nullptr;
            } while (status != queues::OpStatus::OK);

            // In drop-old mode, we can't guarantee that the reader will dequeue all items.
            if (!buf.has_value())
                abort();
        }
    }
    else
    {
        for (uint64_t i = 0; i < SPSC_ITERS; i++)
        {
            std::optional<uint64_t> buf;
            queues::OpStatus status;
            do
            {
                auto result = spsc_queue.dequeue();
                buf = result.first;
                status = result.second;
            } while (status != queues::OpStatus::OK);

            // Verify that the dequeued value matches the expected value
            if (!buf.has_value() || buf.value() != i)
                abort();
        }
    }

    return nullptr;
}

// Note: Based on the simple SPSC benchmark outlined in the academic paper.
void run_spsc_test()
{
    pthread_t t_writer, t_reader;
    alignas(CACHELINE_SIZE) std::atomic<bool> writer_done = false;
    std::cout << "[SPSC] Starting SPSC test with " << SPSC_ITERS << " iterations." << std::endl;
    std::cout << "[SPSC] Queue mode: " << (QUEUE_MODE == queues::QueueMode::DROP_OLD ? "DROP OLD" : "RETRY NEW")
              << std::endl;

    auto begin = std::chrono::steady_clock::now();

    pthread_create(&t_writer, nullptr, spsc_writer, &writer_done);
    pthread_create(&t_reader, nullptr, spsc_reader, &writer_done);
    pthread_join(t_reader, nullptr);
    pthread_join(t_writer, nullptr);

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

    std::cout << "[SPSC] Elapsed time: " << elapsed_us << " µs (" << elapsed_ms << " ms)" << std::endl;

    uint64_t total_op = SPSC_ITERS * 2;
    std::cout << "[SPSC] :: Throughput = " << total_op / (elapsed_us / 1'000'000.0f) << " op/s." << std::endl;
}

constexpr uint64_t NUM_PRODUCERS = 1;
constexpr uint64_t NUM_CONSUMERS = 1;
constexpr uint64_t MPSC_ITEMS_PER_PROD = 250000;
constexpr uint64_t MPSC_CAPACITY = 10000;
constexpr uint64_t MPSC_NUM_BLOCKS = 8;
constexpr bool MPSC_CHECK_CONSUMED = false;

queues::BlockBoundedQueue<q_val_t, QUEUE_MODE> mpsc_queue(MPSC_NUM_BLOCKS, MPSC_CAPACITY);

void mpsc_producer(std::atomic<uint32_t>& enq_counter, uint8_t prod_id, bool& start_flag, std::condition_variable& cv,
                   std::mutex& mtx)
{
    // Wait for the start signal
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return start_flag; });
    }

    for (uint32_t j = 0; j < MPSC_ITEMS_PER_PROD; j++)
    {
        q_val_t item_val = (prod_id * MPSC_ITEMS_PER_PROD) + j;
        while (mpsc_queue.enqueue(item_val) != queues::OpStatus::OK)
            std::this_thread::yield();

        enq_counter.fetch_add(1, std::memory_order_relaxed);
    }
}

void mpsc_consumer(std::atomic<uint32_t>& deq_counter, std::vector<q_val_t>& enq_items, std::mutex& queue_mutex,
                   bool& start_flag, std::condition_variable& cv, std::mutex& mtx, std::atomic<bool>& producers_done)
{
    // Wait for the start signal
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return start_flag; });
    }

    if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
    {
        while (true)
        {
            auto [data, status] = mpsc_queue.dequeue();
            if (status == queues::OpStatus::OK)
            {
                if (!data.has_value())
                    abort();

                if constexpr (MPSC_CHECK_CONSUMED)
                {
                    std::scoped_lock lock(queue_mutex);
                    enq_items.emplace_back(data.value());
                }

                deq_counter.fetch_add(1, std::memory_order_relaxed);
            }
            else if (status == queues::OpStatus::EMPTY)
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
        const uint32_t expected_item_count = NUM_PRODUCERS * MPSC_ITEMS_PER_PROD;
        while (deq_counter.load(std::memory_order_acquire) < expected_item_count)
        {
            auto [data, status] = mpsc_queue.dequeue();
            if (status == queues::OpStatus::OK)
            {
                if (!data.has_value())
                    abort(); // This should never happen in a well-formed queue

                if constexpr (MPSC_CHECK_CONSUMED)
                {
                    std::scoped_lock lock(queue_mutex);
                    enq_items.emplace_back(data.value());
                }

                deq_counter.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    }
}

void run_mpsc_test()
{
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    alignas(CACHELINE_SIZE) std::atomic<uint32_t> enq_counter = 0;
    alignas(CACHELINE_SIZE) std::atomic<uint32_t> deq_counter = 0;
    alignas(CACHELINE_SIZE) std::atomic<bool> producers_done = false;

    std::vector<q_val_t> enq_items;
    enq_items.reserve(NUM_PRODUCERS * MPSC_ITEMS_PER_PROD);
    std::mutex queue_mutex;

    std::condition_variable start_cv;
    std::mutex start_mutex;
    bool start_flag = false;

    std::cout << "[MPSC] Starting MPSC test with " << NUM_PRODUCERS << " producers and " << NUM_CONSUMERS
              << " consumers." << std::endl;
    std::cout << "[MPSC] " << MPSC_ITEMS_PER_PROD << " items/producer. Total: " << (NUM_PRODUCERS * MPSC_ITEMS_PER_PROD)
              << " items." << std::endl;
    std::cout << "[MPSC] Queue mode: " << (QUEUE_MODE == queues::QueueMode::DROP_OLD ? "DROP OLD" : "RETRY NEW")
              << std::endl;

    // Start producer threads
    for (uint8_t i = 0; i < NUM_PRODUCERS; i++)
        producers.emplace_back(mpsc_producer, std::ref(enq_counter), i, std::ref(start_flag), std::ref(start_cv),
                               std::ref(start_mutex));

    // Start consumer threads
    for (uint8_t i = 0; i < NUM_CONSUMERS; i++)
        consumers.emplace_back(mpsc_consumer, std::ref(deq_counter), std::ref(enq_items), std::ref(queue_mutex),
                               std::ref(start_flag), std::ref(start_cv), std::ref(start_mutex),
                               std::ref(producers_done));

    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Give threads time to reach spin-wait
    const auto start{std::chrono::steady_clock::now()};
    {
        std::scoped_lock lock(start_mutex);
        start_flag = true;
    }
    start_cv.notify_all(); // Notify all threads to start processing

    // Wait for all threads to finish executing
    for (auto& producer_thread : producers)
        producer_thread.join();

    // In drop-old mode, signal to the consumers that producers are done enqueuing items
    if constexpr (QUEUE_MODE == queues::QueueMode::DROP_OLD)
        producers_done.store(true, std::memory_order_release);

    for (auto& consumer_thread : consumers)
        consumer_thread.join();

    const auto finish{std::chrono::steady_clock::now()};
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count();

    // Verify that all enqueued items were returned by the consumers
    if (enq_counter.load() == deq_counter.load())
        std::cout << "[MPSC] Success: Matching enqueued and dequeued item counts." << std::endl;
    else
        std::cerr << "[MPSC] Error: Mismatched enqueued and dequeued item counts!" << std::endl;

    if constexpr (MPSC_CHECK_CONSUMED)
    {
        std::unordered_set<q_val_t> seen;
        bool duplicate_found = false;
        bool missing_found = false;

        for (q_val_t v : enq_items)
        {
            auto [it, inserted] = seen.insert(v);
            if (!inserted)
            {
                std::cerr << "[MPSC] Error: Duplicate item detected: " << v << std::endl;
                duplicate_found = true;
            }
        }

        for (q_val_t i = 0; i < NUM_PRODUCERS * MPSC_ITEMS_PER_PROD; ++i)
        {
            if (seen.find(i) == seen.end())
            {
                std::cerr << "[MPSC] Error: Item " << i << " was not found in the dequeued items!" << std::endl;
                missing_found = true;
            }
        }

        if (!duplicate_found && !missing_found)
            std::cout << "[MPSC] All expected items were found exactly once." << std::endl;
        else
            std::cerr << "[MPSC] Error: Issues detected in dequeued items (see above)." << std::endl;
    }

    std::cout << "[MPSC] Elapsed time: " << elapsed_us << " µs (" << elapsed_ms << " ms)" << std::endl;
    std::cout << "[MPSC] Expected items: " << (NUM_PRODUCERS * MPSC_ITEMS_PER_PROD) << std::endl;
    std::cout << "[MPSC] Actual(ENQ): " << enq_counter.load() << " | Actual(DEQ): " << deq_counter.load() << std::endl;

    uint64_t total_ops = static_cast<uint64_t>(enq_counter.load()) + static_cast<uint64_t>(deq_counter.load());
    std::cout << "[MPSC] :: Throughput = " << total_ops / (elapsed_us / 1'000'000.0f) << " op/s." << std::endl;
}

void run_stl_queue_test()
{
    std::queue<q_val_t> stl_queue;
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    alignas(CACHELINE_SIZE) std::atomic<uint32_t> enq_counter = 0;
    alignas(CACHELINE_SIZE) std::atomic<uint32_t> deq_counter = 0;
    std::mutex stl_mtx;
    std::condition_variable stl_cv;

    std::cout << "[STL] Starting mutex-based STL queue test with " << NUM_PRODUCERS << " producers and "
              << NUM_CONSUMERS << " consumers." << std::endl;
    std::cout << "[STL] " << MPSC_ITEMS_PER_PROD << " items/producer. Total: " << (NUM_PRODUCERS * MPSC_ITEMS_PER_PROD)
              << " items." << std::endl;

    const auto stl_start{std::chrono::steady_clock::now()};

    // Producers for STL queue
    for (uint8_t i = 0; i < NUM_PRODUCERS; i++)
    {
        producers.emplace_back(
            [&, i]()
            {
                for (uint32_t j = 0; j < MPSC_ITEMS_PER_PROD; j++)
                {
                    q_val_t item_val = (i * MPSC_ITEMS_PER_PROD) + j;
                    {
                        std::unique_lock<std::mutex> lock(stl_mtx);
                        stl_queue.push(item_val);
                    }
                    enq_counter.fetch_add(1, std::memory_order_relaxed);
                    stl_cv.notify_one();
                }
            });
    }

    // Consumers for STL queue
    for (uint8_t i = 0; i < NUM_CONSUMERS; i++)
    {
        consumers.emplace_back(
            [&, i]()
            {
                const uint32_t expected_item_count = NUM_PRODUCERS * MPSC_ITEMS_PER_PROD;
                while (deq_counter.load(std::memory_order_acquire) < expected_item_count)
                {
                    std::unique_lock<std::mutex> lock(stl_mtx);
                    stl_cv.wait(lock, [&] { return !stl_queue.empty() || deq_counter.load() >= expected_item_count; });
                    if (!stl_queue.empty())
                    {
                        int val = stl_queue.front();
                        stl_queue.pop();
                        deq_counter.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }

    for (auto& producer_thread : producers)
        producer_thread.join();
    for (auto& consumer_thread : consumers)
        consumer_thread.join();

    const auto stl_finish{std::chrono::steady_clock::now()};
    const auto stl_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(stl_finish - stl_start).count();
    const auto stl_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stl_finish - stl_start).count();

    // Verify STL queue results
    if (enq_counter.load() == deq_counter.load())
        std::cout << "[STL] Success: Matching enqueued and dequeued item counts." << std::endl;
    else
        std::cerr << "[STL] Error: Mismatched enqueued and dequeued item counts!" << std::endl;

    std::cout << "[STL] Elapsed time: " << stl_elapsed_us << " µs (" << stl_elapsed_ms << " ms)" << std::endl;
    std::cout << "[STL] Expected items: " << (NUM_PRODUCERS * MPSC_ITEMS_PER_PROD) << std::endl;
    std::cout << "[STL] Actual(ENQ): " << enq_counter.load() << " | Actual(DEQ): " << deq_counter.load() << std::endl;
    uint64_t stl_total_ops = static_cast<uint64_t>(enq_counter.load()) + static_cast<uint64_t>(deq_counter.load());
    std::cout << "[STL] :: Throughput = " << stl_total_ops / (stl_elapsed_us / 1'000'000.0f) << " op/s." << std::endl;
}

enum class TestMode
{
    SPSC,
    MPSC,
    STL,
    UNKNOWN
};

TestMode parse_mode(const std::string& mode)
{
    if (mode == "spsc")
        return TestMode::SPSC;
    if (mode == "mpsc")
        return TestMode::MPSC;
    if (mode == "stl")
        return TestMode::STL;

    return TestMode::UNKNOWN;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " [spsc|mpsc|stl]" << std::endl;
        return 1;
    }

    for (int i = 1; i < argc; ++i)
    {
        switch (parse_mode(argv[i]))
        {
            case TestMode::SPSC:
                run_spsc_test();
                break;
            case TestMode::MPSC:
                run_mpsc_test();
                break;
            case TestMode::STL:
                run_stl_queue_test();
                break;
            default:
                std::cerr << "Unknown mode: " << argv[1] << std::endl;
                return 1;
        }

        // Divider to separate the bechmark outputs
        if (i < argc - 1)
            std::cout << "----------------------------------------" << std::endl;
    }

    return 0;
}