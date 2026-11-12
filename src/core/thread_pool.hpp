// A small thread pool with a single operation: run a loop body in parallel.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace termesh {

class ThreadPool {
public:
    // `threads` is the total number of threads that take part in a
    // parallelFor, including the caller. 0 means "one per hardware thread".
    explicit ThreadPool(unsigned threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned threadCount() const { return static_cast<unsigned>(workers_.size()) + 1; }

    // Call body(i) once for every i in [0, count), spread over the pool, and
    // return when all calls have finished. Iterations are handed out one at a
    // time from a shared atomic counter, so uneven workloads (a tile full of
    // triangles next to an empty one) balance themselves.
    void parallelFor(int count, const std::function<void(int)>& body);

private:
    void workerLoop();
    void runJob();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;   // workers wait here for a new job
    std::condition_variable done_;   // the caller waits here for stragglers

    // The current job. `generation_` changes whenever a new one is posted so
    // that a worker never runs the same job twice.
    const std::function<void(int)>* body_ = nullptr;
    int count_ = 0;
    std::atomic<int> next_{0};
    unsigned generation_ = 0;
    unsigned active_ = 0;  // workers currently inside runJob()
    bool stopping_ = false;
};

}  // namespace termesh
