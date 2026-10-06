#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "CacheKey.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>
#include <cstring>
#include <list>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

std::shared_ptr<const ShaderSnapshot> ReadRawComputeShader(std::uint64_t address) {
    GuestMemory::CheckRange(reinterpret_cast<const void*>(address), sizeof(std::uint32_t), 256);
    static std::mutex cacheMutex;
    static std::list<std::shared_ptr<const ShaderSnapshot>> cache;
    static std::size_t cacheBytes = 0;
    std::shared_ptr<const ShaderSnapshot> cached;
    {
        std::lock_guard lock(cacheMutex);
        const auto found = std::find_if(cache.begin(), cache.end(), [address](const auto& entry) { return entry->codeAddress == address; });
        if (found != cache.end()) cached = *found;
    }
    if (cached) {
        const auto code = std::as_bytes(std::span(cached->code));
        GuestMemory::FlushGpuWrites(address, code.size());
        if (GuestMemory::CompareMapped(address, code) == GuestMemory::Compare::Equal) {
            std::lock_guard lock(cacheMutex);
            const auto found = std::find(cache.begin(), cache.end(), cached);
            if (found != cache.end()) cache.splice(cache.begin(), cache, found);
            return cached;
        }
    }
    constexpr std::size_t limit = 1024 * 1024;
    const auto ranges = GuestMemory::CommittedRanges(address, limit);
    std::uint64_t end = address;
    for (const auto& range : ranges) {
        if (range.first != end) break;
        end = range.second;
    }
    auto result = CaptureRawComputeShader(address, static_cast<std::size_t>(end - address));
    std::lock_guard lock(cacheMutex);
    const auto found = std::find_if(cache.begin(), cache.end(), [address](const auto& entry) { return entry->codeAddress == address; });
    if (found != cache.end()) {
        if ((*found)->code == result->code) {
            result = *found;
            cache.splice(cache.begin(), cache, found);
            return result;
        }
        cacheBytes -= (*found)->code.size() * sizeof(std::uint32_t);
        cache.erase(found);
    }
    const auto bytes = result->code.size() * sizeof(std::uint32_t);
    while (!cache.empty() && (cache.size() >= 64 || cacheBytes + bytes > 8 * 1024 * 1024)) {
        cacheBytes -= cache.back()->code.size() * sizeof(std::uint32_t);
        cache.pop_back();
    }
    cache.push_front(result);
    cacheBytes += bytes;
    return result;
}

bool FailureMemo() {
    static const bool memo = std::getenv("APS5_NO_FAILURE_MEMO") == nullptr;
    return memo;
}

std::shared_ptr<const ShaderRecompiler::SourceHandle> SourceHandleFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, std::uint64_t deviceSerial, const ShaderRecompiler::RecompileRequest& request, bool bypass, const std::string** poisoned) {
    static const bool enabled = std::getenv("APS5_NO_SOURCE_HANDLE_CACHE") == nullptr && std::getenv("APS5_NO_CAPTURE_REUSE") == nullptr;
    static const bool verify = std::getenv("APS5_VERIFY_SOURCE_HANDLE") != nullptr;
    if (!enabled || bypass || ShaderRecompiler::DebugProbeActive() || !request.useCache) return nullptr;
    std::uint64_t key = 0xcbf29ce484222325ull;
    for (const auto value : {static_cast<std::uint64_t>(codeOffset), deviceSerial, ShaderRecompiler::RecompileCacheKey::ContextHash(request)}) {
        key ^= value;
        key *= 0x100000001b3ull;
    }
    if (request.graphics.has_value()) {
        for (const auto& linked : request.graphics->linkedPrograms) {
            for (const auto value : {static_cast<std::uint64_t>(linked.role), linked.binary.codeAddress}) {
                key ^= value;
                key *= 0x100000001b3ull;
            }
        }
    }
    auto& memos = *snapshot.handles;
    {
        std::lock_guard lock(memos.mutex);
        for (const auto& entry : memos.entries) {
            if (entry.key != key || (entry.handle == nullptr && entry.failure == nullptr)) continue;
            if (entry.handle == nullptr) {
                if (verify) {
                    bool resolved = true;
                    try {
                        static_cast<void>(ShaderRecompiler::ResolveSource(request));
                    } catch (const std::exception&) {
                        resolved = false;
                    }
                    if (resolved) throw std::runtime_error("AGC driver: source handle memo poisoned but the source resolved");
                }
                ShaderMemory::CountHandleMemo(true);
                if (poisoned == nullptr) throw std::runtime_error(*entry.failure);
                *poisoned = entry.failure.get();
                return nullptr;
            }
            if (verify && ShaderRecompiler::ResolveSource(request)->source != entry.handle->source) throw std::runtime_error("AGC driver: source handle memo answered a different source");
            ShaderMemory::CountHandleMemo(true);
            return entry.handle;
        }
    }
    std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
    try {
        handle = ShaderRecompiler::ResolveSource(request);
    } catch (const std::exception& error) {
        if (FailureMemo()) {
            std::lock_guard lock(memos.mutex);
            memos.entries[memos.next] = {key, nullptr, std::make_shared<const std::string>(error.what())};
            memos.next = (memos.next + 1) % memos.entries.size();
            memos.poisoned.fetch_add(1, std::memory_order_relaxed);
        }
        throw;
    }
    ShaderMemory::CountHandleMemo(false);
    if (handle == nullptr) return nullptr;
    std::lock_guard lock(memos.mutex);
    memos.entries[memos.next] = {key, handle, nullptr};
    memos.next = (memos.next + 1) % memos.entries.size();
    return handle;
}

void Driver::RegisterShader(const Shader* shader) {
    CheckFailure();
    auto snapshot = ReadRegisteredShader(reinterpret_cast<std::uintptr_t>(shader));

    static const char* traceRegs = std::getenv("APS5_TRACE_SHADER_REGS");
    if (traceRegs != nullptr && (std::string(traceRegs) == "all" || std::strtoull(traceRegs, nullptr, 16) == snapshot->codeAddress)) {
        Shader header{};
        std::memcpy(&header, snapshot->header.data(), sizeof(header));
        const auto trace = [](const ShaderRegister* registers, std::uint32_t count) {
            for (std::uint32_t i = 0; i < count && registers != nullptr; ++i) {
                ShaderRegister value{};
                GuestMemory::Read(reinterpret_cast<std::uintptr_t>(registers) + std::uint64_t{i} * sizeof(value),
                    std::as_writable_bytes(std::span(&value, 1)), alignof(ShaderRegister));
                std::fprintf(stderr, " %x=%08x", value.offset, value.value);
            }
        };
        std::fprintf(stderr, "[shader] 0x%llx type %u cx", static_cast<unsigned long long>(snapshot->codeAddress), header.type);
        trace(header.cx_registers, header.num_cx_registers);
        std::fprintf(stderr, " sh");
        trace(header.sh_registers, header.num_sh_registers);
        std::fprintf(stderr, "\n");
    }
    std::lock_guard lock(mutex);
    rethrowFailure();
    const auto address = snapshot->codeAddress;

    if (shaders == nullptr) shaders = std::make_shared<ShaderRegistry>();
    else if (shaders.use_count() != 1) shaders = std::make_shared<ShaderRegistry>(*shaders);
    shaders->insert_or_assign(address, std::move(snapshot));
    if (shaders->find(NullPixelProgramAddress()) == shaders->end()) {
        shaders->insert_or_assign(NullPixelProgramAddress(), CaptureNullPixelShader());
    }
}

}
