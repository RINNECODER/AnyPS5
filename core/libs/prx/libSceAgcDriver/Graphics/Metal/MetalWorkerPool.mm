#include "MetalWorkerPool.hpp"
#include <pthread.h>
#include <cstdlib>

namespace AgcDriver::Metal {

namespace {
thread_local bool poolThread = false;
}

bool MetalWorkerPool::OnPoolThread() { return poolThread; }

std::size_t MetalWorkerPool::DefaultWorkerCount() {
    const char* env = std::getenv("ANYPS5_METAL_WORKER_THREADS");
    if (env != nullptr) {
        int val = std::atoi(env);
        if (val > 0) return static_cast<std::size_t>(val);
    }
    const auto hc = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hc > 0 ? hc / 2 : 2, 2, 8);
}

MetalWorkerPool& MetalWorkerPool::Shared() {
    static MetalWorkerPool pool(DefaultWorkerCount());
    return pool;
}

MetalWorkerPool::MetalWorkerPool(std::size_t count) {
    workers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers.emplace_back([this, i] {
            poolThread = true;
            char name[32];
            std::snprintf(name, sizeof(name), "agc-metal-w%zu", i);
            pthread_setname_np(name);
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(queueMutex);
                    cv.wait(lock, [this] { return stopping.load(std::memory_order_acquire) || !tasks.empty(); });
                    if (stopping.load(std::memory_order_acquire) && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                if (task) {
                    try {
                        task();
                    } catch (...) {
                    }
                }
            }
        });
    }
}

MetalWorkerPool::~MetalWorkerPool() {
    stopping.store(true, std::memory_order_release);
    cv.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
}

void MetalWorkerPool::Enqueue(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard lock(queueMutex);
        if (stopping.load(std::memory_order_acquire)) return;
        tasks.push(std::move(task));
    }
    cv.notify_one();
}

void MetalWorkerPool::ParallelFor(std::size_t count, const std::function<void(std::size_t index)>& task) {
    if (count == 0 || !task) return;
    if (count == 1 || workers.empty()) {
        task(0);
        return;
    }
    const std::size_t numTasks = count;
    std::atomic<std::size_t> remaining{numTasks};
    std::mutex doneMutex;
    std::condition_variable doneCv;

    for (std::size_t i = 1; i < numTasks; ++i) {
        Enqueue([i, &task, &remaining, &doneMutex, &doneCv] {
            try {
                task(i);
            } catch (...) {
            }
            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard lock(doneMutex);
                doneCv.notify_one();
            }
        });
    }

    try {
        task(0);
    } catch (...) {
    }

    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        return;
    }

    std::unique_lock lock(doneMutex);
    doneCv.wait(lock, [&] { return remaining.load(std::memory_order_acquire) == 0; });
}

}
