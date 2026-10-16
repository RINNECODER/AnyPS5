#include "MetalTestSupport.hpp"
#include "MetalDrawTranslationCache.hpp"

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

}

void RunDrawTranslationCacheTests(const MetalTests::Context& /*context*/) {
    TestBasicLookupAndInsert();
    TestLruEviction();
}
