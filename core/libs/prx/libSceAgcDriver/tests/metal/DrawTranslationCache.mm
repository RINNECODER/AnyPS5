#include "MetalTestSupport.hpp"
#include "MetalDrawTranslationCache.hpp"
#include "MetalWorkerPool.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::Metal;
using MetalTests::Require;

void TestBasicLookupAndInsert() {
    MetalDrawTranslationCache cache;
    Require(cache.Size() == 0, "Initial cache size must be 0");
    Require(MetalDrawTranslationCache::Enabled(), "Cache must be enabled by default");

    DrawTranslationKey key;
    key.vertexVariantId = 101;
    key.fragmentVariantId = 202;
    key.topology = 3;
    key.rasterSamples = 1;
    key.guestDescriptorWords = {0x12345678, 0x9abcdef0};

    // Miss on empty cache
    auto found = cache.Find(key, 0, 0);
    Require(found == nullptr, "Empty cache must return nullptr on Find");
    auto stats = cache.TakeStatistics(false);
    Require(stats.lookups == 1, "Lookups must be 1");
    Require(stats.misses == 1, "Misses must be 1");
    Require(stats.hits == 0, "Hits must be 0");

    // Insert record
    auto record = std::make_shared<DrawTranslationRecord>();
    record->key = key;
    record->memoryVersion = 0;
    record->imageCacheClears = 0;
    cache.Insert(record);

    Require(cache.Size() == 1, "Cache size must be 1 after insertion");
    Require(record->recordId > 0, "Inserted record must receive non-zero recordId");

    // Hit on identical key and matching versions
    found = cache.Find(key, 0, 0);
    Require(found != nullptr, "Find must return cached record");
    Require(found->recordId == record->recordId, "Record ID must match");
    stats = cache.TakeStatistics(true);
    Require(stats.hits == 1, "Hits must be 1");
    Require(stats.inserts == 1, "Inserts must be 1");

    // Miss on memory version change
    found = cache.Find(key, 1, 0);
    Require(found == nullptr, "Memory version mismatch must cause cache miss");

    // Miss on image cache clear change
    found = cache.Find(key, 0, 1);
    Require(found == nullptr, "Image cache clear mismatch must cause cache miss");

    // Miss on modified key
    auto modifiedKey = key;
    modifiedKey.guestDescriptorWords[0] ^= 0x1;
    found = cache.Find(modifiedKey, 0, 0);
    Require(found == nullptr, "Key mismatch must cause cache miss");

    // State elision and reset
    cache.RecordStateElision();
    cache.RecordStateElision();
    stats = cache.TakeStatistics(true);
    Require(stats.redundantStateElisions == 2, "State elision count must be 2");

    stats = cache.TakeStatistics(true);
    Require(stats.redundantStateElisions == 0, "State elision count must be reset");

    // Clear
    cache.Clear();
    Require(cache.Size() == 0, "Cache size must be 0 after Clear");
    stats = cache.TakeStatistics(true);
    Require(stats.clears == 1, "Clears count must be 1");
}

// Live() gates the parallel batch scan: it is true only while the cache serves records at the
// version it is at now. Inserting is not enough (an image-cache expire can move the version before
// the next lookup), so only a hit makes it live, a version mismatch and Clear make it dead.
void TestLiveGate() {
    MetalDrawTranslationCache cache;
    Require(!cache.Live(), "An empty cache must not be live");

    DrawTranslationKey key;
    key.vertexVariantId = 7;
    key.fragmentVariantId = 9;
    auto record = std::make_shared<DrawTranslationRecord>();
    record->key = key;
    record->memoryVersion = 0;
    record->imageCacheClears = 0;
    cache.Insert(record);
    Require(!cache.Live(), "An insert alone must not make the cache live");

    Require(cache.Find(key, 0, 0) != nullptr, "A record at the current version must be served");
    Require(cache.Live(), "Serving a record must make the cache live");

    Require(cache.Find(key, 1, 0) == nullptr, "A record at another version must not be served");
    Require(!cache.Live(), "A version mismatch must make the cache dead");

    Require(cache.Find(key, 0, 0) != nullptr, "The record is served again at its own version");
    Require(cache.Live(), "Serving it again must make the cache live");
    cache.Clear();
    Require(!cache.Live(), "Clear must make the cache dead");
}

void TestLruEviction() {
    MetalDrawTranslationCache cache;
    // Insert MaxEntries + 1 items
    for (std::size_t i = 0; i <= MetalDrawTranslationCache::MaxEntries; ++i) {
        DrawTranslationKey key;
        key.vertexVariantId = i + 1;
        auto record = std::make_shared<DrawTranslationRecord>();
        record->key = key;
        record->memoryVersion = 0;
        record->imageCacheClears = 0;
        cache.Insert(record);
    }
    Require(cache.Size() == MetalDrawTranslationCache::MaxEntries, "Cache size must be clamped at MaxEntries");

    // The first item should have been evicted
    DrawTranslationKey firstKey;
    firstKey.vertexVariantId = 1;
    auto found = cache.Find(firstKey, 0, 0);
    Require(found == nullptr, "Oldest record must be evicted");

    // The last item should still be present
    DrawTranslationKey lastKey;
    lastKey.vertexVariantId = MetalDrawTranslationCache::MaxEntries + 1;
    found = cache.Find(lastKey, 0, 0);
    Require(found != nullptr, "Most recent record must be present");
}

void TestThreadSafety() {
    MetalDrawTranslationCache cache;
    constexpr int ThreadCount = 8;
    constexpr int OperationsPerThread = 200;
    std::vector<std::thread> threads;
    std::atomic<int> foundCount{0};

    for (int t = 0; t < ThreadCount; ++t) {
        threads.emplace_back([&cache, &foundCount, t] {
            for (int i = 0; i < OperationsPerThread; ++i) {
                DrawTranslationKey key;
                key.vertexVariantId = (t * OperationsPerThread + i) % 50 + 1;
                key.fragmentVariantId = 100;
                key.topology = 3;
                key.rasterSamples = 1;

                if (i % 2 == 0) {
                    auto record = std::make_shared<DrawTranslationRecord>();
                    record->key = key;
                    record->memoryVersion = 0;
                    record->imageCacheClears = 0;
                    cache.Insert(record);
                } else {
                    auto found = cache.Find(key, 0, 0);
                    if (found != nullptr) {
                        foundCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                if (i % 50 == 0) {
                    cache.RecordStateElision();
                    static_cast<void>(cache.Contains(key));
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    Require(cache.Size() <= MetalDrawTranslationCache::MaxEntries, "Cache size must not exceed MaxEntries under concurrent writes");
    const auto stats = cache.TakeStatistics(true);
    Require(stats.inserts > 0, "Inserts must be recorded");
    Require(stats.lookups > 0, "Lookups must be recorded");
}

void TestWorkerPool() {
    auto& pool = MetalWorkerPool::Shared();
    Require(pool.WorkerCount() >= 1, "Worker pool must have at least 1 worker");

    std::atomic<int> parallelSum{0};
    constexpr std::size_t ParallelIterations = 64;
    pool.ParallelFor(ParallelIterations, [&parallelSum](std::size_t index) {
        parallelSum.fetch_add(static_cast<int>(index + 1), std::memory_order_relaxed);
    });

    constexpr int ExpectedSum = static_cast<int>(ParallelIterations * (ParallelIterations + 1) / 2);
    Require(parallelSum.load() == ExpectedSum, "ParallelFor must execute all iterations correctly");

    std::atomic<int> enqueueCounter{0};
    std::mutex mtx;
    std::condition_variable cv;
    constexpr int EnqueueCount = 20;

    for (int i = 0; i < EnqueueCount; ++i) {
        pool.Enqueue([&enqueueCounter, &mtx, &cv, EnqueueCount] {
            if (enqueueCounter.fetch_add(1, std::memory_order_relaxed) + 1 == EnqueueCount) {
                std::lock_guard lock(mtx);
                cv.notify_one();
            }
        });
    }

    std::unique_lock lock(mtx);
    cv.wait_for(lock, std::chrono::seconds(5), [&] { return enqueueCounter.load() == EnqueueCount; });
    Require(enqueueCounter.load() == EnqueueCount, "All enqueued jobs must complete");
}

}

void RunDrawTranslationCacheTests(const MetalTests::Context& /*context*/) {
    TestBasicLookupAndInsert();
    TestLiveGate();
    TestLruEviction();
    TestThreadSafety();
    TestWorkerPool();
}
