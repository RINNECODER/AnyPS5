#include "MetalDriverInternal.hpp"
#include "PacketAutoreleasePool.hpp"
#include "prx/libSceAgcDriver/Execution/include/ComputeDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/ConstantFillKernel.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/IndirectDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/PacketCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MeshDraw.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/BindTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/GpuStallWatch.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/GpuTiming.hpp"
#include "PacketStoreRange.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <spirv/unified1/spirv.hpp>
#include <deque>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace AgcDriver::Metal {
namespace {
using namespace ShaderRecompiler;

void configureSamplerArguments(MetalBackend::TargetOptions& options, const RecompileResult& guest, id<MTLDevice> device) {
    options.supportsArgumentBuffersTier2 = device.argumentBuffersSupport == MTLArgumentBuffersTier2;
    options.maxArgumentBufferSamplers = 32;
    options.samplerArgumentBuffer = false;
    std::uint32_t count = 0;
    for (const auto& binding : guest.bindings) {
        if (binding.kind != DescriptorKind::Sampler) continue;
        if (binding.count > options.maxSamplers - count) {
            options.samplerArgumentBuffer = true;
            break;
        }
        count += binding.count;
    }
}

SpirvTarget nativeTarget(id<MTLDevice> device, std::optional<MeshTargetLimits> mesh = {}) {
    // FragmentBarycentricKHR (last) means [[barycentric_coord]] weights; per-vertex inputs stay off (false below).
    static constexpr std::array<std::uint32_t, 8> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityMinLod, spv::CapabilitySampledImageArrayDynamicIndexing,
        spv::CapabilityShaderNonUniform, spv::CapabilitySampledImageArrayNonUniformIndexing, spv::CapabilityFragmentBarycentricKHR};
    const auto supported = [&](std::span<const std::uint32_t> all) {
        return device.supportsShaderBarycentricCoordinates ? all : all.first(all.size() - 1u);
    };
    static constexpr std::array<std::string_view, 3> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage", "SPV_EXT_descriptor_indexing"};
    const auto maximum = device.maxThreadsPerThreadgroup;
    SpirvTarget target{0x00401000, 0x00010300, 32, BdaAbi::Version, supported(capabilities), extensions, false,
        {static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height), static_cast<std::uint32_t>(maximum.depth)},
        static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
    if (mesh) {
        static constexpr std::array<std::uint32_t, 9> meshCapabilities{
            spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
            spv::CapabilityMeshShadingEXT, spv::CapabilityMinLod, spv::CapabilitySampledImageArrayDynamicIndexing,
            spv::CapabilityShaderNonUniform, spv::CapabilitySampledImageArrayNonUniformIndexing, spv::CapabilityFragmentBarycentricKHR};
        static constexpr std::array<std::string_view, 4> meshExtensions{
            "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage", "SPV_EXT_mesh_shader", "SPV_EXT_descriptor_indexing"};
        target.spirvVersion = 0x00010400;
        target.supportedCapabilities = supported(meshCapabilities);
        target.supportedExtensions = meshExtensions;
        target.mesh = mesh;
    }
    return target;
}

// The work already ran on the GPU when it faulted, so this is never a skippable packet.
void checkGpuFault(const BdaAbi::Fault& fault) {
    if (fault.state == BdaAbi::FaultState::Empty) return;
    std::ostringstream message;
    message << "Native Metal guest GPU access failed: reason=" << static_cast<std::uint32_t>(fault.reason)
        << " address=0x" << std::hex << fault.address << " instruction=0x" << fault.instruction
        << std::dec << " bytes=" << fault.bytes << " stage=" << fault.stage;
    throw MetalGpuExecutionError(message.str());
}

// Alias working set (MetalGuestHostAliases::EnableWorkingSet): work that needs guest bytes outside
// the set runs again once they are admitted. A binding miss is found before anything reaches the
// GPU. A shader access outside the set read zero or dropped its store and reported an Unmapped
// fault; the second run repeats the work's other writes, which only differs from one run for
// blending or atomics, and only on the first touch of a granule.
constexpr unsigned WorkingSetAttempts = 64;

template <typename Run>
BdaAbi::Fault runInWorkingSet(MetalGuestHostAliases& aliases, std::span<const NativeGuestMemory::BorrowedRange> ranges, Run&& run) {
    for (unsigned attempt = 1;; ++attempt) {
        BdaAbi::Fault fault{};
        try {
            fault = run();
        } catch (const MetalWorkingSetMiss&) {
            if (attempt < WorkingSetAttempts) continue;
            throw;
        }
        if (attempt < WorkingSetAttempts && fault.state != BdaAbi::FaultState::Empty && fault.reason == BdaAbi::FaultReason::Unmapped &&
            aliases.AdmitGuest(ranges, fault.address, std::max<std::uint32_t>(fault.bytes, 1u))) {
            continue;
        }
        return fault;
    }
}

constexpr std::string_view SkippedDraw = "draw", SkippedDispatch = "dispatch";

// Packets Pm4::Execute handles without reading or storing guest memory (state and register updates).
bool RegisterOnlyPacket(std::uint32_t opcode) {
    switch (opcode) {
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x20: case 0x26: case 0x28: case 0x2a: case 0x2f:
        case 0x59: case 0x69: case 0x76: case 0x79: case 0x7a: case 0x81:
            return true;
        default:
            return false;
    }
}
constexpr std::size_t MaxSkipReasonBytes = 240;

// Log the first skip of each reason, then every power of ten, so a draw that fails
// every frame stays visible without flooding the log.
bool reportSkipCount(std::uint64_t count) {
    while (count >= 10 && count % 10 == 0) count /= 10;
    return count == 1;
}

// The driver itself is unusable (not configured): failing every packet would only
// hide that, so it stays sticky.
class DriverInvariantError final : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

}

// Fail-soft packet execution (#289): an unsupported or failed draw or dispatch
// (decode, shader translation, pipeline or resource setup) is skipped, logged and
// counted, and the command buffer continues. Skipping is safe because guest memory
// only changes when a packet's results are copied back after its last command
// buffer completes; work that ran on the GPU before a later failure in the same
// packet stays in driver-owned buffers and is dropped with the packet.
// A sticky failure is kept for what puts the GPU, guest memory or the driver in
// doubt: command-buffer and encoder errors, faults and copy-back failures of work
// that already ran (MetalGpuExecutionError), guest writes into read-only memory,
// allocation failure, an unconfigured driver, a failure reported elsewhere, and
// shutdown.
template <typename Work>
void MetalDriver::Impl::Tolerate(std::string_view kind, Work&& work) {
    try {
        try {
            DrainPacketAutoreleases(std::forward<Work>(work));
        } catch (...) {
            // Work that failed part-way may have written guest memory the cache does not know about.
            ForgetCachedImages(ForgetReason::Failure);
            throw;
        }
    } catch (const MetalGpuExecutionError&) {
        throw;
    } catch (const NativeGuestMemory::ReadOnlyWriteError&) {
        throw;
    } catch (const DriverInvariantError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        // A sticky failure or a stop that arrived meanwhile outranks this packet's own error.
        CheckFailureAndStopping();
        RecordSkip(kind, error.what());
    }
}

void MetalDriver::Impl::RecordSkip(std::string_view kind, std::string_view what) {
    std::string reason(what.substr(0, what.find('\n')));
    if (reason.size() > MaxSkipReasonBytes) reason.resize(MaxSkipReasonBytes);
    if (BindTrace::Active()) BindTrace::Log("skipped %.*s: %s", static_cast<int>(kind.size()), kind.data(), reason.c_str());
    std::uint64_t count = 0;
    bool unlisted = false;
    {
        std::lock_guard lock(skipMutex);
        ++(kind == SkippedDraw ? skipped.draws : skipped.dispatches);
        const auto found = std::find_if(skipped.reasons.begin(), skipped.reasons.end(), [&](const auto& entry) {
            return entry.kind == kind && entry.what == reason;
        });
        if (found != skipped.reasons.end()) {
            count = ++found->count;
        } else if (skipped.reasons.size() < SkippedWorkDiagnostics::MaxReasons) {
            skipped.reasons.push_back({std::string(kind), reason, 1});
            count = 1;
        } else {
            count = ++skipped.unlistedReasons;
            unlisted = true;
        }
    }
    if (!reportSkipCount(count)) return;
    if (unlisted) {
        // Reasons past the list are not deduplicated, so sample them by their running total.
        std::fprintf(stderr, "[metal] skipped %.*s (unlisted #%llu, reason list full at %zu): %s\n",
            static_cast<int>(kind.size()), kind.data(), static_cast<unsigned long long>(count),
            SkippedWorkDiagnostics::MaxReasons, reason.c_str());
    } else {
        std::fprintf(stderr, "[metal] skipped %.*s (x%llu): %s\n", static_cast<int>(kind.size()), kind.data(),
            static_cast<unsigned long long>(count), reason.c_str());
    }
}

SkippedWorkDiagnostics MetalDriver::SkippedWork() const {
    std::lock_guard lock(impl->skipMutex);
    return impl->skipped;
}

namespace {

// The PA_CL_VS_OUT_CNTL position export bits a vertex program is compiled with (upstream's
// PositionExportLayout, without the snapshot cache): none for a program without pos1..pos3
// exports, so its artifact does not depend on the register. Only draws that enable clip or cull
// distances, the misc vector or the distance vectors pay for the scan.
std::uint32_t PositionExportLayout(std::span<const std::uint32_t> code, std::uint32_t paClVsOutCntl) {
    const auto bits = paClVsOutCntl & Graphics::PositionExportLayoutBits;
    if (bits == 0) return 0;
    const auto decoded = RdnaInstructionDecoder{}.Decode(code);
    const bool used = std::any_of(decoded.instructions.begin(), decoded.instructions.end(), [](const auto& instruction) {
        return instruction.family == RdnaInstructionFamily::EXP && instruction.exportTarget >= 0x0du && instruction.exportTarget <= 0x0fu && instruction.exportEnableMask != 0;
    });
    return used ? bits : 0;
}

// ANYPS5_METAL_STALL_DUMP: the user data and the first words of each bound buffer, appended to the
// stuck dispatch's source as a comment.
std::string DescribeDispatch(std::span<const std::uint32_t> userData, std::span<const MetalShaderResourceBinding> descriptors) {
    std::string text = "\n/* user data:";
    char word[48];
    for (const auto value : userData) {
        std::snprintf(word, sizeof word, " %08x", value);
        text += word;
    }
    for (const auto& descriptor : descriptors) {
        for (std::size_t i = 0; i < descriptor.buffers.size(); ++i) {
            const auto& binding = descriptor.buffers[i];
            if (binding.buffer == nil || binding.buffer.storageMode != MTLStorageModeShared) continue;
            const auto available = binding.buffer.length > binding.offset ? binding.buffer.length - binding.offset : 0;
            std::snprintf(word, sizeof word, "\n set %u binding %u[%zu] %llu bytes:", descriptor.descriptorSet, descriptor.binding, i,
                static_cast<unsigned long long>(binding.length.value_or(available)));
            text += word;
            const auto* words = reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(binding.buffer.contents) + binding.offset);
            for (std::size_t at = 0; at < std::min<std::size_t>(64, available / 4); ++at) {
                std::snprintf(word, sizeof word, " %08x", words[at]);
                text += word;
            }
        }
    }
    return text + "\n*/\n";
}

// ANYPS5_METAL_STALL_DUMP: the buffer ranges (host addresses) the last dispatches bound, oldest first,
// so a stuck dispatch's inputs can be traced to the work that produced them.
std::string RememberDispatch(std::uint64_t program, std::span<const std::uint32_t, 3> groups, std::span<const MetalShaderResourceBinding> descriptors) {
    static std::deque<std::string> history;
    char text[96];
    std::snprintf(text, sizeof text, "\n cs 0x%llx groups %ux%ux%u:", static_cast<unsigned long long>(program), groups[0], groups[1], groups[2]);
    std::string line = text;
    for (const auto& descriptor : descriptors) {
        for (const auto& binding : descriptor.buffers) {
            if (binding.buffer == nil || binding.buffer.storageMode != MTLStorageModeShared) continue;
            const auto available = binding.buffer.length > binding.offset ? binding.buffer.length - binding.offset : 0;
            std::snprintf(text, sizeof text, " [0x%llx +0x%llx]",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(binding.buffer.contents) + binding.offset),
                static_cast<unsigned long long>(binding.length.value_or(available)));
            line += text;
        }
    }
    history.push_back(std::move(line));
    if (history.size() > 64) history.pop_front();
    std::string all = "\n/* recent dispatches (buffer host ranges), oldest first:";
    for (const auto& entry : history) all += entry;
    return all + "\n*/\n";
}

}

// Whether a DISPATCH_INDIRECT runs with the GPU reading its group counts (MetalComputePipeline::
// EncodeIndirect): the counts are whole threadgroups (DISPATCH_INITIATOR.USE_THREAD_DIMENSIONS
// clear) at a 4-byte aligned address. ANYPS5_METAL_GPU_INDIRECT=0 reads them on the CPU after a drain.
bool MetalDriver::Impl::GpuIndirectDispatch(std::uint32_t initiator, std::uint64_t arguments) {
    return GpuIndirect() && (initiator & 0x20u) == 0 && arguments % 4 == 0;
}

// The draws whose indirect records the GPU may read (MetalDraw::Draw, GpuReadsIndirectRecords decide
// the rest once the shader is known): the plain vertex path, with the record count in the packet.
bool MetalDriver::Impl::GpuIndirectDrawPath(const Graphics::State& state, const Pm4::DrawParameters::IndirectDraw& indirect) {
    return state.stages.path == Graphics::ShaderPath::Vertex && !state.stages.mesh && !state.stages.tessellation && !state.rectList &&
        !indirect.countIndirect && indirect.count <= MetalDraw::MaxGpuIndirectRecords && !(indirect.drawIndexEnabled && indirect.drawIndexSgpr >= 0);
}

// Metal's form of Driver::ClassifyIndirectDraw: per dimension (base vertex, first instance), a CP
// write of the record's dword into an SGPR must be one the recompiler folded into the draw's offset
// (InPlace: the Metal indirect draw takes it from the record); with no such write (Constant) the
// draw's offset is the folded user data word (plus GE_INDX_OFFSET for auto draws), which MetalDraw
// puts in place of the record's dword. Any other write takes the CPU path.
bool MetalDriver::Impl::GpuReadsIndirectRecords(const RecompileResult& result, std::uint32_t firstUserSgpr,
    std::span<const std::uint32_t> userData, Pm4::DrawParameters& parameters) {
    using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
    auto& indirect = *parameters.indirect;
    const auto dimension = [&](std::int32_t folded, std::int32_t written, bool shared, bool conflict, std::uint32_t offset, Rule& rule,
                               std::uint32_t& constant) {
        if (written < 0) {
            rule = Rule::Constant;
            std::uint32_t word = 0;
            if (folded >= 0) {
                const auto index = static_cast<std::uint32_t>(folded) - firstUserSgpr;
                if (folded < static_cast<std::int32_t>(firstUserSgpr) || index >= userData.size()) return false;
                word = userData[index];
            }
            if (word > std::numeric_limits<std::uint32_t>::max() - offset) return false;
            constant = offset + word;
            return true;
        }
        if (conflict || folded != written || shared || offset != 0) return false;
        rule = Rule::InPlace;
        return true;
    };
    const bool vertex = dimension(result.vertexOffsetSgpr, indirect.baseVertexSgpr, result.vertexOffsetShared, result.vertexOffsetConflict,
        parameters.indexed ? 0u : indirect.indxOffset, indirect.vertexRule, indirect.vertexConstant);
    const bool instance = dimension(result.instanceOffsetSgpr, indirect.startInstanceSgpr, result.instanceOffsetShared,
        result.instanceOffsetConflict, 0u, indirect.instanceRule, indirect.instanceConstant);
    return vertex && instance;
}

bool MetalDriver::Impl::GpuWaits() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_METAL_GPU_WAITS");
        return value == nullptr || *value != '0';
    }();
    return enabled && DeferWork();
}

bool MetalDriver::Impl::GpuIndirect() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_METAL_GPU_INDIRECT");
        return value == nullptr || *value != '0';
    }();
    return enabled && DeferWork();
}

void MetalDriver::Impl::ExecuteDispatchSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission,
    std::optional<std::uint64_t> indirectArguments) {
    GpuTiming::Scope timing(GpuTiming::Kind::Dispatch);
    if (backend == nullptr || submission.shaders == nullptr) throw DriverInvariantError("Native Metal compute executor is not configured");
    // Decoding reads guest memory (descriptors, user data) that committed BDA stores may change.
    if (timeline.UnknownWritesPending()) { ++drainsBy[0x101]; timeline.Drain(0x101); }
    // Find the registered shader first so the dispatch decodes against its AGC header, as the Vulkan
    // Driver::dispatch does (SCRATCH_EN takes its per-lane size from the header).
    const auto programAddress = DecodeComputeProgramAddress(queue.shader);
    std::shared_ptr<const DriverDetail::ShaderSnapshot> captured;
    captured = DriverDetail::RegisteredProgram(*submission.shaders, programAddress, {0});
    const auto decoded = DecodeComputeDispatch(queue, packet,
        captured != nullptr ? std::span<const std::byte>(captured->header) : std::span<const std::byte>{});
    if (std::any_of(decoded.groups.begin(), decoded.groups.end(), [](auto value) { return value == 0; })) return;
    GpuTiming::TagScope tag(GpuTiming::MakeTag(GpuTiming::Kind::Dispatch, decoded.programAddress));
    GpuStallWatch::Note(1, decoded.programAddress, std::uint64_t{decoded.groups[0] & 0x1fffffu} |
        std::uint64_t{decoded.groups[1] & 0x1fffffu} << 21 | std::uint64_t{decoded.groups[2] & 0x1fffffu} << 42);
    if (BindTrace::Active()) {
        BindTrace::Log("dispatch cs 0x%llx groups %ux%ux%u", static_cast<unsigned long long>(decoded.programAddress),
            decoded.groups[0], decoded.groups[1], decoded.groups[2]);
    }
    if (decoded.compute.scratchDwords != 0) {
        throw std::runtime_error("Native Metal compute scratch memory (COMPUTE_PGM_RSRC2.SCRATCH_EN) is not implemented");
    }
    if (captured == nullptr) {
        CompletePriorGpuWorkAndCopyBack();
        const auto available = NativeGuestMemory::ReadableBorrowedBytes(decoded.programAddress, 1024 * 1024);
        captured = DriverDetail::CaptureRawComputeShader(decoded.programAddress, available);
    }
    const auto& snapshot = *captured;
    if (snapshot.type != 0 || (decoded.programAddress - snapshot.codeAddress) % sizeof(std::uint32_t) != 0) {
        throw std::invalid_argument("Native Metal compute program refers to incompatible shader code");
    }
    const auto offset = static_cast<std::size_t>((decoded.programAddress - snapshot.codeAddress) / sizeof(std::uint32_t));
    std::vector<MemoryRegion> memory{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}};
    if (!snapshot.header.empty()) memory.push_back({snapshot.headerAddress, snapshot.header});
    ShaderMemory capture(memory);
    RecompileRequest request{{ShaderStage::Compute, decoded.programAddress, std::span(snapshot.code).subspan(offset),
        snapshot.headerAddress, snapshot.header},
        {decoded.waveSize, 0, decoded.userData, decoded.compute, {}, {}, memory,
            DriverDetail::HeaderFloatMode(snapshot.type, snapshot.headerAddress, snapshot.header)},
        nativeTarget(nativeDevice), {0, 0, 0, 128}, {}, true};
    const auto resources = capture.Capture(request);
    memory = capture.Regions();
    request.context.memory = memory;
    const auto guest = Recompile(request, *resources);
    // The recompiler memoizes its results: the same result object means the same conversion and
    // pipeline, so a repeated dispatch skips building and hashing the conversion key (the whole
    // SPIR-V) and the Metal source (the function cache key).
    auto [pipeline, bdaStores] = [&]() -> std::pair<std::shared_ptr<const MetalComputePipeline>, bool> {
        std::lock_guard lock(computePipelinesMutex);
        const auto found = computePipelines.find(guest.get());
        if (found != computePipelines.end() && !found->second.guest.expired()) return {found->second.pipeline, found->second.bdaStores};
        MetalBackend::TargetOptions options;
        if (@available(macOS 15.0, *))
            options.supportsWorkgroupAtomicFences = [nativeDevice supportsFamily:MTLGPUFamilyApple1];
        options.supportsInt64 = true;
        options.supportsGpuAddresses = true;
        options.supportsSimdGroups = true;
        configureSamplerArguments(options, *guest, nativeDevice);
        const auto converted = MetalBackend::ConvertToMetal(*guest, ShaderStage::Compute, options);
        GpuStallWatch::SourceScope stallSource(converted.source);
        auto created = std::make_shared<const MetalComputePipeline>(nativeDevice, converted);
        const bool stores = StoresThroughBda(converted);
        if (computePipelines.size() >= 1024) {
            std::erase_if(computePipelines, [](const auto& entry) { return entry.second.guest.expired(); });
            if (computePipelines.size() >= 1024) computePipelines.clear();
        }
        computePipelines[guest.get()] = {guest, created, stores};
        return {created, stores};
    }();
    const auto& converted = pipeline->Reflection();
    GpuStallWatch::SourceScope stallSource(converted.source);
    // Depth/stencil and multisampled color targets live only in the draw executor's resident
    // textures, so a dispatch reading one (an MSAA resolve, a depth-based post pass) binds that
    // texture like a draw does; guest memory at those addresses never holds the rendered pixels.
    // A single-sample descriptor the resident surface cannot serve keeps its guest-memory binding.
    MetalShaderResources::DepthTextureLookup resident;
    if (draw) {
        resident = [this](const Graphics::GuestTextureResource& resource, bool storage, bool compare) -> id<MTLTexture> {
            try {
                return draw->ResidentTexture(resource, storage, compare);
            } catch (const std::invalid_argument&) {
                if (resource.samples > 1) throw;
                return nil;
            }
        };
    }
    std::array<NSUInteger, 3> grid{};
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        const auto threads = static_cast<std::uint64_t>(decoded.groups[axis]) * converted.threadsPerThreadgroup[axis];
        if (threads > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Native Metal compute grid exceeds shader integer range");
        grid[axis] = static_cast<NSUInteger>(threads);
    }
    const auto size = MTLSizeMake(grid[0], grid[1], grid[2]);
    // A constant fill (DCC keys being cleared, most often) tells the timeline what it stores, so a
    // later draw learns the keys without waiting for it (MetalTimeline::PendingBytes). Not for an
    // indirect dispatch: its real group counts are in the argument buffer, not in this packet.
    const auto fill = indirectArguments ? std::nullopt : MatchConstantFill(std::span(snapshot.code).subspan(offset), decoded.userData, decoded.compute.numThreads, decoded.groups,
        decoded.compute.partialThreads);
    // One run of the dispatch; with `deferred` it is committed without waiting when its results need
    // no CPU copy into guest memory, and `completed` gets its fault at the timeline's next drain.
    const auto encode = [this, pipeline, resident, size, indirectArguments, fill,
        stallUserData = GpuStallWatch::DumpPath() != nullptr ? decoded.userData : std::vector<std::uint32_t>{},
        stallProgram = decoded.programAddress, stallGroups = decoded.groups](bool deferred, MetalDraw::Completed completed, bool stores)
        -> std::optional<BdaAbi::Fault> {
        auto bindings = std::make_shared<MetalShaderResources>(*backend, ranges, resident, draw ? &draw->HostAliases() : nullptr,
            draw ? draw->ImageCache() : nullptr);
        bindings->SetCpuReadHook([this](std::uint64_t address, std::uint64_t bytes) { BeforeCpuRead(address, bytes); });
        if (draw) {
            bindings->SetLazyCopyBacks(&draw->LazyCopyBacks());
            bindings->SetMaterializeHook([this](std::uint64_t address, std::uint64_t bytes) {
                MaterializeCopyBacks(MetalLazyCopyBacks::Cause::GpuRead, address, bytes);
            });
        }
        if (deferred) bindings->SetPendingStores(MetalShaderResources::PendingStoresOf(timeline));
        const auto descriptors = bindings->Bindings(pipeline->Reflection());
        if (GpuStallWatch::DumpPath() != nullptr) {
            GpuStallWatch::SetDetail(DescribeDispatch(stallUserData, descriptors) +
                RememberDispatch(stallProgram, std::span<const std::uint32_t, 3>(stallGroups.data(), 3), descriptors));
        }
        // Bound before the command buffer is taken: a binding over a lazy copy-back commits it to
        // the timeline, which may commit the open batch.
        std::optional<MetalBufferBinding> arguments;
        if (indirectArguments) arguments = bindings->Buffer(*indirectArguments, 3 * sizeof(std::uint32_t));
        id<MTLCommandBuffer> commands = backend->CommandBuffer();
        MetalBatchedWork batched(*backend, commands);
        if (indirectArguments) {
            // The GPU reads the group counts when the dispatch runs, after the work that writes them.
            pipeline->EncodeIndirect(commands, descriptors, arguments->buffer, arguments->offset, {}, bindings->Residency());
        } else {
            pipeline->Encode(commands, descriptors, size, {}, bindings->Residency());
        }
        // Committed to the timeline without waiting: its copy-backs may wait for a reader.
        bindings->EncodeReadbacks(commands, deferred);
        batched.Done();
        if (deferred && !bindings->NeedsSynchronousCompletion()) {
            std::vector<MetalTimeline::KnownStore> known;
            if (fill && fill->bytes <= (std::uint64_t{1} << 20u)) {
                try {
                    // What the range holds before the fill (read while this work's borrowed ranges are in scope).
                    const auto before = NativeGuestMemory::ContiguousBorrowedRange(fill->base, static_cast<std::size_t>(fill->bytes), false);
                    MetalTimeline::KnownStore store{fill->base, fill->base + fill->bytes, {}, {before.begin(), before.end()}};
                    std::memcpy(store.pattern.data(), fill->pattern.data(), store.pattern.size());
                    known.push_back(std::move(store));
                } catch (const std::exception&) {
                    known.clear();
                }
            }
            bindings->CommittedWithoutWaiting();
            // Keys of DCC storage images this dispatch leaves uncompressed after earlier pending work.
            for (auto& store : bindings->KnownStores()) known.push_back(std::move(store));
            timeline.Commit(commands, [bindings, commands, completed = std::move(completed)] {
                const auto fault = CompleteCommittedWork([&] { return bindings->Complete(commands); });
                completed(fault);
            }, stores, bindings->WrittenGuestRanges(), std::move(known));
            return std::nullopt;
        }
        MetalEncoders::EndParked(commands);
        [commands commit];
        backend->Wait(commands);
        return CompleteCommittedWork([&] { return bindings->Complete(commands); });
    };
    const auto synchronous = [this, encode] {
        const auto once = [&] { return *encode(false, {}, false); };
        checkGpuFault(draw ? runInWorkingSet(draw->HostAliases(), ranges, once) : once());
    };
    if (!DeferWork() || draw == nullptr) return synchronous();
    // A deferred run that touched guest bytes outside the alias working set admits them and runs
    // again, synchronously, when the drain finds the fault (see runInWorkingSet).
    MetalDraw::Completed completed = [this, synchronous](BdaAbi::Fault fault) {
        if (fault.state == BdaAbi::FaultState::Empty) return;
        if (fault.reason == BdaAbi::FaultReason::Unmapped &&
            draw->HostAliases().AdmitGuest(ranges, fault.address, std::max<std::uint32_t>(fault.bytes, 1u))) {
            ++deferredReruns;
            // The rerun runs inside a drain, often from a later packet: a failure that would
            // have skipped this work skips it here instead of stopping the drain (which would
            // drop the label stores and retirements queued behind it).
            return Tolerate(SkippedDispatch, synchronous);
        }
        checkGpuFault(fault);
    };
    checkGpuFault(runInWorkingSet(draw->HostAliases(), ranges, [&] {
        return encode(true, completed, bdaStores).value_or(BdaAbi::Fault{});
    }));
}

void MetalDriver::Impl::ExecuteDrawSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
    GpuTiming::Scope timing(GpuTiming::Kind::Draw);
    if (draw == nullptr || submission.shaders == nullptr) throw DriverInvariantError("Native Metal draw executor is not configured");
    // Decoding reads guest memory (descriptors, user data) that committed BDA stores may change.
    if (timeline.UnknownWritesPending()) { ++drainsBy[0x100]; timeline.Drain(0x100); }
    auto parameters = Pm4::ResolveDraw(packet, queue);
    if (!parameters.indirect && (parameters.indexCount == 0 || parameters.instanceCount == 0)) {
        if (BindTrace::Active()) BindTrace::Log("empty draw: count %u instances %u", parameters.indexCount, parameters.instanceCount);
        return;
    }
    // A CB resolve draws nothing of its own: CB0's resident samples are resolved into CB1.
    if (const auto control = queue.context.find(0x202); control != queue.context.end() && ((control->second >> 4u) & 7u) == 3u) {
        if (const auto pass = Graphics::DecodeColorMetadataPass(queue); pass && pass->mode == Graphics::ColorMetadataPass::Mode::Resolve) {
            if (parameters.indirect) throw std::runtime_error("AGC graphics: indirect CB resolves are unsupported");
            if (BindTrace::Active()) {
                BindTrace::Log("CB resolve 0x%llx (%u samples) -> 0x%llx %ux%u", static_cast<unsigned long long>(pass->source->address),
                    pass->source->samples, static_cast<unsigned long long>(pass->targets.front().address),
                    pass->targets.front().extent.width, pass->targets.front().extent.height);
            }
            checkGpuFault(runInWorkingSet(draw->HostAliases(), ranges,
                [&] { return draw->ResolveSynchronously(*pass->source, pass->targets.front(), ranges); }));
            return;
        }
    }
    auto decoded = DecodeDrawDispatch(queue, *submission.shaders, DriverDetail::NullPixelProgramAddress());
    GpuTiming::TagScope tag(GpuTiming::MakeTag(GpuTiming::Kind::Draw, decoded.programs.empty() ? 0 : decoded.programs.back().binary.codeAddress));
    GpuStallWatch::Note(2, decoded.programs.empty() ? 0 : decoded.programs.front().binary.codeAddress, parameters.indexCount);
    if (BindTrace::Active()) {
        std::string programs;
        for (const auto& program : decoded.programs) {
            char text[40];
            std::snprintf(text, sizeof text, " 0x%llx", static_cast<unsigned long long>(program.binary.codeAddress));
            programs += text;
        }
        BindTrace::Log("draw programs%s count %u instances %u%s samples %u colors %zu depth %s", programs.c_str(),
            parameters.indexCount, parameters.instanceCount, decoded.state.rectList ? " rect-list" : "",
            decoded.state.rasterSamples, decoded.state.colors.size(), decoded.state.depth ? "yes" : "no");
        const auto& s = decoded.state;
        const auto& blend = s.blends.empty() ? s.blend : s.blends.front();
        BindTrace::Log("  state scissor %d,%d %ux%u viewport %.0f,%.0f %.0fx%.0f blend %u (%d,%d,%d / %d,%d,%d) mask 0x%x depth test %u write %u op %d"
            " stencil %u ref 0x%x cmp %d read 0x%x write 0x%x ops %d/%d/%d cull %u", s.scissor.offset.x, s.scissor.offset.y,
            s.scissor.extent.width, s.scissor.extent.height, s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height,
            blend.blendEnable, blend.srcColorBlendFactor, blend.dstColorBlendFactor, blend.colorBlendOp, blend.srcAlphaBlendFactor,
            blend.dstAlphaBlendFactor, blend.alphaBlendOp, blend.colorWriteMask, s.depthTest, s.depthWrite, s.depthCompare,
            s.stencilTest, s.stencilFront.reference, s.stencilFront.compareOp, s.stencilFront.compareMask, s.stencilFront.writeMask,
            s.stencilFront.failOp, s.stencilFront.depthFailOp, s.stencilFront.passOp, s.cullMode);
        for (const auto& color : decoded.state.colors) {
            BindTrace::Log("  color[%u] 0x%llx vk-format %d %ux%u mip %u samples %u dcc 0x%llx -> %s", color.exportIndex,
                static_cast<unsigned long long>(color.address), static_cast<int>(color.format), color.extent.width, color.extent.height,
                color.mip, color.samples, static_cast<unsigned long long>(color.dccAddress),
                color.samples > 1 ? "resident MSAA surface" : "guest-memory texture");
        }
        if (decoded.state.depth) {
            const auto& depth = *decoded.state.depth;
            BindTrace::Log("  depth 0x%llx stencil 0x%llx vk-format %d %ux%u samples %u", static_cast<unsigned long long>(depth.address),
                static_cast<unsigned long long>(depth.stencilAddress), static_cast<int>(depth.format), depth.extent.width,
                depth.extent.height, depth.samples);
        }
    }
    if ((decoded.state.stages.path != Graphics::ShaderPath::Vertex && decoded.state.stages.path != Graphics::ShaderPath::Geometry) ||
        (decoded.state.rectList && decoded.state.stages.path != Graphics::ShaderPath::Vertex)) {
        throw std::runtime_error("Native Metal draw execution requires the implemented vertex or mesh path");
    }
    const auto executeDirect = [&](Pm4::DrawParameters direct,
        std::optional<std::array<std::uint32_t, 5>> meshArguments = std::nullopt) {
        CheckFailureAndStopping();
        const auto mesh = decoded.state.stages.mesh.has_value();
        const auto rectangle = decoded.state.rectList;
        if (rectangle && direct.indexCount % 3 != 0) throw std::runtime_error("Native Metal draw contains an incomplete rect-list primitive");
        if (mesh) {
            auto& front = decoded.programs.front();
            if (front.firstUserSgpr != 0 || front.userData.size() < MeshIndexBufferUserWord + 4) {
                throw std::runtime_error("Native Metal mesh program lacks the hidden user words");
            }
            const auto descriptor = Graphics::MeshIndexBufferDescriptor(meshArguments ? parameters : direct, front.binary.codeAddress);
            std::copy(descriptor.begin(), descriptor.end(), front.userData.begin() + MeshIndexBufferUserWord);
        }
        auto target = nativeTarget(nativeDevice, mesh ? std::optional(draw->MeshLimits()) : std::nullopt);
        if (rectangle) {
            static constexpr std::array<std::uint32_t, 8> capabilities{
                spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses,
                spv::CapabilityStorageBuffer8BitAccess, spv::CapabilityTessellation, spv::CapabilityMinLod,
                spv::CapabilitySampledImageArrayDynamicIndexing,
                spv::CapabilityShaderNonUniform, spv::CapabilitySampledImageArrayNonUniformIndexing};
            target.supportedCapabilities = capabilities;
            target.tessellation = draw->TessellationLimits();
        }
        const auto pushLimit = mesh ? MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes;
        std::vector<MemoryRegion> memory;
        std::vector<LinkedProgram> linked;
        for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
            const auto& program = decoded.programs[i];
            memory.insert(memory.end(), program.memory.begin(), program.memory.end());
            linked.push_back({decoded.roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
        }
        ShaderMemory capture(memory);
        std::vector<RecompileResult> programs;
        programs.reserve(decoded.programs.size() + (rectangle ? 2u : 0u));
        std::vector<Graphics::CompiledShader> stages;
        stages.reserve(decoded.programs.size() + (rectangle ? 2u : 0u));
        std::uint32_t pushOffset = 0;
        for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
            if (decoded.roles[i] == ProgramRole::GeometryBack) continue;
            const auto& program = decoded.programs[i];
            std::optional<ShaderVertexStageInfo> vertex;
            if (program.binary.stage != ShaderStage::Fragment) {
                vertex = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData);
                if (vertex) {
                    vertex->paClVsOutCntl = PositionExportLayout(program.binary.code, decoded.state.paClVsOutCntl);
                    // MSL has clip distances but no cull distances.
                    if ((vertex->paClVsOutCntl & 0xff00u) != 0) {
                        throw std::runtime_error("AGC graphics: vertex cull distances are unsupported on Metal");
                    }
                }
            }
            auto request = BuildDrawRecompileRequest(program.binary, program.firstUserSgpr, program.userData,
                decoded.state, decoded.pixel, vertex, target, pushOffset, direct, memory, linked);
            // The Vulkan draw sets RegisteredFloatMode here (DrawCapture.cpp); Metal snapshots carry no
            // registered state, so the same RSRC1 is read from the captured header.
            request.context.floatMode = DriverDetail::HeaderFloatMode(program.snapshot->type,
                program.snapshot->headerAddress, program.snapshot->header);
            const auto resources = capture.Capture(request);
            memory = capture.Regions();
            request.context.memory = memory;
            programs.push_back(*Recompile(request, *resources));
            const auto& result = programs.back();
            if (result.pushConstants.size() > pushLimit - pushOffset) {
                throw std::runtime_error("Native Metal draw stage push constants exceed the pipeline block");
            }
            stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushOffset});
            pushOffset += static_cast<std::uint32_t>(result.pushConstants.size());
            if (i == 0 && direct.indirect) {
                if (!GpuReadsIndirectRecords(result, program.firstUserSgpr, program.userData, direct)) {
                    throw IndirectNeedsCpuRecords("Native Metal indirect draw offsets need the records on the CPU");
                }
            } else if (i == 0) {
                FoldDrawOffsets(result, program.firstUserSgpr, program.userData, direct);
            }
        }
        if (rectangle) {
            if (programs.size() != 2 || stages.size() != 2 || stages[0].stage != ShaderStage::Vertex ||
                stages[1].stage != ShaderStage::Fragment) {
                throw std::runtime_error("Native Metal rect-list requires vertex and fragment programs");
            }
            auto generated = BuildRectListShaders(programs[0], programs[1], target);
            programs.push_back(std::move(generated.control));
            programs.push_back(std::move(generated.evaluation));
            stages.insert(stages.begin() + 1, {{ShaderStage::TessellationControl, &programs[2], 0},
                {ShaderStage::TessellationEvaluation, &programs[3], 0}});
        }
        if (!DeferWork()) {
            checkGpuFault(runInWorkingSet(draw->HostAliases(), ranges,
                [&] { return draw->DrawSynchronously(decoded.state, direct, stages, ranges, meshArguments); }));
            return;
        }
        // The draw may be committed without waiting; what a synchronous rerun needs (after a
        // working-set fault found at the drain) stays alive with its completion.
        struct Job {
            Graphics::State state;
            Pm4::DrawParameters direct;
            std::vector<RecompileResult> programs;
            std::vector<Graphics::CompiledShader> stages;
            std::optional<std::array<std::uint32_t, 5>> meshArguments;
        };
        auto job = std::make_shared<Job>(Job{decoded.state, direct, std::move(programs), std::move(stages), meshArguments});
        const auto synchronous = [this, job] {
            checkGpuFault(runInWorkingSet(draw->HostAliases(), ranges,
                [&] { return draw->DrawSynchronously(job->state, job->direct, job->stages, ranges, job->meshArguments); }));
        };
        MetalDraw::Completed completed = [this, synchronous](BdaAbi::Fault fault) {
            if (fault.state == BdaAbi::FaultState::Empty) return;
            if (fault.reason == BdaAbi::FaultReason::Unmapped &&
                draw->HostAliases().AdmitGuest(ranges, fault.address, std::max<std::uint32_t>(fault.bytes, 1u))) {
                ++deferredReruns;
                // The rerun runs inside a drain, often from a later packet: a failure that would
                // have skipped this work skips it here instead of stopping the drain (which would
                // drop the label stores and retirements queued behind it).
                return Tolerate(SkippedDraw, synchronous);
            }
            checkGpuFault(fault);
        };
        checkGpuFault(runInWorkingSet(draw->HostAliases(), ranges, [&] {
            return draw->Draw(job->state, job->direct, job->stages, ranges, job->meshArguments, &timeline, completed)
                .value_or(BdaAbi::Fault{});
        }));
    };
    if (!parameters.indirect) {
        executeDirect(parameters);
        return;
    }
    std::vector<IndirectDrawProgram> programs;
    programs.reserve(decoded.programs.size());
    for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
        auto& program = decoded.programs[i];
        programs.push_back({decoded.roles[i], program.userDataBase, program.firstUserSgpr, program.userData});
    }
    MarkIndirectDrawSgprs(*parameters.indirect, programs);
    // The GPU reads the records when the draw runs, after the work that writes them (GPU culling
    // output, most often), so nothing waits here; a draw the GPU cannot take as it is reads them below.
    if (GpuIndirect() && GpuIndirectDrawPath(decoded.state, *parameters.indirect)) {
        try {
            executeDirect(parameters);
            ++gpuIndirectDraws;
            return;
        } catch (const IndirectNeedsCpuRecords&) {
            ++cpuIndirectDraws;
        }
    }
    CompletePriorGpuWorkAndCopyBack();
    const auto records = ReadIndirectDrawRecords(*parameters.indirect);
    for (std::uint32_t record = 0; record < records.size(); ++record) {
        CheckFailureAndStopping();
        // Earlier records may already have reached guest memory, so a failing record is
        // skipped on its own (and counted as one skipped draw) instead of the rest of the packet.
        Tolerate(SkippedDraw, [&] {
            const auto expanded = ExpandIndirectDrawRecord(parameters, records[record], record, programs);
            if (!expanded) return;
            std::optional<std::array<std::uint32_t, 5>> meshArguments;
            if (decoded.state.stages.mesh && expanded->parameters.indexed) {
                const auto& mesh = *decoded.state.stages.mesh;
                const auto inputSize = mesh.inputPrimitive == 1 ? 1u : mesh.inputPrimitive == 2 ? 2u : 3u;
                const auto step = mesh.inputPrimitive == 5 || mesh.inputPrimitive == 6 ? 1u : inputSize;
                if (expanded->parameters.indexCount < inputSize || mesh.primitivesPerGroup == 0) {
                    throw std::runtime_error("Native Metal mesh draw contains no complete primitive");
                }
                const auto groups = ((expanded->parameters.indexCount - inputSize) / step) / mesh.primitivesPerGroup + 1u;
                meshArguments = {groups, expanded->parameters.instanceCount, 1, expanded->parameters.indexCount,
                    records[record].firstVertexOrIndex};
            }
            executeDirect(expanded->parameters, meshArguments);
        });
    }
}

bool MetalDriver::Impl::TryExecuteDrawBatch(std::size_t& cursor, QueueState& queue, const Submission& submission) {
    if (!MetalDraw::ParallelEncodingEnabled()) return false;
    if (draw == nullptr || submission.shaders == nullptr) return false;
    // The batch is built from translation records, and it recompiles every draw it looks at to key
    // the lookup. Until the cache has served a record at its current version, that work is wasted:
    // records only appear as the serial path encodes, and an image-cache expire moves the version
    // under every record (MetalDrawTranslationCache::Live).
    if (!draw->TranslationCache().Live()) return false;

    std::vector<MetalDraw::PreparedDraw> batch;
    QueueState scanQueue = queue;
    std::size_t scanCursor = cursor;
    const auto totalWords = submission.commands.size();
    std::optional<MetalEncoders::PassKey> targetPassKey;

    while (scanCursor < totalWords) {
        const auto header = submission.commands[scanCursor];
        const auto count = Pm4::PacketWords(header);
        if (count == 0 || count > totalWords - scanCursor) break;
        const auto next = scanCursor + count;
        const auto opcode = (header >> 8u) & 0xffu;

        if (Pm4::FillerPacket(header) || Pm4::PureNopPacket(header)) {
            scanCursor = next;
            continue;
        }

        if (Pm4::DrawOpcode(opcode)) {
            if (Pm4::IndirectDrawOpcode(opcode)) break;
            const auto packet = std::span(submission.commands).subspan(scanCursor, count);
            try {
                auto direct = Pm4::ResolveDraw(packet, scanQueue);
                if (direct.indirect || direct.indexCount == 0 || direct.instanceCount == 0) break;
                auto decoded = DecodeDrawDispatch(scanQueue, *submission.shaders, DriverDetail::NullPixelProgramAddress());
                if (decoded.state.stages.path != Graphics::ShaderPath::Vertex ||
                    decoded.state.rectList || decoded.state.stages.mesh.has_value()) {
                    break;
                }

                auto target = nativeTarget(nativeDevice, std::nullopt);
                std::vector<MemoryRegion> memory;
                std::vector<LinkedProgram> linked;
                for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
                    const auto& program = decoded.programs[i];
                    memory.insert(memory.end(), program.memory.begin(), program.memory.end());
                    linked.push_back({decoded.roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
                }
                ShaderMemory capture(memory);
                std::vector<RecompileResult> programs;
                programs.reserve(decoded.programs.size());
                std::vector<Graphics::CompiledShader> stages;
                stages.reserve(decoded.programs.size());
                std::uint32_t pushOffset = 0;
                for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
                    if (decoded.roles[i] == ProgramRole::GeometryBack) continue;
                    const auto& program = decoded.programs[i];
                    std::optional<ShaderVertexStageInfo> vertex;
                    if (program.binary.stage != ShaderStage::Fragment) {
                        vertex = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData);
                        if (vertex) {
                            vertex->paClVsOutCntl = PositionExportLayout(program.binary.code, decoded.state.paClVsOutCntl);
                            if ((vertex->paClVsOutCntl & 0xff00u) != 0) break;
                        }
                    }
                    auto request = BuildDrawRecompileRequest(program.binary, program.firstUserSgpr, program.userData,
                        decoded.state, decoded.pixel, vertex, target, pushOffset, direct, memory, linked);
                    request.context.floatMode = DriverDetail::HeaderFloatMode(program.snapshot->type,
                        program.snapshot->headerAddress, program.snapshot->header);
                    const auto resources = capture.Capture(request);
                    memory = capture.Regions();
                    request.context.memory = memory;
                    programs.push_back(*Recompile(request, *resources));
                    const auto& result = programs.back();
                    stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushOffset});
                    pushOffset += static_cast<std::uint32_t>(result.pushConstants.size());
                    if (i == 0) FoldDrawOffsets(result, program.firstUserSgpr, program.userData, direct);
                }

                if (stages.size() < 2) break;

                auto prepared = draw->PrepareCachedDraw(decoded.state, direct, stages, ranges);
                if (!prepared) break;

                if (!targetPassKey) {
                    targetPassKey = prepared->cachedRecord->passKey;
                } else if (!MetalEncoders::Continues(*targetPassKey, prepared->cachedRecord->passKey)) {
                    break;
                }

                batch.push_back(std::move(*prepared));
                scanCursor = next;
            } catch (...) {
                break;
            }
        } else if (RegisterOnlyPacket(opcode) || opcode == 0x28 || opcode == 0x2f || opcode == 0x59 || opcode == 0x81) {
            try {
                Pm4::Execute(std::span(submission.commands).subspan(scanCursor, count), scanQueue);
                scanCursor = next;
            } catch (...) {
                break;
            }
        } else {
            break;
        }
    }

    if (batch.size() < MetalDraw::MinParallelDraws()) {
        return false;
    }

    if (timeline.UnknownWritesPending()) {
        ++drainsBy[0x100];
        timeline.Drain(0x100);
    }

    checkGpuFault(runInWorkingSet(draw->HostAliases(), ranges, [&] {
        return draw->DrawBatchParallel(batch, ranges, DeferWork() ? &timeline : nullptr, nullptr)
            .value_or(BdaAbi::Fault{});
    }));

    queue = std::move(scanQueue);
    cursor = scanCursor;
    return true;
}

bool MetalDriver::Impl::DeferLabelStore(std::span<const std::uint32_t> packet, std::uint32_t queueId) {
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    const bool interrupt = opcode == 0x49 && ((packet[2] >> 24u) & 7u) != 0;
    const auto label = Pm4::DecodeLabelWrite(packet);
    const auto store = PacketStoreRange(packet);
    const auto storeBytes = store ? store->bytes : 0;
    if (label ? storeBytes == 0 || storeBytes > 64 || label->Bytes().size() > 64 : storeBytes != 0) return false;
    // The store is guest-visible once it runs: what the work before it rendered reaches guest
    // memory ahead of it (MetalLazyCopyBacks rule 1).
    MaterializeCopyBacks(MetalLazyCopyBacks::Cause::Signal);
    if (label) {
        // The destination stops matching cached images in packet order, as when it runs now.
        ForgetCachedImages(*store);
        PendingLabel pending{label->address, {}, label->Bytes().size(), ++labelSerial};
        std::copy(label->Bytes().begin(), label->Bytes().end(), pending.bytes.begin());
        // Forgetting the oldest only makes a later wait on it take the slow path.
        if (pendingLabels.size() >= 256) pendingLabels.erase(pendingLabels.begin());
        pendingLabels.push_back(pending);
    }
    const auto serial = label ? labelSerial : 0;
    std::vector<std::uint32_t> copy(packet.begin(), packet.end());
    // The GPU signals the label point (MetalTimeline::DeferSignal); an end-of-pipe interrupt, which a
    // guest thread waits on, commits the batch so it is not delivered behind work encoded after it.
    timeline.DeferSignal([this, copy = std::move(copy), stores = label.has_value(), interrupt, queueId, serial] {
        if (serial != 0) std::erase_if(pendingLabels, [&](const PendingLabel& other) { return other.serial == serial; });
        // RELEASE_MEM and WRITE_DATA use no queue state; a timestamp is taken now, after the work.
        static QueueState* const scratch = new QueueState();
        Pm4::Execute(copy, *scratch);
        if (stores) {
            if (const auto range = PacketStoreRange(copy); range && range->bytes != 0 && range->bytes <= 64) {
                std::array<std::byte, 64> stored{};
                const auto written = std::span(stored).first(static_cast<std::size_t>(range->bytes));
                GuestMemory::Read(range->begin, written, 1);
                ownLabels.Record(range->begin, written);
            }
        }
        NotifyLabelStore();
        if (interrupt) {
            ForgetCachedImages(ForgetReason::Event);
            DeliverGuestEopInterrupt(queueId);
        }
    }, interrupt);
    return true;
}

bool MetalDriver::Impl::PendingLabelSatisfies(std::span<const std::uint32_t> packet) const {
    const auto address = static_cast<std::uint64_t>(packet[2]) | (static_cast<std::uint64_t>(packet[3]) << 32u);
    const auto bytes = Pm4::WaitAwaitedBytes(packet);
    for (auto label = pendingLabels.rbegin(); label != pendingLabels.rend(); ++label) {
        if (address + bytes <= label->address || label->address + label->size <= address) continue;
        if (address < label->address || address + bytes > label->address + label->size) return false;
        std::uint64_t value = 0;
        std::memcpy(&value, label->bytes.data() + (address - label->address), bytes);
        return Pm4::WaitComparesValue(packet, value);
    }
    return false;
}

void MetalDriver::Impl::ExecuteSubmission(const Submission& submission, QueueState& queue) {
    // #293: a mapping change no longer waits for the GPU to idle; it swaps
    // `ranges` with gpuMutex held. Every guest access below holds gpuMutex, and
    // re-borrows here after each (re)acquire, so a packet never runs on a stale
    // mapping and work parked across a change resumes on the new one.
    std::optional<NativeGuestMemory::BorrowedRangesScope> borrowed;
    std::uint64_t borrowedRevision = 0;
    // The guest may have written anything since the previous submission ran.
    ForgetCachedImages(ForgetReason::Submission);
    {
        std::lock_guard lock(gpuMutex);
        ownLabels.Reset();
    }
    const auto borrowCurrentRanges = [&] {
        if (borrowed && borrowedRevision == rangesRevision) return;
        borrowed.reset();
        borrowed.emplace(ranges);
        borrowedRevision = rangesRevision;
    };
    if (submission.suspend) {
        std::lock_guard lock(gpuMutex);
        borrowCurrentRanges();
        CheckFailureAndStopping();
        CompletePriorGpuWorkAndCopyBack();
        drawAhead.Retire(submission.serial);
        return;
    }
    if (BindTrace::Active()) {
        BindTrace::Log("submission queue %u serial %llu words %zu flips %zu", submission.queue,
            static_cast<unsigned long long>(submission.serial), submission.commands.size(), submission.flips.size());
    }
    if (PacketCensus::Enabled()) PacketCensus::NoteSubmission(submission);
    drawAhead.Post(submission, queue, draw.get(), nativeDevice);
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        progress.fetch_add(1, std::memory_order_relaxed);
        CheckFailureAndStopping();
        const auto header = submission.commands[cursor];
        const auto count = Pm4::PacketWords(header);
        if (count > submission.commands.size() - cursor) throw std::invalid_argument("Native Metal captured packet exceeds its command buffer");
        const auto packet = std::span(submission.commands).subspan(cursor, count);
        auto next = cursor + count;
        if (Pm4::FillerPacket(header)) { cursor = next; continue; }
        // A plain NOP carries no state: nothing to validate, order against or execute.
        if (Pm4::PureNopPacket(header)) { cursor = next; continue; }
        const auto opcode = (header >> 8u) & 0xffu;
        PacketCensus::PacketTimer censusTimer(packet);
        if (header != FlipPacketHeader && header != RenderingWaitPacketHeader) Pm4::Validate(packet, submission.queue);
        std::unique_lock lock(gpuMutex);
        borrowCurrentRanges();
        CheckFailureAndStopping();
        // Retire what the GPU finished meanwhile (labels, interrupts, earlier submissions) without waiting.
        if (!timeline.Empty()) timeline.Reap();
        // The predicate is guest memory earlier GPU work may write.
        if (Pm4::Predicated(header) && queue.predication.operation != 0) CompletePriorGpuWorkAndCopyBack(0x108);
        if (Pm4::Predicated(header) && queue.predication.operation != 0 && !Pm4::PredicationPasses(queue)) {
            const auto skippedFrom = cursor;
            cursor = opcode == 0x3f ? submission.conditionalEnds.at(cursor) : next;
            if (cursor < next || cursor > submission.commands.size()) throw std::invalid_argument("Native Metal predicated indirect-buffer end is invalid");
            if (BindTrace::Active()) {
                BindTrace::Log("predicated packet 0x%02x skipped (%zu words, predicate 0x%llx)", opcode, cursor - skippedFrom,
                    static_cast<unsigned long long>(queue.predication.address));
            }
            continue;
        }
        if (opcode == 0x3f) { cursor = next; continue; }
        if (header == RenderingWaitPacketHeader) {
            // Waits until video out no longer shows the display buffer the following work renders
            // into: a host condition that GPU work committed so far neither satisfies nor observes,
            // and work committed after the wait runs after that work on the queue. So the wait no
            // longer drains the timeline first (flips still do, so no deferred flip is pending
            // here), and the GPU keeps executing the frame meanwhile.
            if (!DeferWork()) CompletePriorGpuWorkAndCopyBack(0x105);
            ForgetCachedImages(ForgetReason::Wait);
            const auto wait = submission.renderingWaits.at(cursor);
            if (wait == nullptr) throw std::runtime_error("Native Metal rendering wait reservation is null");
            NoteWaitBlocked(submission.queue, 0, true);
            timeline.Flush();
            lock.unlock();
            try {
                CheckFailureAndStopping();
                wait->Wait();
                CheckFailureAndStopping();
            } catch (...) {
                NoteWaitBlocked(submission.queue, 0, false);
                throw;
            }
            NoteWaitBlocked(submission.queue, 0, false);
        } else if (header == FlipPacketHeader) {
            CompletePriorGpuWorkAndCopyBack(0x106);
            ForgetCachedImages(ForgetReason::Flip);
            if (FrameProfile::Enabled()) FrameProfile::Shared().Flip();
            if (PacketCensus::Enabled()) { censusTimer.Finish(); PacketCensus::Flip(); }
            if (draw != nullptr && GpuTiming::Enabled()) {
                const auto drawCache = draw->TakeDrawCacheStatistics();
                const auto encoders = MetalEncoders::TakeStatistics();
                if (drawCache.lookups != 0) {
                    std::fprintf(stderr, "{\"schema_version\":1,\"event\":\"draw_cache\",\"lookups\":%llu,\"hits\":%llu,\"misses\":%llu,\"inserts\":%llu,\"clears\":%llu,\"elisions\":%llu,\"rejected_images\":%llu,\"rejected_writes\":%llu,\"rejected_attachments\":%llu,\"parallel_passes\":%llu,\"parallel_draws\":%llu}\n",
                        static_cast<unsigned long long>(drawCache.lookups), static_cast<unsigned long long>(drawCache.hits),
                        static_cast<unsigned long long>(drawCache.misses), static_cast<unsigned long long>(drawCache.inserts),
                        static_cast<unsigned long long>(drawCache.clears), static_cast<unsigned long long>(drawCache.redundantStateElisions),
                        static_cast<unsigned long long>(drawCache.rejectedImages), static_cast<unsigned long long>(drawCache.rejectedWrites),
                        static_cast<unsigned long long>(drawCache.rejectedAttachments),
                        static_cast<unsigned long long>(encoders.parallelPasses), static_cast<unsigned long long>(encoders.parallelDraws));
                }
            }
            if (draw != nullptr && draw->ImageCache() != nullptr && GpuTiming::Enabled()) {
                static std::uint64_t flips = 0;
                if (++flips % 16 == 0) {
                    const auto cache = draw->ImageCache()->TakeStatistics();
                    std::fprintf(stderr, "{\"schema_version\":1,\"event\":\"image_cache\",\"flips\":16,\"hits\":%llu,\"misses\":%llu,\"inserts\":%llu,\"clears\":%llu,\"verified\":%llu,\"rejected\":%llu,\"hashed_mib\":%.1f,\"by\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu],\"invalidated\":%llu,\"invalidated_by\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu],\"evicted\":%llu,\"replaced\":%llu}\n",
                        static_cast<unsigned long long>(cache.hits), static_cast<unsigned long long>(cache.misses),
                        static_cast<unsigned long long>(cache.inserts), static_cast<unsigned long long>(cache.clears),
                        static_cast<unsigned long long>(cache.verified), static_cast<unsigned long long>(cache.rejected),
                        static_cast<double>(cache.hashedBytes) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(cache.clearsBy[0]), static_cast<unsigned long long>(cache.clearsBy[1]),
                        static_cast<unsigned long long>(cache.clearsBy[2]), static_cast<unsigned long long>(cache.clearsBy[3]),
                        static_cast<unsigned long long>(cache.clearsBy[4]), static_cast<unsigned long long>(cache.clearsBy[5]),
                        static_cast<unsigned long long>(cache.clearsBy[6]), static_cast<unsigned long long>(cache.clearsBy[7]),
                        static_cast<unsigned long long>(cache.invalidated),
                        static_cast<unsigned long long>(cache.invalidatedBy[0]), static_cast<unsigned long long>(cache.invalidatedBy[1]),
                        static_cast<unsigned long long>(cache.invalidatedBy[2]), static_cast<unsigned long long>(cache.invalidatedBy[3]),
                        static_cast<unsigned long long>(cache.invalidatedBy[4]), static_cast<unsigned long long>(cache.invalidatedBy[5]),
                        static_cast<unsigned long long>(cache.invalidatedBy[6]),
                        static_cast<unsigned long long>(cache.evicted), static_cast<unsigned long long>(cache.replaced));
                    std::fprintf(stderr, "{\"schema_version\":1,\"event\":\"gpu_timeline\",\"committed\":%llu,\"drains\":%llu,\"reruns\":%llu,\"command_buffers\":%llu}\n",
                        static_cast<unsigned long long>(timeline.Committed()), static_cast<unsigned long long>(timeline.Drains()),
                        static_cast<unsigned long long>(deferredReruns), static_cast<unsigned long long>(backend->CommandBuffersCreated()));
                    const auto lazy = draw->LazyCopyBacks().TakeStatistics();
                    std::fprintf(stderr, "{\"schema_version\":1,\"event\":\"lazy_copyback\",\"flips\":16,\"recorded\":%llu,\"merged\":%llu,\"encoded\":%llu,\"fused\":%llu,\"points\":%llu,\"by\":{\"drain\":%llu,\"signal\":%llu,\"retire\":%llu,\"cpu_read\":%llu,\"gpu_read\":%llu,\"overlap\":%llu,\"mapping\":%llu}}\n",
                        static_cast<unsigned long long>(lazy.recorded), static_cast<unsigned long long>(lazy.merged),
                        static_cast<unsigned long long>(lazy.encoded), static_cast<unsigned long long>(lazy.fused),
                        static_cast<unsigned long long>(lazy.points),
                        static_cast<unsigned long long>(lazy.by[0]), static_cast<unsigned long long>(lazy.by[1]),
                        static_cast<unsigned long long>(lazy.by[2]), static_cast<unsigned long long>(lazy.by[3]),
                        static_cast<unsigned long long>(lazy.by[4]), static_cast<unsigned long long>(lazy.by[5]),
                        static_cast<unsigned long long>(lazy.by[6]));
                    std::string top;
                    for (std::size_t i = 0; i < drainsBy.size(); ++i) {
                        if (drainsBy[i] == 0) continue;
                        char item[32];
                        std::snprintf(item, sizeof item, " %zx:%llu", i, static_cast<unsigned long long>(drainsBy[i]));
                        top += item;
                    }
                    std::fprintf(stderr, "[agc] drains by cause:%s; barrier waits %llu; indirect draws gpu %llu cpu %llu\n", top.c_str(),
                        static_cast<unsigned long long>(barrierWaits), static_cast<unsigned long long>(gpuIndirectDraws),
                        static_cast<unsigned long long>(cpuIndirectDraws));
                }
            }
            CheckFailureAndStopping();
            const auto flip = submission.flips.at(cursor);
            if (flip == nullptr) throw std::runtime_error("Native Metal flip reservation is null");
            if (frameSerial == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Native Metal frame serial overflow");
            auto frame = std::make_shared<FrameTiming>(++frameSerial);
            const auto now = FrameTiming::Clock::now();
            frame->IncludeSubmission(submission.serial, now, now, now, true);
            frame->SetFlip(submission.serial, cursor, now, now);
            lock.unlock();
            flip->GpuReady(frame);
            // A flip raises the queue's end-of-pipe event once the work before it completes, as a
            // RELEASE_MEM with INT_SEL does (upstream 326e0bc29); that work completed above. A driver
            // configured without an interrupt callback (presentation-only replays) has no event to raise.
            if (eopInterrupt) DeliverFlipEopInterrupt(submission.queue);
        } else if (opcode == 0x15) {
            Tolerate(SkippedDispatch, [&] { ExecuteDispatchSynchronously(queue, packet, submission); });
            Pm4::NoteDispatchRegisters(packet, queue);
        } else if (opcode == 0x16) {
            // The indirect arguments are guest memory earlier GPU work may write: the GPU reads them
            // itself when the dispatch runs (GpuIndirectDispatch), so nothing waits here.
            std::optional<std::uint64_t> arguments;
            try {
                arguments = Pm4::DispatchArgumentAddress(packet, queue);
            } catch (const std::exception&) {
            }
            if (arguments && GpuIndirectDispatch(packet.back(), *arguments)) {
                Tolerate(SkippedDispatch, [&] {
                    const std::array<std::uint32_t, 5> placeholder{0xc0031500u, 1, 1, 1, packet.back()};
                    ExecuteDispatchSynchronously(queue, placeholder, submission, arguments);
                });
            } else {
                CompletePriorGpuWorkAndCopyBack(opcode);
                Tolerate(SkippedDispatch, [&] {
                    const auto direct = Pm4::ResolveDispatch(packet, queue);
                    ExecuteDispatchSynchronously(queue, direct, submission);
                });
            }
            Pm4::NoteDispatchRegisters(packet, queue);
        } else if (Pm4::DrawOpcode(opcode)) {
            drawAhead.Progress(submission.serial, cursor);
            if (TryExecuteDrawBatch(cursor, queue, submission)) {
                continue;
            }
            Tolerate(SkippedDraw, [&] { ExecuteDrawSynchronously(queue, packet, submission); });
        } else if (opcode == 0x3c || opcode == 0x93) {
            if (timeline.Empty()) pendingLabels.clear();
            if (!timeline.Empty() && PendingLabelSatisfies(packet)) {
                ++barrierWaits;
                cursor = next;
                continue;
            }
            // Committed work after this wait runs after the work before it on the one Metal queue, so
            // when no committed work or pending label store of ours may write the awaited bytes, the
            // value is the guest CPU's (or a store of ours that already ran) and the CPU waits only for
            // it. Meanwhile the GPU goes on and the timeline retires what it finished, so labels of
            // ours the guest CPU waits on before it writes this one still reach it. A value our own
            // pending work may still write drains first, so the wait never passes on the bytes from
            // before that work. ANYPS5_METAL_GPU_WAITS=0 always drains first, as before.
            const auto address = static_cast<std::uint64_t>(packet[2]) | (static_cast<std::uint64_t>(packet[3]) << 32u);
            const auto awaitedEnd = address + Pm4::WaitAwaitedBytes(packet);
            const bool oursPending = timeline.MayHaveWritten(address, awaitedEnd) ||
                std::any_of(pendingLabels.begin(), pendingLabels.end(), [&](const PendingLabel& label) {
                    return address < label.address + label.size && label.address < awaitedEnd;
                });
            if (!GpuWaits() || oursPending) CompletePriorGpuWorkAndCopyBack(opcode);
            else timeline.Flush();
            bool blocked = false;
            try {
                while (!Pm4::WaitSatisfied(packet)) {
                    CheckFailureAndStopping();
                    if (!blocked) { NoteWaitBlocked(submission.queue, address, true); blocked = true; }
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    CheckFailureAndStopping();
                    lock.lock();
                    borrowCurrentRanges();
                    if (!timeline.Empty()) timeline.Reap();
                }
            } catch (...) {
                if (blocked) NoteWaitBlocked(submission.queue, address, false);
                throw;
            }
            if (blocked) NoteWaitBlocked(submission.queue, address, false);
            CheckFailureAndStopping();
            // A wait that our own earlier label store already satisfied is a GPU-to-GPU barrier; any
            // other (it blocked, or the guest wrote the value) may let guest CPU writes reach later work.
            std::array<std::byte, 8> awaited{};
            const auto awaitedBytes = std::span(awaited).first(Pm4::WaitAwaitedBytes(packet));
            GuestMemory::Read(address, awaitedBytes, 1);
            // ownLabels is shared by every queue's worker: guard it with gpuMutex like its Reset.
            const bool ours = !blocked && [&] { std::lock_guard lock(gpuMutex); return ownLabels.Holds(address, awaitedBytes); }();
            if (!ours) ForgetCachedImages(ForgetReason::Wait);
        } else if (opcode == 0x22) {
            CompletePriorGpuWorkAndCopyBack(opcode);
            if (Pm4::ReadCondition(packet) == 0) {
                next = submission.conditionalEnds.at(cursor);
                if (next < cursor + count || next > submission.commands.size()) throw std::invalid_argument("Native Metal conditional execution end is invalid");
                if (BindTrace::Active()) {
                    BindTrace::Log("COND_EXEC 0x%x%08x is zero: skipped %zu words", packet[2], packet[1], next - cursor - count);
                }
            }
        } else if (opcode == 0x42 || opcode == 0x58 || opcode == 0x46) {
            // Cache flushes and pipeline barriers store nothing here, and the Metal queue already
            // runs committed work in order; only the sample counter dump reads results on the CPU.
            if (opcode == 0x46 && (packet[1] & 0x3fu) == 0x39u) {
                CompletePriorGpuWorkAndCopyBack(opcode);
                if (draw == nullptr) throw std::runtime_error("Native Metal sample counter executor is not configured");
                const auto address = static_cast<std::uint64_t>(packet[2]) | (static_cast<std::uint64_t>(packet[3]) << 32u);
                ForgetCachedImages(ForgetReason::Event);
                draw->DumpSamplesSynchronously(address);
            }
        } else {
            if ((opcode == 0x49 || opcode == 0x37) && DeferWork() && !timeline.Empty() && DeferLabelStore(packet, submission.queue)) {
                cursor = next;
                continue;
            }
            // Packets the CPU executes that read or store guest memory wait for committed work first.
            // SET_*_REG_INDIRECT lists were read at submit time (CaptureIndirectRegisterLists), so these
            // packets only update registers here.
            const bool indirectRegisters = Pm4::IndirectRegisterOpcode(opcode);
            if (!indirectRegisters && !RegisterOnlyPacket(opcode)) CompletePriorGpuWorkAndCopyBack(opcode);
            // A store the driver performs itself: only its destination stops matching cached images.
            // (A label it writes lets the guest go on, but guest writes reach later work of this
            // submission only behind a wait, which forgets everything.)
            if (const auto store = PacketStoreRange(packet)) ForgetCachedImages(*store);
            if (PacketCensus::Enabled()) PacketCensus::NoteRegisterWrites(packet, indirectRegisters ? &submission.registerLists.at(cursor) : nullptr, queue);
            if (indirectRegisters) Pm4::ExecuteIndirectRegisters(packet, submission.registerLists.at(cursor), queue);
            else Pm4::Execute(packet, queue);
            // Remember label values this driver wrote (RELEASE_MEM data, WRITE_DATA), for the wait above.
            if (opcode == 0x49 || opcode == 0x37) {
                if (const auto store = PacketStoreRange(packet); store && store->bytes != 0 && store->bytes <= 64) {
                    std::array<std::byte, 64> stored{};
                    const auto written = std::span(stored).first(static_cast<std::size_t>(store->bytes));
                    GuestMemory::Read(store->begin, written, 1);
                    std::lock_guard lock(gpuMutex);
                    ownLabels.Record(store->begin, written);
                }
            }
            if (opcode == 0x49 || opcode == 0x37 || opcode == 0x40 || opcode == 0x50 || opcode == 0x83) NotifyLabelStore();
            if (opcode == 0x49 && ((packet[2] >> 24u) & 7u) != 0) {
                CompletePriorGpuWorkAndCopyBack();
                ForgetCachedImages(ForgetReason::Event);
                DeliverGuestEopInterrupt(submission.queue);
            }
        }
        cursor = next;
    }
    {
        std::lock_guard lock(gpuMutex);
        borrowCurrentRanges();
        CheckFailureAndStopping();
        // With deferred work the worker goes on to the next submission; MetalDriver::Impl::Run
        // retires this one once its GPU work completed.
        if (!DeferWork()) CompletePriorGpuWorkAndCopyBack(0x107);
        ForgetCachedImages(ForgetReason::Submission);
    }
    borrowed.reset();
    drawAhead.Retire(submission.serial);
    if (submission.rewindTail != nullptr) ExecuteRewindTail(submission, queue);
}

}
