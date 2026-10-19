#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace AgcDriver::Metal {

class MetalWorkerPool {
public:
    static MetalWorkerPool& Shared();
    static std::size_t DefaultWorkerCount();

    explicit MetalWorkerPool(std::size_t count);
    ~MetalWorkerPool();

    MetalWorkerPool(const MetalWorkerPool&) = delete;
    MetalWorkerPool& operator=(const MetalWorkerPool&) = delete;

    void Enqueue(std::function<void()> task);
    void ParallelFor(std::size_t count, const std::function<void(std::size_t index)>& task);

    [[nodiscard]] static bool OnPoolThread();
    [[nodiscard]] std::size_t WorkerCount() const { return workers.size(); }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::atomic<bool> stopping{false};
};

}
