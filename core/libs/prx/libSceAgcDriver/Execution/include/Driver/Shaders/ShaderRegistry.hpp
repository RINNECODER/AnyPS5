#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERREGISTRY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERREGISTRY_HPP

#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <algorithm>
#include <iterator>
#include <map>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct Shader;

namespace ShaderRecompiler {
class ShaderPreparationContext;
}

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::DriverDetail {

struct ShaderSnapshot;

class ShaderRegistry {
    using Chunk = std::map<std::uint64_t, std::vector<std::shared_ptr<const ShaderSnapshot>>>;
    using Chunks = std::vector<std::shared_ptr<Chunk>>;

public:
    using key_type = std::uint64_t;
    using mapped_type = Chunk::mapped_type;
    using value_type = Chunk::value_type;

    class const_iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ShaderRegistry::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        const_iterator() = default;
        reference operator*() const { return *entry; }
        pointer operator->() const { return &*entry; }
        const_iterator& operator++() {
            if (++entry == (*chunk)->end()) {
                ++chunk;
                entry = chunk == last ? Chunk::const_iterator{} : (*chunk)->cbegin();
            }
            return *this;
        }
        const_iterator operator++(int) {
            auto previous = *this;
            ++*this;
            return previous;
        }
        const_iterator& operator--() {
            if (chunk == last || entry == (*chunk)->cbegin()) {
                --chunk;
                entry = std::prev((*chunk)->cend());
            } else {
                --entry;
            }
            return *this;
        }
        const_iterator operator--(int) {
            auto previous = *this;
            --*this;
            return previous;
        }
        bool operator==(const const_iterator& other) const { return chunk == other.chunk && entry == other.entry; }

    private:
        friend class ShaderRegistry;
        const_iterator(Chunks::const_iterator chunk, Chunks::const_iterator last, Chunk::const_iterator entry) : chunk(chunk), last(last), entry(entry) {}
        Chunks::const_iterator chunk{};
        Chunks::const_iterator last{};
        Chunk::const_iterator entry{};
    };
    using iterator = const_iterator;

    ShaderRegistry() = default;
    ShaderRegistry(std::initializer_list<value_type> entries) {
        for (const auto& [address, headers] : entries) emplace(address, headers);
    }

    const_iterator begin() const { return chunks.empty() ? end() : const_iterator(chunks.cbegin(), chunks.cend(), chunks.front()->cbegin()); }
    const_iterator end() const { return const_iterator(chunks.cend(), chunks.cend(), {}); }
    const_iterator find(std::uint64_t address) const;
    const_iterator upper_bound(std::uint64_t address) const;
    bool contains(std::uint64_t address) const { return find(address) != end(); }
    const mapped_type& at(std::uint64_t address) const;
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    mapped_type& operator[](std::uint64_t address);
    std::pair<const_iterator, bool> emplace(std::uint64_t address, mapped_type headers);
    std::size_t erase(std::uint64_t address);
    void clear() {
        chunks.clear();
        count = 0;
    }

private:
    static constexpr std::size_t ChunkEntries = 64;

    std::size_t chunkFor(std::uint64_t address) const;
    Chunk& writable(std::size_t index);

    Chunks chunks;
    std::size_t count = 0;
};

inline std::size_t ShaderRegistry::chunkFor(std::uint64_t address) const {
    const auto above = std::upper_bound(chunks.begin(), chunks.end(), address, [](std::uint64_t key, const std::shared_ptr<Chunk>& chunk) { return key < chunk->begin()->first; });
    return above == chunks.begin() ? 0u : static_cast<std::size_t>(above - chunks.begin()) - 1u;
}

inline ShaderRegistry::Chunk& ShaderRegistry::writable(std::size_t index) {
    auto& chunk = chunks[index];
    if (chunk.use_count() != 1) chunk = std::make_shared<Chunk>(*chunk);
    return *chunk;
}

inline ShaderRegistry::const_iterator ShaderRegistry::find(std::uint64_t address) const {
    if (chunks.empty()) return end();
    const auto index = chunkFor(address);
    const auto found = chunks[index]->find(address);
    if (found == chunks[index]->end()) return end();
    return const_iterator(chunks.cbegin() + static_cast<std::ptrdiff_t>(index), chunks.cend(), found);
}

inline ShaderRegistry::const_iterator ShaderRegistry::upper_bound(std::uint64_t address) const {
    if (chunks.empty()) return end();
    const auto index = chunkFor(address);
    const auto found = chunks[index]->upper_bound(address);
    if (found != chunks[index]->end()) return const_iterator(chunks.cbegin() + static_cast<std::ptrdiff_t>(index), chunks.cend(), found);
    if (index + 1u == chunks.size()) return end();
    return const_iterator(chunks.cbegin() + static_cast<std::ptrdiff_t>(index + 1u), chunks.cend(), chunks[index + 1u]->cbegin());
}

inline const ShaderRegistry::mapped_type& ShaderRegistry::at(std::uint64_t address) const {
    const auto found = find(address);
    if (found == end()) throw std::out_of_range("shader registry has no entry for the code address");
    return found->second;
}

inline ShaderRegistry::mapped_type& ShaderRegistry::operator[](std::uint64_t address) {
    if (chunks.empty()) chunks.push_back(std::make_shared<Chunk>());
    const auto index = chunks.front()->empty() ? 0u : chunkFor(address);
    auto& chunk = writable(index);
    const auto [entry, inserted] = chunk.try_emplace(address);
    auto& headers = entry->second;
    if (!inserted) return headers;
    ++count;
    if (chunk.size() > ChunkEntries) {
        auto upper = std::make_shared<Chunk>();
        for (auto moved = std::next(chunk.begin(), static_cast<std::ptrdiff_t>(chunk.size() / 2u)); moved != chunk.end();) upper->insert(upper->end(), chunk.extract(moved++));
        chunks.insert(chunks.begin() + static_cast<std::ptrdiff_t>(index + 1u), std::move(upper));
    }
    return headers;
}

inline std::pair<ShaderRegistry::const_iterator, bool> ShaderRegistry::emplace(std::uint64_t address, mapped_type headers) {
    if (const auto found = find(address); found != end()) return {found, false};
    (*this)[address] = std::move(headers);
    return {find(address), true};
}

inline std::size_t ShaderRegistry::erase(std::uint64_t address) {
    if (!contains(address)) return 0;
    const auto index = chunkFor(address);
    auto& chunk = writable(index);
    chunk.erase(address);
    --count;
    if (chunk.empty()) chunks.erase(chunks.begin() + static_cast<std::ptrdiff_t>(index));
    return 1;
}

struct PreparedShaderState {
    struct Entry {
        std::size_t codeOffset;
        std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
    };
    struct Rectangle {
        std::uint64_t vertexId;
        std::uint64_t fragmentId;
        std::shared_ptr<const ShaderRecompiler::RectListShaders> shaders;
    };
    struct RectangleProgress {
        std::weak_ptr<const ShaderSnapshot> fragment;
        std::size_t vertexCount;
        std::size_t fragmentCount;
    };
    struct GraphicsAbi {
        std::vector<std::uint64_t> key;
        std::weak_ptr<const ShaderRegistry> registry;
        std::vector<std::weak_ptr<const ShaderSnapshot>> stages;
    };
    std::vector<Entry> entries;
    std::vector<std::vector<std::uint64_t>> registeredAbis;
    std::vector<GraphicsAbi> graphicsAbis;
    std::vector<Rectangle> rectangles;
    std::vector<RectangleProgress> rectangleProgress;
    std::vector<std::weak_ptr<const ShaderSnapshot>> fragments;
    bool rectangleRequested = false;
    bool deferred = false;
};
struct PreparedShaders : PreparedShaderState {
    std::mutex mutex;
    std::condition_variable settled;
    std::exception_ptr failure;
    std::atomic<bool> demanded{false};
    bool pending = false;
};

struct RegisteredShaderState {
    Registers shader;
    Registers context;
    Registers userConfig;
};

struct ShaderSnapshot {
    std::uint64_t codeAddress;
    std::uint64_t headerAddress;
    std::uint8_t type;
    std::vector<std::uint32_t> code;
    std::vector<std::byte> header;
    std::shared_ptr<PreparedShaders> prepared = std::make_shared<PreparedShaders>();
    std::shared_ptr<const RegisteredShaderState> registeredState;
    std::shared_ptr<std::atomic<std::uint8_t>> workgroupUse = std::make_shared<std::atomic<std::uint8_t>>(0u);
    std::shared_ptr<std::atomic<bool>> rawGraphicsNoted = std::make_shared<std::atomic<bool>>(false);
};


std::shared_ptr<const ShaderSnapshot> ReadRawShader(std::uint64_t address);
std::shared_ptr<const ShaderSnapshot> ProgramSnapshot(const ShaderRegistry& shaders, std::uint64_t address, std::initializer_list<std::uint8_t> types);
void NoteRawGraphicsDraw(const ShaderSnapshot& snapshot);
bool RawGraphicsDrawn();
bool RawGraphicsDrawnIn(std::uint64_t begin, std::uint64_t end);
bool ProgramUsesWorkgroup(const ShaderSnapshot& snapshot, std::size_t codeOffset);

std::shared_ptr<const ShaderRecompiler::SourceHandle> PrepareShaderWithDiagnostics(const ShaderRecompiler::RecompileRequest& request, ShaderRecompiler::ShaderPreparationContext* preparation = nullptr);

std::uint64_t NullPixelProgramAddress();
ShaderSnapshot PrepareNullPixelProgram(const VulkanDevice& device);
std::optional<ShaderRecompiler::ShaderFloatMode> RegisteredFloatMode(const ShaderSnapshot& snapshot);
std::optional<ShaderRecompiler::ShaderFloatMode> DispatchFloatMode(const ShaderSnapshot& snapshot, const Registers& live);
std::shared_ptr<const ShaderSnapshot> RegisteredHeader(const ShaderRegistry& registry, const Shader* shader);
// Inline so the Metal driver, which does not link ShaderRegistry.cpp, resolves programs identically.
inline std::shared_ptr<const ShaderSnapshot> RegisteredProgram(const ShaderRegistry& registry, std::uint64_t address, std::initializer_list<std::uint8_t> types) {
    auto found = registry.upper_bound(address);
    if (found == registry.begin()) return {};
    --found;
    std::shared_ptr<const ShaderSnapshot> result;
    bool registered = false;
    for (const auto& snapshot : found->second) {
        if (address - snapshot->codeAddress >= snapshot->code.size() * sizeof(std::uint32_t)) continue;
        registered = true;
        if (std::find(types.begin(), types.end(), snapshot->type) == types.end()) continue;
        if (result != nullptr && result->type != snapshot->type) throw std::runtime_error("AGC driver: registered program has ambiguous shader binary types");
        result = snapshot;
    }
    if (registered && result == nullptr) throw std::runtime_error("AGC driver: registered program refers to an incompatible shader binary type");
    return result;
}
void PublishRegisteredShader(std::shared_ptr<ShaderRegistry>& registry, const std::shared_ptr<const ShaderSnapshot>& snapshot);
void SubmitRegistrationPreparation(std::function<void()> task, const PreparedShaders* prepared = nullptr);
void PrioritizeRegistrationPreparation(PreparedShaders& prepared);
void AwaitRegistrationPreparation();
void SubmitLibraryPrebuild(std::function<void()> task);
void AwaitLibraryPrebuilds();
void CloseRegistrationPreparation();
unsigned RegistrationPreparationThreads();

void ResolvePreparedGraphics(const ShaderSnapshot& front, const std::shared_ptr<const ShaderSnapshot>& fragment, std::uint32_t primitiveType, const ShaderRecompiler::SpirvTarget& target);

ShaderRecompiler::RectListShaders PreparedRectangle(const ShaderSnapshot& snapshot, std::uint64_t vertexId, std::uint64_t fragmentId);
ShaderRecompiler::RectListShaders DrawRectangle(const ShaderSnapshot& front, const std::shared_ptr<const ShaderSnapshot>& fragment, std::uint64_t vertexId, std::uint64_t fragmentId, const ShaderRecompiler::SpirvTarget& target);

std::shared_ptr<const ShaderRecompiler::SourceHandle> SourceHandleFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request);
ShaderRecompiler::PreparedShaderInvocation InvocationFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request);

}

#endif
