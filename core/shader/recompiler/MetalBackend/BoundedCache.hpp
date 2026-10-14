#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

// An exact cache key and its hash, computed once when the key is built. A key may carry a hash the
// caller already knows for (part of) its bytes; a wrong hash can only make a lookup miss, because
// keys are always compared in full.
class CacheKey {
public:
    CacheKey() = default;
    explicit CacheKey(std::string bytes) : bytes(std::move(bytes)), hash(std::hash<std::string_view>{}(this->bytes)) {}
    CacheKey(std::string bytes, std::size_t hash) : bytes(std::move(bytes)), hash(hash) {}

    [[nodiscard]] const std::string& Bytes() const { return bytes; }
    [[nodiscard]] std::size_t Hash() const { return hash; }
    [[nodiscard]] std::size_t Size() const { return bytes.size(); }
    bool operator==(const CacheKey& other) const { return hash == other.hash && bytes == other.bytes; }

private:
    std::string bytes;
    std::size_t hash = 0;
};

// A key looked up without building it: a short head followed by a long tail (a shader's source, say),
// with the hash its CacheKey would carry.
struct CacheKeyProbe {
    std::string_view head;
    std::string_view tail;
    std::size_t hash;
};

struct CacheKeyHash {
    using is_transparent = void;
    std::size_t operator()(const CacheKey& key) const { return key.Hash(); }
    std::size_t operator()(const CacheKeyProbe& probe) const { return probe.hash; }
};

struct CacheKeyEqual {
    using is_transparent = void;
    bool operator()(const CacheKey& left, const CacheKey& right) const { return left == right; }
    bool operator()(const CacheKeyProbe& probe, const CacheKey& key) const { return Same(probe, key); }
    bool operator()(const CacheKey& key, const CacheKeyProbe& probe) const { return Same(probe, key); }

private:
    static bool Same(const CacheKeyProbe& probe, const CacheKey& key) {
        const std::string_view bytes = key.Bytes();
        return probe.hash == key.Hash() && bytes.size() == probe.head.size() + probe.tail.size() &&
            bytes.substr(0, probe.head.size()) == probe.head && bytes.substr(probe.head.size()) == probe.tail;
    }
};

// Thread-safe least-recently-used map from exact byte keys to values, bounded by entry count and by
// key-plus-value bytes. Keys are compared in full (the hash, computed once per key, only picks the
// bucket), so two different inputs can never share an entry. Values are copied out on a hit: cache
// large values through a shared pointer.
template<class Value>
class BoundedCache {
public:
    BoundedCache(std::size_t maxEntries, std::size_t maxBytes) : maxEntries(maxEntries), maxBytes(maxBytes) {}

    [[nodiscard]] std::optional<Value> Find(const CacheKey& key) { return find(key); }
    [[nodiscard]] std::optional<Value> Find(const CacheKeyProbe& key) { return find(key); }

    // Stores `value` unless the key is already present (a concurrent miss built an equivalent value) or the
    // entry alone exceeds the byte bound. Returns the value now cached for the key.
    Value Insert(CacheKey key, Value value, std::size_t valueBytes) {
        std::lock_guard lock(mutex);
        if (const auto found = entries.find(key); found != entries.end()) return found->second.value;
        const std::size_t weight = key.Size() + valueBytes;
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
        std::list<const CacheKey*>::iterator position;
    };

    template<class Key>
    std::optional<Value> find(const Key& key) {
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

    const std::size_t maxEntries;
    const std::size_t maxBytes;
    mutable std::mutex mutex;
    std::list<const CacheKey*> order;
    std::unordered_map<CacheKey, Entry, CacheKeyHash, CacheKeyEqual> entries;
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
    void Text(std::string_view text) { Bytes(text.data(), text.size()); }
    // Room for `extra` more bytes, so that a large key is not regrown (and copied) while it is written.
    void Reserve(std::size_t extra) { bytes.reserve(bytes.size() + extra); }
    [[nodiscard]] std::string Take() { return std::move(bytes); }
    [[nodiscard]] CacheKey TakeKey() { return CacheKey(std::move(bytes)); }

private:
    std::string bytes;
};

}
