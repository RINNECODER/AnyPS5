#include "MetalComputeDispatch.hpp"
#include "MetalGuestMemory.hpp"
#include "MetalShaderPipeline.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

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

namespace Abi = ShaderRecompiler::BdaAbi;

void validatePhysicalRanges(std::span<const NativeGuestMemory::BorrowedRange> ranges) {
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> addresses;
    for (const auto& range : ranges) {
        const auto begin = reinterpret_cast<std::uintptr_t>(range.host.data());
        if (range.host.size() > std::numeric_limits<std::uintptr_t>::max() - begin) {
            throw std::invalid_argument("Metal borrowed host range address overflows");
        }
        addresses.emplace_back(begin, begin + range.host.size());
    }
    std::sort(addresses.begin(), addresses.end());
    for (std::size_t i = 1; i < addresses.size(); ++i) {
        if (addresses[i].first < addresses[i - 1].second) {
            throw std::invalid_argument("Metal compute borrows physically overlapping host ranges at different guest addresses");
        }
    }
}

void appendShaderRegion(std::vector<MemoryRegion>& memory, std::uint64_t address,
                        std::span<const std::byte> bytes) {
    if (bytes.empty()) return;
    if (address == 0 || bytes.size() > std::numeric_limits<std::uint64_t>::max() - address) {
        throw std::invalid_argument("Metal shader guest address or byte range is invalid");
    }
    const auto end = address + bytes.size();
    for (const auto& region : memory) {
        const auto regionEnd = region.guestAddress + region.bytes.size();
        if (address >= regionEnd || end <= region.guestAddress) continue;
        if (address < region.guestAddress || end > regionEnd) {
            throw std::invalid_argument("Metal shader spans partially overlapping borrowed guest memory");
        }
        const auto offset = static_cast<std::size_t>(address - region.guestAddress);
        if (std::memcmp(region.bytes.data() + offset, bytes.data(), bytes.size()) != 0) {
            throw std::invalid_argument("Metal checked shader bytes disagree with borrowed guest memory");
        }
        return;
    }
    memory.push_back({address, bytes});
}

SpirvTarget target(id<MTLDevice> device) {
    static constexpr std::array<std::uint32_t, 7> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityMinLod, spv::CapabilitySampledImageArrayDynamicIndexing,
        spv::CapabilityShaderNonUniform, spv::CapabilitySampledImageArrayNonUniformIndexing};
    static constexpr std::array<std::string_view, 3> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage", "SPV_EXT_descriptor_indexing"};
    const auto maximum = device.maxThreadsPerThreadgroup;
    return {0x00401000, 0x00010300, 32, Abi::Version, capabilities, extensions, false,
            {static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height),
             static_cast<std::uint32_t>(maximum.depth)}, static_cast<std::uint32_t>(maximum.width),
            static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
}

struct CopyBack {
    std::span<std::byte> host;
    MetalBufferBinding binding;
};

MetalBufferBinding bindGuestRange(const MetalGuestMemory::DispatchSnapshot& snapshot,
                                 std::uint64_t address, std::uint64_t bytes, bool writable,
                                 std::vector<CopyBack>& copyBack) {
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() ||
        bytes > std::numeric_limits<std::uint64_t>::max() - address) {
        throw std::invalid_argument("Metal guest buffer descriptor byte range is invalid");
    }
    auto host = NativeGuestMemory::ContiguousBorrowedRange(address, static_cast<std::size_t>(bytes), writable);
    NSArray<id<MTLBuffer>>* buffers = snapshot.Buffers();
    Abi::Header header{};
    std::memcpy(&header, snapshot.Table().contents, sizeof(header));
    if (header.version != Abi::Version || header.count != buffers.count || header.entryBytes != sizeof(Abi::Range)) {
        throw std::runtime_error("Metal compute snapshot table metadata is invalid");
    }
    for (std::uint32_t i = 0; i < header.count; ++i) {
        Abi::Range range{};
        std::memcpy(&range, static_cast<const std::byte*>(snapshot.Table().contents) + sizeof(header) + i * sizeof(range), sizeof(range));
        if (address < range.begin || address >= range.end || bytes > range.end - address) continue;
        id<MTLBuffer> buffer = buffers[i];
        if (range.deviceAddress < buffer.gpuAddress) throw std::runtime_error("Metal snapshot GPU address offset is invalid");
        const auto offset = range.deviceAddress - buffer.gpuAddress + address - range.begin;
        if (offset % sizeof(std::uint32_t) != 0) {
            throw std::invalid_argument("Metal guest buffer descriptor view must be DWORD aligned");
        }
        if (offset >= buffer.length || bytes > buffer.length - offset) {
            throw std::runtime_error("Metal snapshot buffer descriptor exceeds its mirror");
        }
        MetalBufferBinding binding{buffer, static_cast<NSUInteger>(offset), static_cast<NSUInteger>(bytes)};
        if (writable) copyBack.push_back({host, binding});
        return binding;
    }
    throw std::out_of_range("Metal guest buffer descriptor has no coherent snapshot mirror");
}

bool bindingWritten(const DescriptorBinding& binding, std::uint32_t element) {
    if (!binding.bufferWritten.empty() && binding.bufferWritten.size() != binding.count) {
        throw std::invalid_argument("Metal guest buffer write metadata has the wrong descriptor count");
    }
    if (!binding.bufferAtomic.empty() && binding.bufferAtomic.size() != binding.count) {
        throw std::invalid_argument("Metal guest buffer atomic metadata has the wrong descriptor count");
    }
    const bool atomic = !binding.bufferAtomic.empty() && binding.bufferAtomic[element];
    const bool written = binding.bufferWritten.empty() ? !binding.readOnly : binding.bufferWritten[element];
    if (binding.readOnly && (atomic || written)) {
        throw std::invalid_argument("Metal guest buffer read-only and write metadata disagree");
    }
    return written || atomic;
}

}

MetalComputeDispatch::MetalComputeDispatch(id<MTLDevice> device) : device(device) {
    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple7] || ![device supportsFamily:MTLGPUFamilyMetal3]) {
        throw std::invalid_argument("Native Metal compute dispatch requires an Apple Silicon Metal 3 device");
    }
    queue = [device newCommandQueue];
    if (queue == nil) throw std::runtime_error("Native Metal compute dispatch queue creation failed");
}

Abi::Fault MetalComputeDispatch::DispatchSynchronously(const ComputeDispatchState& state,
    std::span<const std::uint32_t> checkedCode, std::span<const NativeGuestMemory::BorrowedRange> ranges,
    std::uint64_t checkedHeaderAddress, std::span<const std::byte> checkedHeader) {
    std::lock_guard lock(dispatchMutex);
    NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    validatePhysicalRanges(ranges);
    if (checkedCode.empty() || state.programAddress == 0 || state.programAddress % sizeof(std::uint32_t) != 0 ||
        (state.waveSize != 32 && state.waveSize != 64) ||
        std::any_of(state.compute.numThreads.begin(), state.compute.numThreads.end(), [](auto value) { return value == 0; })) {
        throw std::invalid_argument("Native Metal compute dispatch has invalid shader or decoded execution state");
    }
    if (checkedHeader.empty() != (checkedHeaderAddress == 0)) {
        throw std::invalid_argument("Native Metal compute shader header span and address must be supplied together");
    }
    std::vector<MemoryRegion> memory;
    for (const auto& range : ranges) memory.push_back({range.guestAddress, range.host});
    appendShaderRegion(memory, state.programAddress, std::as_bytes(checkedCode));
    appendShaderRegion(memory, checkedHeaderAddress, checkedHeader);
    if (std::any_of(state.groups.begin(), state.groups.end(), [](auto value) { return value == 0; })) return {};

    RecompileRequest request{{ShaderStage::Compute, state.programAddress, checkedCode, checkedHeaderAddress, checkedHeader},
        {state.waveSize, 0, state.userData, state.compute, {}, {}, memory}, target(device), {0, 0, 0, 128}, {}, true};
    const auto guest = Recompile(request);
    MetalBackend::TargetOptions options;
    if (@available(macOS 15.0, *))
        options.supportsWorkgroupAtomicFences = [device supportsFamily:MTLGPUFamilyApple1];
    options.supportsInt64 = true;
    options.supportsGpuAddresses = true;
    options.supportsSimdGroups = true;
    configureSamplerArguments(options, guest, device);
    const auto converted = MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
    MetalComputePipeline pipeline(device, converted);
    MetalGuestMemory guestMemory(device);
    for (const auto& range : ranges) {
        guestMemory.RegisterBorrowedHostSpanUntilSnapshotsComplete(range.guestAddress, range.host, range.writable);
    }
    auto snapshot = guestMemory.CaptureAfterPriorSnapshotsComplete();
    std::vector<MetalShaderResourceBinding> bindings;
    std::vector<CopyBack> copyBack;
    id<MTLBuffer> emptyBuffer = nil;
    for (const auto& mapping : converted.resources) {
        if (!mapping.active) continue;
        const auto found = std::find_if(guest.bindings.begin(), guest.bindings.end(), [&mapping](const auto& binding) {
            return binding.descriptorSet == mapping.descriptorSet && binding.binding == mapping.binding;
        });
        if (found == guest.bindings.end() ||
            (found->kind != DescriptorKind::StorageBuffer && found->kind != DescriptorKind::UniformBuffer)) {
            throw std::invalid_argument("Native Metal compute dispatch requires implemented buffer descriptor metadata");
        }
        MetalShaderResourceBinding native;
        native.descriptorSet = found->descriptorSet;
        native.binding = found->binding;
        switch (found->role) {
        case DescriptorRole::GuestBuffers:
            if (found->guestDescriptor.size() != static_cast<std::size_t>(found->count) * 4) {
                throw std::invalid_argument("Native Metal guest buffer descriptor words are incomplete");
            }
            for (std::uint32_t i = 0; i < found->count; ++i) {
                ShaderBufferResource descriptor;
                std::copy_n(found->guestDescriptor.begin() + i * 4u, 4, descriptor.fields.begin());
                const bool written = bindingWritten(*found, i);
                if (descriptor.GetSize() == 0 || descriptor.Base48() == 0) {
                    if (emptyBuffer == nil) {
                        const std::uint32_t zero = 0;
                        emptyBuffer = [device newBufferWithBytes:&zero length:sizeof(zero) options:MTLResourceStorageModeShared];
                        if (emptyBuffer == nil) throw std::runtime_error("Metal empty descriptor buffer allocation failed");
                    }
                    native.buffers.push_back({emptyBuffer, 0, 0});
                } else {
                    native.buffers.push_back(bindGuestRange(snapshot, descriptor.Base48(), descriptor.GetSize(), written, copyBack));
                }
            }
            break;
        case DescriptorRole::ShaderData:
        case DescriptorRole::FlattenedSrt: {
            // Runtime ABI words the recompiler captured (user data, buffer offsets, flattened SRT):
            // image records would name heaps this buffer-only adapter never binds.
            if (found->count != 1 || found->guestDescriptor.empty() ||
                (found->role == DescriptorRole::ShaderData && guest.runtimeImageCount != 0)) {
                throw std::invalid_argument("Native Metal compute captured shader data binding is invalid");
            }
            const auto bytes = found->guestDescriptor.size() * sizeof(std::uint32_t);
            id<MTLBuffer> buffer = [device newBufferWithBytes:found->guestDescriptor.data() length:bytes options:MTLResourceStorageModeShared];
            if (buffer == nil) throw std::runtime_error("Native Metal compute captured shader data allocation failed");
            native.buffers.push_back({buffer, 0, bytes});
            break;
        }
        case DescriptorRole::BdaPagetable:
        case DescriptorRole::FaultBuffer: {
            if (found->count != 1) throw std::invalid_argument("Native Metal BDA descriptors require exactly one buffer");
            id<MTLBuffer> buffer = found->role == DescriptorRole::BdaPagetable ? snapshot.Table() : snapshot.FaultBuffer();
            native.buffers.push_back({buffer, 0, buffer.length});
            break;
        }
        default:
            throw std::invalid_argument("Native Metal compute descriptor role requires an unimplemented resource adapter");
        }
        bindings.push_back(std::move(native));
    }
    std::vector<id<MTLResource>> resident;
    for (id<MTLBuffer> buffer in snapshot.Buffers()) resident.push_back(buffer);
    if (converted.requiresGpuAddresses && resident.empty()) resident.push_back(snapshot.Table());
    std::array<NSUInteger, 3> grid{};
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        const auto threads = static_cast<std::uint64_t>(state.groups[axis]) * converted.threadsPerThreadgroup[axis];
        if (threads > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Native Metal compute dispatch grid exceeds the shader integer range");
        }
        grid[axis] = static_cast<NSUInteger>(threads);
    }
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    if (commands == nil) throw std::runtime_error("Native Metal compute dispatch command creation failed");
    pipeline.Encode(commands, bindings, MTLSizeMake(grid[0], grid[1], grid[2]), {}, resident);
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) {
        const char* error = commands.error.localizedDescription.UTF8String;
        throw std::runtime_error(std::string("Native Metal compute execution failed: ") + (error == nullptr ? "unknown GPU error" : error));
    }
    const auto fault = snapshot.CompleteAndCopyDirtyPagesToBorrowedHost(commands);
    for (const auto& write : copyBack) {
        std::memcpy(write.host.data(), static_cast<const std::byte*>(write.binding.buffer.contents) + write.binding.offset,
                    write.host.size());
    }
    return fault;
}

}
