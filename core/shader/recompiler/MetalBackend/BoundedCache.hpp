#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace ShaderRecompiler::MetalBackend {

struct CacheCounters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

// Thread-safe least-recently-used map from exact byte keys to values, bounded by entry count and by
// key-plus-value bytes. Keys are compared in full (a hash only picks the bucket), so two different
// inputs can never share an entry.
template<class Value>
class BoundedCache {
public:
    BoundedCache(std::size_t maxEntries, std::size_t maxBytes) : maxEntries(maxEntries), maxBytes(maxBytes) {}

    [[nodiscard]] std::optional<Value> Find(const std::string& key) {
        std::lock_guard lock(mutex);
        const auto found = entries.find(key);
        if (found == entries.end()) {
            ++counters.misses;
            return std::nullopt;
        }
        ++counters.hits;
        order.splice(order.begin(), order, found->second.position);
        return found->second.value;
    }

    // Stores `value` unless the key is already present (a concurrent miss built an equivalent value) or the
    // entry alone exceeds the byte bound. Returns the value now cached for the key.
    Value Insert(std::string key, Value value, std::size_t valueBytes) {
        std::lock_guard lock(mutex);
        if (const auto found = entries.find(key); found != entries.end()) return found->second.value;
        const std::size_t weight = key.size() + valueBytes;
        if (weight > maxBytes || maxEntries == 0) return value;
        auto [entry, inserted] = entries.emplace(std::move(key), Entry{value, weight, {}});
        order.push_front(&entry->first);
        entry->second.position = order.begin();
        counters.bytes += weight;
        while (entries.size() > maxEntries || counters.bytes > maxBytes) {
            const auto victim = entries.find(*order.back());
            counters.bytes -= victim->second.weight;
            order.pop_back();
            entries.erase(victim);
        }
        return value;
    }

    void Clear() {
        std::lock_guard lock(mutex);
        order.clear();
        entries.clear();
        counters = {};
    }

    [[nodiscard]] CacheCounters Counters() const {
        std::lock_guard lock(mutex);
        auto result = counters;
        result.entries = entries.size();
        return result;
    }

private:
    struct Entry {
        Value value;
        std::size_t weight;
        std::list<const std::string*>::iterator position;
    };

    const std::size_t maxEntries;
    const std::size_t maxBytes;
    mutable std::mutex mutex;
    std::list<const std::string*> order;
    std::unordered_map<std::string, Entry> entries;
    CacheCounters counters;
};

// Appends fixed-width values to an exact cache key.
class CacheKeyWriter {
public:
    template<class T>
    void Value(const T& value) {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T> || std::is_floating_point_v<T>, "key fields must be scalar");
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    void Bool(bool value) { Value<std::uint8_t>(value ? 1u : 0u); }
    void Bytes(const void* data, std::size_t size) {
        Value<std::uint64_t>(size);
        bytes.append(static_cast<const char*>(data), size);
    }
    void Text(const std::string& text) { Bytes(text.data(), text.size()); }
    [[nodiscard]] std::string Take() { return std::move(bytes); }

private:
    std::string bytes;
};

}
