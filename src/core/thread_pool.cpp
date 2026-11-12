#include "core/thread_pool.hpp"

#include <algorithm>

namespace termesh {

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    workers_.reserve(threads - 1);
    for (unsigned i = 1; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) worker.join();
}

void ThreadPool::runJob() {
    for (;;) {
        const int i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count_) break;
        (*body_)(i);
    }
}

void ThreadPool::parallelFor(int count, const std::function<void(int)>& body) {
    if (count <= 0) return;
    if (count == 1 || workers_.empty()) {
        for (int i = 0; i < count; ++i) body(i);
        return;
    }

    {
        std::lock_guard lock(mutex_);
        body_ = &body;
        count_ = count;
        next_.store(0, std::memory_order_relaxed);
        ++generation_;
    }
    wake_.notify_all();

    runJob();  // the calling thread does its share

    // The counter is exhausted, but workers may still be inside their last
    // iteration. Wait for them, then retire the job so late wakers skip it.
    std::unique_lock lock(mutex_);
    done_.wait(lock, [this] { return active_ == 0; });
    body_ = nullptr;
}

void ThreadPool::workerLoop() {
    unsigned seen = 0;
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stopping_ || (generation_ != seen && body_ != nullptr); });
        if (stopping_) return;
        seen = generation_;
        ++active_;
        lock.unlock();

        runJob();

        lock.lock();
        if (--active_ == 0) done_.notify_one();
    }
}

}  // namespace termesh
