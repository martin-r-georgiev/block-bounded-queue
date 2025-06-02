#include <queue/bbq.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

std::string status_to_string(queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status status)
{
    switch (status)
    {
        case queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::OK:
            return "OK";
        case queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::FULL:
            return "FULL";
        case queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::EMPTY:
            return "EMPTY";
        case queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::BUSY:
            return "BUSY";
        default:
            return "UNKNOWN";
    }
}

constexpr uint8_t NUM_PRODUCERS = 4;
constexpr uint8_t NUM_CONSUMERS = 4;
constexpr uint32_t ITEMS_PER_PRODUCER = 25000;

int main()
{
    queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW> queue(4, 128);
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    std::atomic<uint32_t> enq_item_count = 0;
    std::atomic<uint32_t> deq_item_count = 0;
    std::unordered_set<uint32_t> item_set;
    std::mutex queue_mutex;
    std::mutex log_mutex;

    const auto start{std::chrono::steady_clock::now()};

    // Start producer threads
    for (uint8_t i = 0; i < NUM_PRODUCERS; i++)
    {
        producers.emplace_back(
            [&, i]()
            {
                for (uint32_t j = 0; j < ITEMS_PER_PRODUCER; j++)
                {
                    uint32_t item_val = (i * ITEMS_PER_PRODUCER) + j;
                    while (queue.enqueue(item_val) !=
                           queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::OK)
                        std::this_thread::yield();

                    enq_item_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    // Start consumer threads
    for (uint8_t i = 0; i < NUM_CONSUMERS; i++)
    {
        consumers.emplace_back(
            [&, i]()
            {
                const uint32_t expected_item_count = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
                while (deq_item_count.load(std::memory_order_acquire) < expected_item_count)
                {
                    auto [data, status] = queue.dequeue();
                    if (status == queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::OK)
                    {
                        if (!data.has_value())
                        {
                            std::lock_guard<std::mutex> lock(log_mutex);
                            std::cerr << "Error: Dequeued an empty value!" << std::endl;
                            continue;
                        }

                        {
                            std::lock_guard<std::mutex> lock(queue_mutex);
                            item_set.insert(data.value());
                        }

                        deq_item_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        // {
                        //     std::lock_guard<std::mutex> lock(log_mutex);
                        //     std::cout << "Consumer thread " << i << " | Dequeue status: " << status_to_string(status)
                        //               << " | Enqueued item count: " << enq_item_count.load()
                        //               << " | Dequeued item count: " << deq_item_count.load() << std::endl;
                        // }

                        std::this_thread::yield();
                    }
                }
            });
    }

    // Wait for all threads to finish executing
    for (auto& producer_thread : producers)
        producer_thread.join();

    for (auto& consumer_thread : consumers)
        consumer_thread.join();

    const auto finish{std::chrono::steady_clock::now()};
    const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count();

    // Verify that all enqueued items were returned by the consumers
    if (enq_item_count.load() == deq_item_count.load())
        std::cout << "Success: Matching enqueued and dequeued item counts." << std::endl;
    else
        std::cerr << "Error: Mismatched enqueued and dequeued item counts!" << std::endl;

    for (uint32_t i = 0; i < NUM_PRODUCERS * ITEMS_PER_PRODUCER; i++)
        if (item_set.find(i) == item_set.end())
            std::cerr << "Error: Item " << i << " was not found in the dequeued items!" << std::endl;

    std::cout << "Elapsed time: " << duration_us << " us (" << duration_ms << " ms)" << std::endl;
    std::cout << "Expected item count: " << (NUM_PRODUCERS * ITEMS_PER_PRODUCER) << std::endl;
    std::cout << "Enqueued: " << enq_item_count.load() << ", Dequeued: " << deq_item_count.load() << std::endl;

    return 0;
}