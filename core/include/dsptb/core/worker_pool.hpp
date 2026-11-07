#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace dsptb {

// A small fixed pool for non-real-time background work: file decode, peak overviews, later offline jobs and
// export. Tasks run in submission order per free thread.
class WorkerPool {
public:
    explicit WorkerPool(unsigned threads = 2);
    ~WorkerPool(); // finishes queued tasks, then joins

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void submit(std::function<void()> task);

private:
    void loop();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

} // namespace dsptb
