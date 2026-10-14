#include "MetalShaderResources.hpp"
#include "GpuTiming.hpp"
#include "BindTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "RuntimeAbi.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace AgcDriver::Metal {

namespace {
using namespace ShaderRecompiler;
namespace Abi = ShaderRecompiler::BdaAbi;

MetalGuestMemory::DispatchSnapshot capture(MetalGuestMemory& memory,
    std::span<const NativeGuestMemory::BorrowedRange> ranges, MetalGuestHostAliases* aliases) {
    for (const auto& range : ranges) {
        const auto begin = reinterpret_cast<std::uintptr_t>(range.host.data());
        if (range.host.size() > std::numeric_limits<std::uintptr_t>::max() - begin) {
            throw std::invalid_argument("Metal draw borrowed host address overflows");
        }
        memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(range.guestAddress, range.host, range.writable);
    }
    return memory.CaptureAfterPriorSnapshotsComplete(aliases);
}

// A short description of a guest image for skip reasons: no address, so equal images share a reason.
std::string describeImage(const Graphics::GuestTextureResource& resource) {
    char text[160];
    std::snprintf(text, sizeof text, "format %u %ux%ux%u mips %u-%u of %u samples %u tile %u",
        resource.format, resource.width, resource.height, resource.depthOrLastArray + 1u, resource.baseLevel,
        resource.lastLevel, resource.mipCount, resource.samples, static_cast<unsigned>(resource.tileMode));
    return text;
}

// BindTrace: one image binding, and for guest memory how many of 64 evenly spaced texels are non-zero.
// Non-zero texels among 64 evenly spaced guest texels of the image, or -1 when unreadable.
int probeGuest(const Graphics::GuestTextureResource& resource) {
    const auto geometry = Graphics::DescribeSurface(resource);
    const auto elementBytes = Graphics::BytesPerElement(resource.format);
    if (geometry.guestBytes == 0 || elementBytes == 0 || elementBytes > 16 ||
        !GuestMemory::Accessible(reinterpret_cast<const void*>(resource.baseAddress), static_cast<std::size_t>(geometry.guestBytes), false)) {
        return -1;
    }
    int nonZero = 0;
    const auto stride = geometry.guestBytes / 64u / elementBytes * elementBytes;
    for (std::uint64_t probeIndex = 0; probeIndex < 64; ++probeIndex) {
        std::array<std::byte, 16> texel{};
        GuestMemory::Read(resource.baseAddress + probeIndex * stride, std::span(texel).first(elementBytes));
        nonZero += std::any_of(texel.begin(), texel.begin() + elementBytes, [](std::byte value) { return value != std::byte{0}; });
    }
    return nonZero;
}

void traceImage(std::uint32_t set, std::uint32_t binding, std::uint32_t element, const Graphics::GuestTextureResource& resource,
    bool storage, bool written, bool resident) {
    char probe[48] = "";
    if (!resident) {
        const auto nonZero = probeGuest(resource);
        if (nonZero >= 0) std::snprintf(probe, sizeof probe, " guest non-zero %d/64", nonZero);
        else std::snprintf(probe, sizeof probe, " guest unreadable");
    }
    BindTrace::Log("  %s %u.%u[%u] 0x%llx %s dim %u array %u-%u%s -> %s%s", storage ? (written ? "storage-rw" : "storage-r") : "sampled",
        set, binding, element, static_cast<unsigned long long>(resource.baseAddress), describeImage(resource).c_str(),
        static_cast<unsigned>(resource.dimension), resource.baseArray, resource.depthOrLastArray,
        resource.dccAddress != 0 ? " dcc" : "", resident ? "resident surface" : "guest memory", probe);
}

auto textureKey(const Graphics::GuestTextureResource& resource, bool compare) {
    return std::tuple(resource.baseAddress, resource.width, resource.height, resource.depthOrLastArray,
        resource.baseArray, resource.mipCount, resource.baseLevel, resource.lastLevel, resource.tileMode,
        resource.dimension, resource.format, resource.dstSelX, resource.dstSelY, resource.dstSelZ,
        resource.dstSelW, resource.dccAddress, resource.dccAlphaOnMsb, resource.dccPipeAligned, resource.minLod, resource.allocatedMipCount, compare);
}

bool overlaps(std::uint64_t begin, std::uint64_t end, std::uint64_t otherBegin, std::uint64_t otherEnd) {
    return begin < otherEnd && otherBegin < end;
}

bool arrayImage(const Graphics::GuestTextureResource& resource) {
    return resource.dimension == Graphics::TextureDimension::k2DArray || resource.dimension == Graphics::TextureDimension::k1DArray;
}

// Two array descriptors that address slice ranges of one allocation: the same base, texel layout and
// mip chain, differing at most in their first and last array slice.
bool sameArrayAllocation(const Graphics::GuestTextureResource& a, const Graphics::GuestTextureResource& b) {
    return arrayImage(a) && a.dimension == b.dimension &&
        std::tie(a.baseAddress, a.width, a.height, a.mipCount, a.allocatedMipCount, a.tileMode, a.samples, a.dccAddress, a.dccAlphaOnMsb, a.dccPipeAligned) ==
        std::tie(b.baseAddress, b.width, b.height, b.mipCount, b.allocatedMipCount, b.tileMode, b.samples, b.dccAddress, b.dccAlphaOnMsb, b.dccPipeAligned) &&
        Graphics::BytesPerElement(a.format) == Graphics::BytesPerElement(b.format);
}

// A guest array image is often bound through several descriptors that each address part of its slices:
// one dispatch binds slice 0 of a two-slice image through a descriptor whose last array slice is 0 and
// slice 1 through one whose base and last slice are 1. On the GPU both are views of one allocation, but
// each descriptor alone describes a differently sized surface, so the captured backings would overlap
// without sharing. Every array descriptor of a shader's image bindings is therefore widened to the
// last slice any descriptor of the same allocation addresses; each keeps its own base slice.
std::vector<Graphics::GuestTextureResource> widestArrays(const MetalBackend::Result& shader) {
    std::vector<Graphics::GuestTextureResource> widest;
    for (const auto& mapping : shader.resources) {
        if (!mapping.active) continue;
        for (const auto& binding : shader.guest.bindings) {
            if (binding.descriptorSet != mapping.descriptorSet || binding.binding != mapping.binding ||
                binding.role != DescriptorRole::GuestImages || binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 8) {
                continue;
            }
            for (std::uint32_t i = 0; i < binding.count; ++i) {
                const auto words = std::span(binding.guestDescriptor).subspan(i * 8u, 8);
                if (NullImageDescriptor(words)) continue;
                Graphics::GuestTextureResource descriptor;
                try {
                    descriptor = Graphics::DecodeTextureResource(words);
                } catch (const std::exception&) {
                    continue; // The binding pass reports the descriptor.
                }
                if (!arrayImage(descriptor)) continue;
                const auto found = std::find_if(widest.begin(), widest.end(), [&](const auto& other) { return sameArrayAllocation(other, descriptor); });
                if (found == widest.end()) widest.push_back(descriptor);
                else found->depthOrLastArray = std::max(found->depthOrLastArray, descriptor.depthOrLastArray);
            }
        }
    }
    return widest;
}

// The runtime ABI image records in a shader's ShaderData words name element ranges of the image heaps
// the shader binds; a dynamic image index reads its element from them, so every record must lie
// inside its bound heap before the words reach the GPU (as the shared driver's
// ValidateRuntimeResources requires).
void validateRuntimeImages(const ShaderRecompiler::RecompileResult& program, std::span<const std::uint32_t> words) {
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::invalid_argument(std::string("Metal runtime image metadata: ") + message);
    };
    ShaderRecompiler::RuntimeAbi::RequireVersion(program.runtimeAbiVersion);
    constexpr auto bindingCount = static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Count);
    std::array<const ShaderRecompiler::DescriptorBinding*, bindingCount> heaps{};
    if (program.runtimeImageCount != 0u) {
        for (const auto& binding : program.bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
            auto& heap = heaps[binding.binding % bindingCount];
            require(heap == nullptr, "duplicate runtime image binding");
            heap = &binding;
        }
    }
    require(words.size() == program.shaderDataDwords, "invalid compact shader data size");
    constexpr auto metadataWords = sizeof(ShaderRecompiler::RuntimeAbi::ResourceMetadata) / sizeof(std::uint32_t);
    require(program.imageMetadataDword <= words.size() &&
        program.runtimeImageCount <= (words.size() - program.imageMetadataDword) / metadataWords, "records exceed shader data");
    for (const auto index : program.runtimeImageResources) {
        require(index < program.runtimeImageCount, "record exceeds its compact layout");
        const auto offset = program.imageMetadataDword + index * metadataWords;
        const auto kind = words[offset];
        const auto first = words[offset + 1u];
        const auto count = words[offset + 2u];
        require(count != 0u, "record has no descriptor elements");
        require(kind < heaps.size() && heaps[kind] != nullptr, "record references an unbound image heap");
        require(first < heaps[kind]->count && count <= heaps[kind]->count - first, "record exceeds its bound heap");
    }
}

// The keys an uncompressed store covers after an image write: the console's DCC extent
// (Graphics::DccKeyCount) for pipe-aligned metadata, else one key per 256 surface bytes. As in the
// shared driver, metadata that is writable only over the shorter range is not modeled.
std::size_t storedKeyCount(const Graphics::GuestTextureResource& descriptor, std::size_t bytes) {
    const auto keys = Graphics::DccKeyBytes(bytes);
    const auto count = Graphics::DccKeyCount(descriptor, bytes);
    if (count > std::numeric_limits<std::uint64_t>::max() - descriptor.dccAddress) {
        throw std::invalid_argument("Metal image DCC metadata range overflows");
    }
    if (count == keys || descriptor.dccAddress == 0 ||
        GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.dccAddress), count, true)) return count;
    char message[256];
    std::snprintf(message, sizeof(message),
        "Metal DCC metadata 0x%llx is not writable over the 0x%zx key bytes of its surface (0x%zx at one per 256 bytes)",
        static_cast<unsigned long long>(descriptor.dccAddress), count, keys);
    throw NativeGuestMemory::ReadOnlyWriteError(message);
}

void validateCommittedImageWrite(std::span<const NativeGuestMemory::BorrowedRange> ranges,
    std::uint64_t address, std::size_t bytes) {
    for (const auto& range : ranges) {
        // Sparse holes may discard stores, but committed read-only bytes must never
        // be exposed through a writable native view, including staged view upgrades.
        if (!range.writable && overlaps(address, address + bytes, range.guestAddress, range.guestAddress + range.host.size())) {
            throw NativeGuestMemory::ReadOnlyWriteError("Metal storage texture writes overlap read-only committed guest intervals");
        }
    }
}

bool writtenBuffer(const DescriptorBinding& binding, std::uint32_t i) {
    if ((!binding.bufferWritten.empty() && binding.bufferWritten.size() != binding.count) ||
        (!binding.bufferAtomic.empty() && binding.bufferAtomic.size() != binding.count)) {
        throw std::invalid_argument("Metal draw buffer write metadata has the wrong descriptor count");
    }
    const bool atomic = !binding.bufferAtomic.empty() && binding.bufferAtomic[i];
    const bool written = binding.bufferWritten.empty() ? !binding.readOnly : binding.bufferWritten[i];
    if (binding.readOnly && (atomic || written)) throw std::invalid_argument("Metal draw buffer permission metadata disagrees");
    return atomic || written;
}

}

MetalShaderResources::MetalShaderResources(const MetalDevice& backend,
    std::span<const NativeGuestMemory::BorrowedRange> ranges, DepthTextureLookup depthLookup, MetalGuestHostAliases* aliases,
    ImageCache* imageCache)
    : backend(backend), borrowed(std::in_place, ranges), borrowedRanges(ranges.begin(), ranges.end()),
      memory(backend.Device()), snapshot(capture(memory, ranges, aliases)),
      depthLookup(std::move(depthLookup)), aliases(aliases), imageCache(imageCache) {
    resident.push_back(snapshot.Table());
    resident.push_back(snapshot.FaultBuffer());
    for (id<MTLBuffer> buffer in snapshot.Buffers()) resident.push_back(buffer);
}

void MetalShaderResources::forgetCachedImages(std::uint64_t begin, std::uint64_t end) {
    if (imageCache == nullptr) return;
    for (const auto& range : borrowedRanges) {
        const auto clippedBegin = std::max(begin, range.guestAddress);
        const auto clippedEnd = std::min(end, range.guestAddress + range.host.size());
        if (clippedBegin < clippedEnd) imageCache->Invalidate(range.host.data() + (clippedBegin - range.guestAddress), clippedEnd - clippedBegin);
    }
}

bool MetalShaderResources::physicallyOverlaps(std::uint64_t begin, std::uint64_t end,
    std::uint64_t otherBegin, std::uint64_t otherEnd) const {
    for (const auto& range : borrowedRanges) {
        const auto clippedBegin = std::max(begin, range.guestAddress);
        const auto clippedEnd = std::min(end, range.guestAddress + range.host.size());
        if (clippedBegin >= clippedEnd) continue;
        const auto hostBegin = reinterpret_cast<std::uintptr_t>(range.host.data()) + clippedBegin - range.guestAddress;
        const auto hostEnd = hostBegin + clippedEnd - clippedBegin;
        for (const auto& other : borrowedRanges) {
            const auto clippedOtherBegin = std::max(otherBegin, other.guestAddress);
            const auto clippedOtherEnd = std::min(otherEnd, other.guestAddress + other.host.size());
            if (clippedOtherBegin >= clippedOtherEnd) continue;
            const auto otherHostBegin = reinterpret_cast<std::uintptr_t>(other.host.data()) + clippedOtherBegin - other.guestAddress;
            if (overlaps(hostBegin, hostEnd, otherHostBegin, otherHostBegin + clippedOtherEnd - clippedOtherBegin)) return true;
        }
    }
    return false;
}

MetalBufferBinding MetalShaderResources::mirror(std::uint64_t address, std::size_t bytes, bool writable) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - address) {
        throw std::invalid_argument("Metal draw guest byte range is invalid");
    }
    static_cast<void>(NativeGuestMemory::ContiguousBorrowedRange(address, bytes, writable));
    auto buffers = snapshot.Buffers();
    Abi::Header header{};
    std::memcpy(&header, snapshot.Table().contents, sizeof(header));
    if (header.count != buffers.count || header.version != Abi::Version || header.entryBytes != sizeof(Abi::Range)) {
        throw std::runtime_error("Metal draw snapshot table is inconsistent");
    }
    for (std::uint32_t i = 0; i < header.count; ++i) {
        Abi::Range range{};
        std::memcpy(&range, static_cast<const std::byte*>(snapshot.Table().contents) + sizeof(header) + i * sizeof(range), sizeof(range));
        if (address < range.begin || address >= range.end || bytes > range.end - address) continue;
        id<MTLBuffer> buffer = buffers[i];
        if (range.deviceAddress < buffer.gpuAddress) throw std::runtime_error("Metal draw mirror GPU address is invalid");
        const auto offset = range.deviceAddress - buffer.gpuAddress + address - range.begin;
        if (offset >= buffer.length || bytes > buffer.length - offset) throw std::runtime_error("Metal draw mirror extent is invalid");
        return {buffer, static_cast<NSUInteger>(offset), bytes};
    }
    // Bytes of adjacent guest mappings that alias consecutive host pages: one alias over them.
    if (const auto bridged = snapshot.Bridge(address, bytes, aliases)) return {bridged->first, static_cast<NSUInteger>(bridged->second), bytes};
    // Borrowed bytes outside the alias working set: admit them and let the caller set up again.
    if (aliases != nullptr && aliases->AdmitGuest(borrowedRanges, address, bytes)) {
        throw MetalWorkingSetMiss("Metal draw guest range is outside the alias working set");
    }
    throw std::out_of_range("Metal draw guest range has no shared mirror");
}

MetalBufferBinding MetalShaderResources::stageStorageImage(std::uint64_t address, std::size_t bytes,
    std::vector<ImageRange>& writableRanges) {
    std::vector<ImageRange> readableRanges;
    const auto end = address + bytes;
    for (const auto& range : borrowedRanges) {
        const auto begin = std::max(address, range.guestAddress);
        const auto limit = std::min(end, range.guestAddress + range.host.size());
        if (begin >= limit) continue;
        const auto size = static_cast<std::size_t>(limit - begin);
        const ImageRange clipped{static_cast<std::size_t>(begin - address),
            range.host.subspan(static_cast<std::size_t>(begin - range.guestAddress), size),
            mirror(begin, size, false)};
        for (const auto& other : readableRanges) {
            const auto host = reinterpret_cast<std::uintptr_t>(clipped.host.data());
            const auto otherHost = reinterpret_cast<std::uintptr_t>(other.host.data());
            if (overlaps(host, host + clipped.host.size(), otherHost, otherHost + other.host.size())) {
                throw std::invalid_argument("Metal storage image committed intervals alias each other physically");
            }
        }
        readableRanges.push_back(clipped);
        if (range.writable) writableRanges.push_back(clipped);
    }
    if (writableRanges.empty()) {
        throw NativeGuestMemory::ReadOnlyWriteError("Metal storage texture has no writable committed guest intervals");
    }
    id<MTLBuffer> buffer = backend.Buffer(bytes);
    std::memset(buffer.contents, 0, bytes);
    for (const auto& range : readableRanges) {
        std::memcpy(static_cast<std::byte*>(buffer.contents) + range.offset,
            static_cast<const std::byte*>(range.mirror.buffer.contents) + range.mirror.offset, range.host.size());
    }
    return {buffer, 0, bytes};
}

MetalBufferBinding MetalShaderResources::Buffer(std::uint64_t address, std::size_t bytes, bool writable) {
    auto buffer = mirror(address, bytes, writable);
    for (const auto& image : images) {
        const auto& descriptor = image.texture->Descriptor();
        const auto begin = descriptor.baseAddress;
        const auto count = Graphics::DccKeyCount(descriptor, image.texture->GuestBytes());
        if (writable && descriptor.dccAddress != 0 &&
            (overlaps(address, address + bytes, descriptor.dccAddress, descriptor.dccAddress + count) ||
             physicallyOverlaps(address, address + bytes, descriptor.dccAddress, descriptor.dccAddress + count))) {
            throw std::invalid_argument("Metal draw guest buffer writes alias active image DCC metadata");
        }
        if (overlaps(address, address + bytes, begin, begin + image.texture->GuestBytes()) ||
            physicallyOverlaps(address, address + bytes, begin, begin + image.texture->GuestBytes())) {
            throw std::invalid_argument("Metal draw guest buffer aliases an active native image");
        }
    }
    bufferRanges.emplace_back(address, address + bytes);
    if (writable) forgetCachedImages(address, address + bytes);
    if (writable) bufferWrites.push_back({address, NativeGuestMemory::ContiguousBorrowedRange(address, bytes, true), buffer});
    return buffer;
}

Graphics::DccKeys MetalShaderResources::textureKeys(const Graphics::GuestTextureResource& descriptor, std::size_t bytes) {
    if (descriptor.dccAddress == 0) return Graphics::DccKeys::Uncompressed;
    const auto count = Graphics::DccKeyBytes(bytes);
    if (count > std::numeric_limits<std::uint64_t>::max() - descriptor.dccAddress) {
        throw std::invalid_argument("Metal texture DCC metadata range overflows");
    }
    if (cpuRead) cpuRead(descriptor.dccAddress, count);
    for (const auto& image : images) {
        const auto begin = image.texture->Descriptor().baseAddress;
        if (overlaps(descriptor.dccAddress, descriptor.dccAddress + count, begin, begin + image.texture->GuestBytes()) ||
            physicallyOverlaps(descriptor.dccAddress, descriptor.dccAddress + count, begin, begin + image.texture->GuestBytes())) {
            throw std::invalid_argument("Metal texture DCC metadata aliases an active native image");
        }
    }
    for (const auto& write : bufferWrites) {
        if (overlaps(descriptor.dccAddress, descriptor.dccAddress + count, write.address, write.address + write.host.size()) ||
            physicallyOverlaps(descriptor.dccAddress, descriptor.dccAddress + count, write.address, write.address + write.host.size())) {
            throw std::invalid_argument("Metal texture DCC metadata aliases active guest buffer writes");
        }
    }
    auto keys = Graphics::DccKeys::Unreadable;
    if (count != 0 && GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.dccAddress), count)) {
        auto buffer = mirror(descriptor.dccAddress, count, false);
        keys = Graphics::ClassifyDccKeys({static_cast<const std::byte*>(buffer.buffer.contents) + buffer.offset, count});
    }
    if (keys == Graphics::DccKeys::Uncompressed) return keys;
    std::array<std::byte, 16> probe{};
    if (Graphics::IsDccClear(keys) && Graphics::FillDccClear(Graphics::ResolveTextureFormat(descriptor.format), keys,
            descriptor.dccAlphaOnMsb, std::span(probe).first(Graphics::BytesPerElement(descriptor.format)))) return keys;
    char message[320];
    std::snprintf(message, sizeof(message),
        "Metal texture DCC metadata is unresolved (%s keys, format %u, surface 0x%llx, metadata 0x%llx)",
        Graphics::DccKeysName(keys), descriptor.format, static_cast<unsigned long long>(descriptor.baseAddress),
        static_cast<unsigned long long>(descriptor.dccAddress));
    throw std::runtime_error(message);
}

// One surface bound twice at the same base, once by a descriptor that names its DCC metadata and
// once by one that names none: a mip-chain pass renders one level of a mipmapped color target (bound
// without DCC) while sampling another level through a texture descriptor that names the keys. While
// the metadata says "uncompressed" both views mean the same plain texels, so they share one backing;
// a written DCC view leaves the keys uncompressed (markDccUncompressed).
bool MetalShaderResources::sharesPlainTexels(const Image& image, const Graphics::GuestTextureResource& descriptor) {
    const auto& original = image.texture->Descriptor();
    if (original.dccAddress == descriptor.dccAddress) return false;
    if (original.dccAddress != 0 && descriptor.dccAddress != 0) return false;
    if (!image.texture->CanShareBackingIgnoringDcc(descriptor)) return false;
    if (original.dccAddress != 0) return image.keys == Graphics::DccKeys::Uncompressed;
    // The keys of a mipmapped color target are ignored (Graphics::State binds it without DCC), so
    // when this work writes such a surface its plain texels are what it holds, whatever stale keys a
    // texture descriptor of it names (a downsample pass samples one level while rendering the next).
    if (image.written && original.mipCount > 1) return true;
    try {
        return textureKeys(descriptor, image.texture->GuestBytes()) == Graphics::DccKeys::Uncompressed;
    } catch (const std::exception&) {
        return false;  // compressed or aliased metadata: the overlap is reported as before
    }
}

// For the overlap error: the keys of whichever view names DCC metadata the other does not.
std::string MetalShaderResources::dccKeysNote(const Image& image, const Graphics::GuestTextureResource& descriptor) {
    const auto& original = image.texture->Descriptor();
    if (original.dccAddress == descriptor.dccAddress) return {};
    if (original.dccAddress != 0) return std::string("bound DCC keys ") + Graphics::DccKeysName(image.keys) + "; ";
    try {
        return std::string("new DCC keys ") + Graphics::DccKeysName(textureKeys(descriptor, image.texture->GuestBytes())) + "; ";
    } catch (const std::exception& error) {
        return std::string("new DCC keys: ") + error.what() + "; ";
    }
}

void MetalShaderResources::validateDccWrite(const Graphics::GuestTextureResource& descriptor, std::size_t bytes, Graphics::DccKeys keys) {
    if (descriptor.dccAddress == 0) return;
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.baseAddress), bytes, true)) {
        throw NativeGuestMemory::ReadOnlyWriteError("Metal DCC image writes require a fully writable guest surface");
    }
    const auto count = storedKeyCount(descriptor, bytes);
    if (count == 0) return;
    if (keys == Graphics::DccKeys::Uncompressed && !GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.dccAddress), count, true)) return;
    static_cast<void>(mirror(descriptor.dccAddress, count, true));
}

std::shared_ptr<MetalTexture> MetalShaderResources::Texture(const Graphics::GuestTextureResource& descriptor,
    bool written, bool compare, bool atomic, std::optional<std::uint32_t> writtenLevel) {
    return texture(descriptor, written, compare, atomic, false, false, writtenLevel);
}

void MetalShaderResources::noteWrite(Image& image, const Graphics::GuestTextureResource& descriptor, bool storage,
    std::optional<std::uint32_t> level) {
    auto& part = image.writtenPart;
    const auto& geometry = image.texture->Geometry();
    // Storage views address one level; a render target, the level it attaches (else every level).
    const auto levelBegin = level.value_or(storage ? descriptor.baseLevel : 0u);
    const auto levelEnd = level ? *level + 1u : storage ? descriptor.baseLevel + 1u : static_cast<std::uint32_t>(geometry.mips.size());
    // Array views address their layers; volumes and thick layouts are copied whole.
    const bool layered = geometry.imageDepth <= 1 && !geometry.thick;
    const auto layerBegin = layered ? descriptor.baseArray : 0u;
    const auto layerEnd = layered ? std::min<std::uint32_t>(descriptor.depthOrLastArray + 1u, geometry.layers) : geometry.layers;
    part.levelBegin = std::min(part.levelBegin, levelBegin);
    part.levelEnd = std::max(part.levelEnd, levelEnd);
    part.layerBegin = std::min(part.layerBegin, layerBegin);
    part.layerEnd = std::max(part.layerEnd, layerEnd);
}

std::shared_ptr<MetalTexture> MetalShaderResources::texture(const Graphics::GuestTextureResource& descriptor,
    bool written, bool compare, bool atomic, bool minimumLodLowered, bool storage, std::optional<std::uint32_t> writtenLevel) {
    if (Graphics::EffectiveMinLod(descriptor) != 0 && !minimumLodLowered) {
        throw std::invalid_argument("Metal texture minimum LOD view clamp is not supported");
    }
    if (minimumLodLowered && (written || atomic)) {
        throw std::invalid_argument("Metal shader minimum LOD certificate cannot authorize native texture writes");
    }
    written = written || atomic;
    if (written && Graphics::IsBlockCompressed(descriptor.format)) {
        throw std::invalid_argument("Metal BC compressed textures cannot be written by shaders or render passes");
    }
    const auto format = Graphics::ResolveTextureFormat(descriptor.format);
    if (written && (format == VK_FORMAT_R5G6B5_UNORM_PACK16 || format == VK_FORMAT_A1R5G5B5_UNORM_PACK16 ||
        format == VK_FORMAT_R4G4B4A4_UNORM_PACK16 || format == VK_FORMAT_B4G4R4A4_UNORM_PACK16)) {
        throw std::invalid_argument("Metal packed 16-bit textures do not support native storage or render writes");
    }
    if (atomic && (format != VK_FORMAT_R32_UINT && format != VK_FORMAT_R32_SINT && format != VK_FORMAT_R32_SFLOAT)) {
        throw std::invalid_argument("Metal texture atomics require one 32-bit component");
    }
    for (auto& image : images) {
        const bool dccDiffers = !image.texture->CanShareBacking(descriptor) && sharesPlainTexels(image, descriptor);
        if (!dccDiffers && !image.texture->CanShareBacking(descriptor)) continue;
        if (Graphics::IsDccClear(image.keys)) {
            const auto& original = image.texture->Descriptor();
            const auto elementBytes = Graphics::BytesPerElement(original.format);
            std::array<std::byte, 16> capturedClear{}, requestedClear{};
            if (!Graphics::FillDccClear(Graphics::ResolveTextureFormat(original.format), image.keys,
                    original.dccAlphaOnMsb, std::span(capturedClear).first(elementBytes)) ||
                !Graphics::FillDccClear(format, image.keys, descriptor.dccAlphaOnMsb,
                    std::span(requestedClear).first(elementBytes)) ||
                !std::equal(capturedClear.begin(), capturedClear.begin() + elementBytes, requestedClear.begin())) {
                throw std::invalid_argument("Metal texture views require incompatible captured DCC clear encodings");
            }
        }
        if (!storage && !image.original.empty()) {
            static_cast<void>(NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, image.texture->GuestBytes(), written));
        }
        if (written) {
            validateCommittedImageWrite(borrowedRanges, descriptor.baseAddress, image.texture->GuestBytes());
            if (image.original.empty()) {
                static_cast<void>(NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, image.texture->GuestBytes(), true));
            }
            validateDccWrite(descriptor, image.texture->GuestBytes(), image.keys);
            image.written = true;
            noteWrite(image, descriptor, storage, writtenLevel);
            forgetCachedImages(descriptor.baseAddress, descriptor.baseAddress + image.texture->GuestBytes());
        }
        for (const auto& view : imageViews) {
            if (textureKey(view.texture->Descriptor(), view.compare) == textureKey(descriptor, compare)) return view.texture;
        }
        auto view = image.texture->CreateView(descriptor, compare, minimumLodLowered, dccDiffers);
        imageViews.push_back({view, compare});
        resident.push_back(view->Texture());
        return view;
    }
    const auto geometry = Graphics::DescribeSurface(descriptor);
    if (geometry.guestBytes > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("Metal texture guest surface exceeds native addressing");
    }
    auto bytes = static_cast<std::size_t>(geometry.guestBytes);
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress) {
        throw std::invalid_argument("Metal texture guest byte range is invalid");
    }
    if (written) validateCommittedImageWrite(borrowedRanges, descriptor.baseAddress, bytes);
    const bool staged = storage && !std::any_of(borrowedRanges.begin(), borrowedRanges.end(), [&](const auto& range) {
        return descriptor.baseAddress >= range.guestAddress && descriptor.baseAddress - range.guestAddress < range.host.size() &&
            bytes <= range.host.size() - (descriptor.baseAddress - range.guestAddress) && (!written || range.writable);
    });
    std::vector<ImageRange> writableRanges;
    if (staged && cpuRead) cpuRead(descriptor.baseAddress, bytes);
    auto buffer = staged ? stageStorageImage(descriptor.baseAddress, bytes, writableRanges) : mirror(descriptor.baseAddress, bytes, written);
    auto end = descriptor.baseAddress + bytes;
    const auto keyCount = Graphics::DccKeyCount(descriptor, bytes);
    if (descriptor.dccAddress != 0 && (keyCount > std::numeric_limits<std::uint64_t>::max() - descriptor.dccAddress ||
        overlaps(descriptor.baseAddress, end, descriptor.dccAddress, descriptor.dccAddress + keyCount) ||
        physicallyOverlaps(descriptor.baseAddress, end, descriptor.dccAddress, descriptor.dccAddress + keyCount))) {
        throw std::invalid_argument("Metal texture DCC metadata aliases its own native image or overflows");
    }
    for (const auto& [begin, bufferEnd] : bufferRanges) {
        if (overlaps(descriptor.baseAddress, end, begin, bufferEnd) ||
            physicallyOverlaps(descriptor.baseAddress, end, begin, bufferEnd)) {
            throw std::invalid_argument("Metal draw native image aliases an active guest buffer");
        }
    }
    for (const auto& image : images) {
        const auto& other = image.texture->Descriptor();
        const auto begin = other.baseAddress;
        if (other.dccAddress != 0 &&
            (overlaps(descriptor.baseAddress, end, other.dccAddress, other.dccAddress + Graphics::DccKeyCount(other, image.texture->GuestBytes())) ||
             physicallyOverlaps(descriptor.baseAddress, end, other.dccAddress, other.dccAddress + Graphics::DccKeyCount(other, image.texture->GuestBytes())))) {
            throw std::invalid_argument("Metal texture native image aliases active DCC metadata");
        }
        if (overlaps(descriptor.baseAddress, end, begin, begin + image.texture->GuestBytes()) ||
            physicallyOverlaps(descriptor.baseAddress, end, begin, begin + image.texture->GuestBytes())) {
            throw std::invalid_argument("Metal draw overlapping image views have incompatible captured backing (" + dccKeysNote(image, descriptor) +
                describeImage(descriptor) + (descriptor.baseAddress == begin ? " at the same base as " : " overlapping ") +
                describeImage(other) + "; " + image.texture->ShareMismatch(descriptor) + ")");
        }
    }
    auto host = staged ? std::span<std::byte>{} : NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, bytes, written);
    std::vector<std::byte> original;
    if (staged) {
        const auto* captured = static_cast<const std::byte*>(buffer.buffer.contents) + buffer.offset;
        original.assign(captured, captured + bytes);
    }
    const auto keys = textureKeys(descriptor, bytes);
    if (written) validateDccWrite(descriptor, bytes, keys);
    // A surface read or written in place is what the image cache can serve and hold: its texels are
    // the guest bytes when its DCC keys (read again at every binding) say uncompressed, or the clear
    // value they name when they say clear.
    std::uint64_t content = 0;
    if (Graphics::IsDccClear(keys)) {
        std::array<std::byte, 16> element{};
        const auto elementBytes = std::min<std::size_t>(Graphics::BytesPerElement(descriptor.format), element.size());
        if (Graphics::FillDccClear(format, keys, descriptor.dccAlphaOnMsb, std::span(element).first(elementBytes))) {
            content = 0xcbf29ce484222325ull ^ (static_cast<std::uint64_t>(keys) << 56u) ^ (static_cast<std::uint64_t>(format) << 40u);
            for (std::size_t i = 0; i < elementBytes; ++i) content = (content ^ static_cast<std::uint64_t>(element[i])) * 0x100000001b3ull;
            content |= 1u;
        }
    }
    const bool cacheable = imageCache != nullptr && !staged && host.size() == bytes &&
        (keys == Graphics::DccKeys::Uncompressed || content != 0);
    auto cached = cacheable ? imageCache->Find(descriptor.baseAddress, host.data(), descriptor, content) : std::nullopt;
    // A cached texture of the whole array serves a view of its first layers; this work then binds
    // (and, written, copies back) the whole surface, unless that would overlap another of its
    // images or buffers or leave the borrowed ranges.
    if (cached && cached->texture->GuestBytes() > bytes) {
        const auto whole = cached->texture->GuestBytes();
        bool fits = whole <= std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress;
        const auto wholeEnd = descriptor.baseAddress + whole;
        for (const auto& image : images) {
            const auto begin = image.texture->Descriptor().baseAddress;
            fits = fits && !overlaps(descriptor.baseAddress, wholeEnd, begin, begin + image.texture->GuestBytes()) &&
                !physicallyOverlaps(descriptor.baseAddress, wholeEnd, begin, begin + image.texture->GuestBytes());
        }
        for (const auto& [begin, bufferEnd] : bufferRanges) {
            fits = fits && !overlaps(descriptor.baseAddress, wholeEnd, begin, bufferEnd) && !physicallyOverlaps(descriptor.baseAddress, wholeEnd, begin, bufferEnd);
        }
        std::span<std::byte> wholeHost;
        if (fits) {
            try {
                if (written) validateCommittedImageWrite(borrowedRanges, descriptor.baseAddress, whole);
                wholeHost = NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, whole, written);
            } catch (const std::exception&) {
                fits = false;
            }
        }
        if (fits && wholeHost.data() == host.data()) {
            bytes = whole;
            end = wholeEnd;
            host = wholeHost;
            buffer = mirror(descriptor.baseAddress, bytes, written);
        } else {
            cached.reset();
        }
    }
    // This work rewrites the surface: no cached texture describes it until the copy-back completes.
    if (written) forgetCachedImages(descriptor.baseAddress, end);
    std::shared_ptr<MetalTexture> texture;
    bool textureCompare = compare;
    if (cached) {
        texture = cached->texture;
        textureCompare = cached->compare;
    } else {
        if (BindTrace::Active()) {
            BindTrace::Log("  image miss 0x%llx %s bytes %zu written %d cacheable %d staged %d keys %u content %llx",
                static_cast<unsigned long long>(descriptor.baseAddress), describeImage(descriptor).c_str(), bytes, written ? 1 : 0,
                cacheable ? 1 : 0, staged ? 1 : 0, static_cast<unsigned>(keys), static_cast<unsigned long long>(content));
        }
        texture = std::shared_ptr<MetalTexture>(new MetalTexture(backend, descriptor, compare, minimumLodLowered));
        if (keys == Graphics::DccKeys::Uncompressed) {
            if (cpuRead && !texture->UploadsInPlace(buffer.buffer, buffer.offset)) cpuRead(descriptor.baseAddress, bytes);
            texture->Upload(buffer.buffer, buffer.offset);
        } else {
            std::vector<std::byte> clear(bytes);
            if (!Graphics::FillDccClear(Graphics::ResolveTextureFormat(descriptor.format), keys, descriptor.dccAlphaOnMsb, clear)) {
                throw std::runtime_error("Metal texture DCC clear encoding changed after classification");
            }
            texture->Upload(clear);
        }
    }
    resident.push_back(texture->Texture());
    images.push_back({texture, buffer, host, written, keys, std::move(writableRanges), std::move(original)});
    images.back().compare = textureCompare;
    images.back().cacheable = cacheable;
    // Once this work writes the surface its texels are the guest bytes again.
    images.back().content = written ? 0 : content;
    if (written) noteWrite(images.back(), descriptor, storage, writtenLevel);
    imageViews.push_back({texture, textureCompare});
    if (textureKey(texture->Descriptor(), textureCompare) == textureKey(descriptor, compare)) return texture;
    // A cached texture made for another view of the surface backs this one as a view, as two
    // descriptors of one surface share a backing within a single draw or dispatch.
    auto view = texture->CreateView(descriptor, compare, minimumLodLowered);
    imageViews.push_back({view, compare});
    resident.push_back(view->Texture());
    return view;
}

MetalBufferBinding MetalShaderResources::upload(const void* data, std::size_t bytes) {
    constexpr std::size_t ChunkBytes = std::size_t{16} << 10u;
    // Bound at any offset with this alignment: no native buffer binding needs more.
    constexpr std::size_t Alignment = 256;
    if (bytes == 0) throw std::invalid_argument("Metal draw small buffer must not be empty");
    if (bytes > ChunkBytes / 2) {
        id<MTLBuffer> buffer = [backend.Device() newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
        if (buffer == nil) throw std::runtime_error("Metal draw captured shader data allocation failed");
        resident.push_back(buffer);
        return {buffer, 0, bytes};
    }
    std::size_t offset = (uploadUsed + Alignment - 1) & ~(Alignment - 1);
    if (uploadChunks.empty() || offset + bytes > ChunkBytes) {
        uploadChunks.push_back(MetalBufferPool::Shared().Take(backend.Device(), ChunkBytes));
        resident.push_back(uploadChunks.back());
        offset = 0;
    }
    uploadUsed = offset + bytes;
    std::memcpy(static_cast<std::byte*>(uploadChunks.back().contents) + offset, data, bytes);
    return {uploadChunks.back(), offset, bytes};
}

namespace {

// The sRGB pixel format a texture of `format` is sampled through under a sampler that forces sRGB
// decoding (the Metal counterparts of the Vulkan driver's SrgbTwinFormat), or Invalid.
MTLPixelFormat SrgbTwin(MTLPixelFormat format) {
    switch (format) {
        case MTLPixelFormatR8Unorm: return MTLPixelFormatR8Unorm_sRGB;
        case MTLPixelFormatRG8Unorm: return MTLPixelFormatRG8Unorm_sRGB;
        case MTLPixelFormatRGBA8Unorm: return MTLPixelFormatRGBA8Unorm_sRGB;
        case MTLPixelFormatBGRA8Unorm: return MTLPixelFormatBGRA8Unorm_sRGB;
        case MTLPixelFormatBC1_RGBA: return MTLPixelFormatBC1_RGBA_sRGB;
        case MTLPixelFormatBC2_RGBA: return MTLPixelFormatBC2_RGBA_sRGB;
        case MTLPixelFormatBC3_RGBA: return MTLPixelFormatBC3_RGBA_sRGB;
        case MTLPixelFormatBC7_RGBAUnorm: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        default: return MTLPixelFormatInvalid;
    }
}

// The same levels, slices and swizzle of `view` read through `format`. A view of a view takes its swizzle
// relative to the root texture, not composed with the parent's: passing `view.swizzle` keeps a non-identity
// T# channel mapping applied once (ImageSampleForceDegamma samples an R/B swap through this view).
id<MTLTexture> SrgbView(id<MTLTexture> view, MTLPixelFormat format) {
    const auto type = view.textureType;
    const NSUInteger slices = type == MTLTextureType3D ? 1u : type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray ? view.arrayLength * 6u : view.arrayLength;
    id<MTLTexture> result = [view newTextureViewWithPixelFormat:format textureType:type levels:NSMakeRange(0, view.mipmapLevelCount)
                                                         slices:NSMakeRange(0, slices) swizzle:view.swizzle];
    if (result == nil) throw std::runtime_error("Metal cannot create the sRGB view of a texture sampled through a sampler that forces sRGB decoding");
    return result;
}

}

std::vector<MetalShaderResourceBinding> MetalShaderResources::Bindings(const MetalBackend::Result& shader) {
    ValidatePixelSamplerBindings(shader.guest.bindings);
    const auto arrays = widestArrays(shader);
    // A sampler that forces sRGB decoding (S# FORCE_DEGAMMA) samples an sRGB image through its own view.
    // Any other image paired with it is sampled through an sRGB view of the same texture (SrgbTwin), as
    // the Vulkan driver's SrgbView does; a format without an sRGB twin is refused.
    const auto samplerBinding = std::find_if(shader.guest.bindings.begin(), shader.guest.bindings.end(),
        [](const auto& binding) { return binding.role == DescriptorRole::GuestSamplers; });
    const auto degammaFormat = [&](const DescriptorBinding& images, std::uint32_t element, std::uint32_t format) {
        if (images.kind != DescriptorKind::SampledImage || element >= images.imageSamplers.size()) return false;
        for (std::uint32_t sampler = 0; sampler < 32u; ++sampler) {
            if (((images.imageSamplers[element] >> sampler) & 1u) == 0u) continue;
            if (samplerBinding == shader.guest.bindings.end() || samplerBinding->guestDescriptor.size() < (sampler + 1u) * 4u) {
                throw std::invalid_argument("a sampled texture is paired with sampler element " + std::to_string(sampler) + ", which its shader does not bind");
            }
            const bool forced = ((samplerBinding->guestDescriptor[sampler * 4u] >> 20u) & 1u) != 0u;
            if (forced && !Graphics::IsSrgbTextureFormat(format)) return true;
        }
        return false;
    };
    std::vector<MetalShaderResourceBinding> result;
    for (const auto& mapping : shader.resources) {
        if (!mapping.active) continue;
        const auto found = std::find_if(shader.guest.bindings.begin(), shader.guest.bindings.end(), [&mapping](const auto& binding) {
            return mapping.descriptorSet == binding.descriptorSet && mapping.binding == binding.binding;
        });
        if (found == shader.guest.bindings.end()) throw std::invalid_argument("Metal draw active descriptor metadata is missing");
        MetalShaderResourceBinding native;
        native.descriptorSet = found->descriptorSet;
        native.binding = found->binding;
        switch (found->role) {
        case DescriptorRole::GuestBuffers:
            if (found->guestDescriptor.size() != static_cast<std::size_t>(found->count) * 4 || !mapping.buffer) {
                throw std::invalid_argument("Metal draw guest buffer descriptor metadata is invalid");
            }
            for (std::uint32_t i = 0; i < found->count; ++i) {
                ShaderBufferResource descriptor;
                std::copy_n(found->guestDescriptor.begin() + i * 4u, 4, descriptor.fields.begin());
                const bool written = writtenBuffer(*found, i);
                if (descriptor.GetSize() == 0 || descriptor.Base48() == 0) {
                    if (!emptyBuffer) {
                        const std::uint32_t zero = 0;
                        emptyBuffer = upload(&zero, sizeof(zero));
                        emptyBuffer->length = 0;
                    }
                    native.buffers.push_back(*emptyBuffer);
                } else {
                    if (BindTrace::Active()) {
                        std::array<std::uint32_t, 16> head{};
                        const auto bytes = std::min<std::size_t>(sizeof head, descriptor.GetSize());
                        std::uint32_t sum = 0;
                        if (GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.Base48()), bytes, false)) {
                            GuestMemory::Read(descriptor.Base48(), std::as_writable_bytes(std::span(head)).first(bytes));
                            for (const auto word : head) sum = sum * 31u + word;
                        }
                        BindTrace::Log("  buffer %u.%u[%u] 0x%llx size %u%s head %08x %08x %08x %08x hash %08x", mapping.descriptorSet,
                            mapping.binding, i, static_cast<unsigned long long>(descriptor.Base48()), static_cast<unsigned>(descriptor.GetSize()),
                            written ? " rw" : "", head[0], head[1], head[2], head[3], sum);
                    }
                    auto buffer = Buffer(descriptor.Base48(), descriptor.GetSize(), written);
                    if (buffer.offset % 4 != 0) throw std::invalid_argument("Metal draw storage buffer view must be DWORD aligned");
                    native.buffers.push_back(buffer);
                }
            }
            break;
        case DescriptorRole::ShaderData:
        case DescriptorRole::FlattenedSrt: {
            if (found->count != 1 || !mapping.buffer || found->guestDescriptor.empty()) {
                throw std::invalid_argument("Metal draw captured shader data binding is invalid");
            }
            if (found->role == DescriptorRole::ShaderData) validateRuntimeImages(shader.guest, found->guestDescriptor);
            const auto bytes = found->guestDescriptor.size() * sizeof(std::uint32_t);
            native.buffers.push_back(upload(found->guestDescriptor.data(), bytes));
            break;
        }
        case DescriptorRole::BdaPagetable:
        case DescriptorRole::FaultBuffer: {
            if (found->count != 1 || !mapping.buffer) throw std::invalid_argument("Metal draw BDA binding metadata is invalid");
            id<MTLBuffer> buffer = found->role == DescriptorRole::BdaPagetable ? snapshot.Table() : snapshot.FaultBuffer();
            native.buffers.push_back({buffer, 0, buffer.length});
            break;
        }
        case DescriptorRole::GuestImages: {
            if (!mapping.texture || found->guestDescriptor.size() != static_cast<std::size_t>(found->count) * 8) {
                throw std::invalid_argument("Metal draw image descriptor words are invalid");
            }
            if ((!found->imageWritten.empty() && found->imageWritten.size() != found->count) ||
                (!found->imageAtomic.empty() && found->imageAtomic.size() != found->count) ||
                (!found->imageAtomic64.empty() && found->imageAtomic64.size() != found->count) ||
                (!found->imageDepthCompare.empty() && found->imageDepthCompare.size() != found->count)) {
                throw std::invalid_argument("Metal draw image access metadata has the wrong descriptor count");
            }
            if (std::find(found->imageAtomic64.begin(), found->imageAtomic64.end(), true) != found->imageAtomic64.end()) {
                throw std::invalid_argument("Metal 64-bit image atomics are not implemented");
            }
            std::uint32_t storageMipOffset = 0;
            for (std::uint32_t i = 0; i < found->count; ++i) {
                if (NullImageDescriptor(std::span(found->guestDescriptor).subspan(i * 8u, 8))) {
                    // An empty element of a runtime ABI image heap stays unbound, as in the Vulkan driver.
                    native.textures.push_back(nil);
                    storageMipOffset = 0;
                    continue;
                }
                auto descriptor = Graphics::DecodeTextureResource(std::span(found->guestDescriptor).subspan(i * 8u, 8));
                const bool forcedSrgb = degammaFormat(*found, i, descriptor.format);
                for (const auto& array : arrays) {
                    if (sameArrayAllocation(array, descriptor)) descriptor.depthOrLastArray = array.depthOrLastArray;
                }
                const bool storage = found->kind == DescriptorKind::StorageImage || found->kind == DescriptorKind::StorageTexelBuffer;
                if (storage) {
                    const auto words = std::span(found->guestDescriptor).subspan(i * 8u, 8);
                    const bool sameAsPrevious = i != 0 && std::equal(words.begin(), words.end(), found->guestDescriptor.begin() + (i - 1u) * 8u);
                    storageMipOffset = sameAsPrevious ? storageMipOffset + 1u : 0u;
                    if (descriptor.mipCount == 0 || storageMipOffset > std::numeric_limits<std::uint32_t>::max() - descriptor.baseLevel) {
                        throw std::invalid_argument("Metal storage descriptor array mip addressing is invalid");
                    }
                    const auto mip = std::min(descriptor.baseLevel + storageMipOffset, descriptor.mipCount - 1u);
                    if (descriptor.minLod > mip * 256u) {
                        throw std::invalid_argument("guest storage texture descriptor clamps its minimum LOD above the level it addresses, which is not implemented");
                    }
                    descriptor.baseLevel = descriptor.lastLevel = mip;
                }
                const bool atomic = i < found->imageAtomic.size() && found->imageAtomic[i];
                const bool written = (i < found->imageWritten.size() && found->imageWritten[i]) || atomic;
                if (written && !storage) throw std::invalid_argument("Metal draw sampled image has writable metadata");
                const bool depth = i < found->imageDepthCompare.size() && found->imageDepthCompare[i];
                const bool pixel = !found->imageUnnormalized.empty() && found->imageUnnormalized[i];
                // A pixel certificate must not borrow normalized minimum-LOD lowering.
                if (pixel && (storage || depth || written || Graphics::EffectiveMinLod(descriptor) != 0)) {
                    throw std::invalid_argument("Metal pixel sampled image cannot use storage, depth, or normalized minimum LOD lowering");
                }
                const bool minimumLodLowered = !storage && Graphics::EffectiveMinLod(descriptor) != 0 &&
                    std::any_of(shader.minimumLodImages.begin(), shader.minimumLodImages.end(), [&](const auto& image) {
                        return image.descriptorSet == mapping.descriptorSet && image.binding == mapping.binding && image.element == i;
                    });
                if (!storage && Graphics::EffectiveMinLod(descriptor) != 0 && !minimumLodLowered) {
                    throw std::invalid_argument("Metal sampled minimum LOD view is missing complete shader lowering certification");
                }
                if (storage && descriptor.minLod > descriptor.baseLevel * 256u) {
                    throw std::invalid_argument("guest storage texture descriptor clamps its minimum LOD above the level it addresses, which is not implemented");
                }
                id<MTLTexture> view = depthLookup ? depthLookup(descriptor, storage, depth) : nil;
                if (BindTrace::Active()) traceImage(mapping.descriptorSet, mapping.binding, i, descriptor, storage, written, view != nil);
                if (view == nil && descriptor.samples > 1) {
                    // Only resident depth and color surfaces are multisampled; guest-memory MSAA images have no upload path.
                    throw std::invalid_argument("Metal multisampled guest image is not a resident multisampled depth or color surface (guest-memory MSAA images have no upload path; " +
                        describeImage(descriptor) + ")");
                }
                if (view == nil) {
                    auto texture = this->texture(descriptor, written, depth, atomic, minimumLodLowered, storage);
                    const bool rawSintStorage = storage && written && !atomic && mapping.unsignedStorageImage &&
                        Graphics::ResolveTextureFormat(descriptor.format) == VK_FORMAT_R32_SINT;
                    view = rawSintStorage ? texture->RawSintStorageView() :
                        storage ? texture->StorageView(atomic) : texture->SampledView();
                }
                if (forcedSrgb) {
                    const auto twin = SrgbTwin(view.pixelFormat);
                    if (twin == MTLPixelFormatInvalid) {
                        throw std::invalid_argument("a texture of guest format " + std::to_string(descriptor.format) +
                            ", which is not sRGB and has no sRGB view, is sampled through a sampler that forces sRGB decoding, which is not implemented");
                    }
                    view = SrgbView(view, twin);
                }
                if (pixel) {
                    if (view.device != backend.Device()) throw std::invalid_argument("Metal pixel sampled view belongs to a different device");
                    ValidatePixelSampledView(view, descriptor);
                }
                native.textures.push_back(view);
                if (pixel) resident.push_back(view);
            }
            break;
        }
        case DescriptorRole::GuestSamplers:
            if (!mapping.sampler || found->guestDescriptor.size() != static_cast<std::size_t>(found->count) * 4) {
                throw std::invalid_argument("Metal draw sampler descriptor words are invalid");
            }
            if (!found->samplerDepthCompare.empty() && found->samplerDepthCompare.size() != found->count) {
                throw std::invalid_argument("Metal draw sampler comparison metadata has the wrong descriptor count");
            }
            for (std::uint32_t i = 0; i < found->count; ++i) {
                const bool compare = i < found->samplerDepthCompare.size() && found->samplerDepthCompare[i];
                const bool pixel = !found->samplerUnnormalized.empty() && found->samplerUnnormalized[i];
                // FORCE_DEGAMMA is checked against the paired images' formats (degammaFormat) instead.
                auto sampler = std::make_shared<MetalSampler>(backend.Device(), std::span(found->guestDescriptor).subspan(i * 4u, 4), compare, pixel, true);
                native.samplers.push_back(sampler->Handle());
                samplers.push_back(std::move(sampler));
            }
            native.samplersMatchCapturedDescriptors = true;
            break;
        default:
            throw std::invalid_argument("Metal draw descriptor role requires an unimplemented native resource adapter");
        }
        result.push_back(std::move(native));
    }
    return result;
}

std::span<const id<MTLResource>> MetalShaderResources::Residency() const { return resident; }

void MetalShaderResources::EncodeReadbacks(id<MTLCommandBuffer> commands) {
    if (GpuTiming::SplitReadbacks() && std::any_of(images.begin(), images.end(), [](const Image& image) { return image.written; })) {
        // Profiling only: the copy-backs run right behind the work, in a command buffer of their own.
        [commands enqueue];
        id<MTLCommandBuffer> readbacks = backend.CommandBuffer();
        [readbacks enqueue];
        for (auto& image : images) {
            if (image.written && image.texture->EncodeReadback(readbacks, image.buffer.buffer, image.buffer.offset)) image.readbackIn = commands;
        }
        [readbacks addCompletedHandler:^(id<MTLCommandBuffer> done) {
            GpuTiming::Scope timing(GpuTiming::Kind::Readback);
            GpuTiming::Record(0, done.GPUEndTime - done.GPUStartTime, done.GPUStartTime, done.GPUEndTime);
        }];
        [readbacks commit];
        return;
    }
    for (auto& image : images) {
        if (!image.written) continue;
        // Only what this work wrote, when the rest of the guest bytes already equal the texture: an
        // uncompressed surface read and written in place (a DCC or staged one is copied whole).
        const bool partial = image.original.empty() && image.keys == Graphics::DccKeys::Uncompressed &&
            image.texture->Descriptor().dccAddress == 0 && image.writtenPart.levelBegin < image.writtenPart.levelEnd;
        if (image.texture->EncodeReadback(commands, image.buffer.buffer, image.buffer.offset, partial ? image.writtenPart : MetalTexture::Subresources{})) {
            image.readbackIn = commands;
        }
    }
}

bool MetalShaderResources::NeedsSynchronousCompletion() const {
    if (snapshot.CopiedBytes() != 0) return true;
    for (const auto& write : bufferWrites) {
        if (static_cast<const std::byte*>(write.buffer.buffer.contents) + write.buffer.offset != write.host.data()) return true;
    }
    for (const auto& image : images) {
        if (!image.written) continue;
        if (image.readbackIn == nil || !image.original.empty()) return true;
        if (static_cast<const std::byte*>(image.buffer.buffer.contents) + image.buffer.offset != image.host.data()) return true;
    }
    return false;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> MetalShaderResources::WrittenGuestRanges() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    for (const auto& write : bufferWrites) written.emplace_back(write.address, write.address + write.host.size());
    for (const auto& image : images) {
        if (!image.written) continue;
        const auto begin = image.texture->Descriptor().baseAddress;
        written.emplace_back(begin, begin + image.texture->GuestBytes());
    }
    return written;
}

// Written images leave their DCC metadata "uncompressed" (the copy-back stores plain texels).
void MetalShaderResources::markDccUncompressed() {
    for (const auto& image : images) {
        const auto& descriptor = image.texture->Descriptor();
        if (!image.written || descriptor.dccAddress == 0) continue;
        const auto count = storedKeyCount(descriptor, image.texture->GuestBytes());
        if (count == 0 || !GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.dccAddress), count, true)) continue;
        forgetCachedImages(descriptor.dccAddress, descriptor.dccAddress + count);
        auto buffer = mirror(descriptor.dccAddress, count, true);
        std::memset(static_cast<std::byte*>(buffer.buffer.contents) + buffer.offset, 0xff, count);
        std::vector<std::byte> uncompressed(count, std::byte{0xff});
        GuestMemory::Write(descriptor.dccAddress, uncompressed);
    }
}

void MetalShaderResources::CommittedWithoutWaiting() {
    if (imageCache != nullptr) {
        for (const auto& image : images) {
            if (image.cacheable) {
                imageCache->Insert(image.texture->Descriptor().baseAddress, image.host.data(), image.texture, image.compare,
                    image.written ? 0 : image.content, image.written);
            }
        }
    }
    // Later work reads the keys on the CPU when it binds the surface, after this point; the GPU
    // never reads them, so they may change before this work's copy-back runs.
    markDccUncompressed();
    committedWithoutWaiting = true;
    borrowed.reset();
}

Abi::Fault MetalShaderResources::Complete(id<MTLCommandBuffer> commands) {
    if (commands == nil || commands.status != MTLCommandBufferStatusCompleted || commands.device != backend.Device()) {
        throw std::invalid_argument("Metal draw resource copyback requires successful GPU completion");
    }
    // The GPU is done with this work's small buffers.
    for (id<MTLBuffer> chunk : uploadChunks) MetalBufferPool::Shared().Give(chunk);
    uploadChunks.clear();
    const auto* words = static_cast<const std::uint32_t*>(snapshot.FaultBuffer().contents);
    constexpr std::uint64_t pageBytes = std::uint64_t{1} << Abi::WrittenPageShift;
    std::vector<std::uint64_t> writtenPages;
    if (words[Abi::WrittenOverflowWord] != 0 || !Abi::ForEachWrittenRange(words, [&](std::uint64_t begin, std::size_t bytes) {
            for (std::uint64_t offset = 0; offset < bytes; offset += pageBytes) writtenPages.push_back(begin + offset);
        })) {
        if (imageCache != nullptr) imageCache->Clear();
        throw std::runtime_error("Metal draw BDA written-page tracker is incomplete or overflowed");
    }
    for (const auto page : writtenPages) forgetCachedImages(page, page + pageBytes);
    for (const auto page : writtenPages) {
        for (const auto& range : borrowedRanges) {
            if (!range.writable) continue;
            const auto writeBegin = std::max(page, range.guestAddress);
            const auto writeEnd = std::min(page + pageBytes, range.guestAddress + range.host.size());
            if (writeBegin >= writeEnd) continue;
            for (const auto& image : images) {
                const auto& descriptor = image.texture->Descriptor();
                const auto begin = descriptor.baseAddress;
                if (descriptor.dccAddress != 0 &&
                    (overlaps(writeBegin, writeEnd, descriptor.dccAddress, descriptor.dccAddress + Graphics::DccKeyCount(descriptor, image.texture->GuestBytes())) ||
                     physicallyOverlaps(writeBegin, writeEnd, descriptor.dccAddress, descriptor.dccAddress + Graphics::DccKeyCount(descriptor, image.texture->GuestBytes())))) {
                    throw std::runtime_error("Metal draw BDA writes alias active image DCC metadata");
                }
                if (overlaps(writeBegin, writeEnd, begin, begin + image.texture->GuestBytes()) ||
                    physicallyOverlaps(writeBegin, writeEnd, begin, begin + image.texture->GuestBytes())) {
                    throw std::runtime_error("Metal draw BDA writes alias an active image and require native image-buffer coherence");
                }
            }
        }
    }
    for (const auto& image : images) {
        image.texture->FinishUpload();
        if (image.written && image.readbackIn != commands) {
            image.texture->Readback(image.buffer.buffer, image.buffer.offset);
        }
    }
    const auto fault = snapshot.CompleteAndCopyDirtyPagesToBorrowedHost(commands);
    for (const auto& write : bufferWrites) {
        const auto* written = static_cast<const std::byte*>(write.buffer.buffer.contents) + write.buffer.offset;
        if (written != write.host.data()) std::memcpy(write.host.data(), written, write.host.size());
    }
    for (const auto& image : images) {
        if (!image.written) continue;
        const auto* current = static_cast<const std::byte*>(image.buffer.buffer.contents) + image.buffer.offset;
        if (image.original.empty()) {
            // An in-place copy-back already wrote the guest bytes through the alias.
            if (current != image.host.data()) std::memcpy(image.host.data(), current, image.texture->GuestBytes());
            continue;
        }
        for (const auto& range : image.writableRanges) {
            const auto* captured = image.original.data() + range.offset;
            const auto* updated = current + range.offset;
            auto* mirror = static_cast<std::byte*>(range.mirror.buffer.contents) + range.mirror.offset;
            for (std::size_t block = 0; block < range.host.size(); block += 256) {
                const auto limit = block + std::min<std::size_t>(256, range.host.size() - block);
                if (std::memcmp(updated + block, captured + block, limit - block) == 0) continue;
                for (std::size_t at = block; at < limit;) {
                    if (updated[at] == captured[at]) { ++at; continue; }
                    auto end = at + 1;
                    while (end < limit && updated[end] != captured[end]) ++end;
                    std::memcpy(range.host.data() + at, updated + at, end - at);
                    std::memcpy(mirror + at, updated + at, end - at);
                    at = end;
                }
            }
        }
    }
    if (!committedWithoutWaiting) markDccUncompressed();
    // Every image now matches its guest bytes: read ones were uploaded from them and written ones
    // were copied back over them. Work that faulted runs again and records them then.
    if (imageCache != nullptr && fault.state == Abi::FaultState::Empty && !committedWithoutWaiting) {
        for (const auto& image : images) {
            if (image.cacheable) {
                imageCache->Insert(image.texture->Descriptor().baseAddress, image.host.data(), image.texture, image.compare,
                    image.written ? 0 : image.content, image.written);
            }
        }
    }
    // Written surfaces' guest bytes now hold their copy-backs.
    if (imageCache != nullptr && fault.state == Abi::FaultState::Empty) {
        for (const auto& image : images) {
            if (image.cacheable && image.written) imageCache->Seal(image.host.data(), image.texture.get());
        }
    }
    if (BindTrace::Active()) {
        for (const auto& image : images) {
            if (!image.written) continue;
            const auto& descriptor = image.texture->Descriptor();
            BindTrace::Log("  wrote 0x%llx %s via %s: guest non-zero %d/64", static_cast<unsigned long long>(descriptor.baseAddress),
                describeImage(descriptor).c_str(), image.original.empty() ? "borrowed host" : "staged ranges", probeGuest(descriptor));
        }
    }
    return fault;
}

}
