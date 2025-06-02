#include <queue/bbq.h>

#include <iostream>

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

int main()
{
    queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW> queue(4, 8);

    // Enqueue a single element
    queue.enqueue(42);

    // Dequeue the element
    auto [data, status] = queue.dequeue();
    if (status == queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::OK)
        std::cout << "Dequeued: " << data.value() << std::endl;
    else
        std::cout << "Error: Dequeue failed | Status: " << status_to_string(status) << std::endl;

    // Attempt to dequeue again. Should return EMPTY since the queue is now empty.
    auto [data2, status2] = queue.dequeue();
    if (status2 == queues::BlockBoundedQueue<int, queues::QueueMode::RETRY_NEW>::Status::OK)
        std::cout << "Dequeued: " << data2.value() << std::endl;
    else
        std::cout << "Error: Dequeue failed | Status: " << status_to_string(status2) << std::endl;

    std::cout << "Hello, World!" << std::endl;
    return 0;
}