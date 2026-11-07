#include "dsptb/core/worker_pool.hpp"

namespace dsptb {

WorkerPool::WorkerPool(unsigned threads) {
    for (unsigned i = 0; i < (threads ? threads : 1); ++i)
        threads_.emplace_back([this] { loop(); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_)
        t.join();
}

void WorkerPool::submit(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void WorkerPool::loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty())
                return; // stopping and drained
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

} // namespace dsptb
