#include "MetalShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdio>
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
    std::span<const NativeGuestMemory::BorrowedRange> ranges) {
    for (const auto& range : ranges) {
        const auto begin = reinterpret_cast<std::uintptr_t>(range.host.data());
        if (range.host.size() > std::numeric_limits<std::uintptr_t>::max() - begin) {
            throw std::invalid_argument("Metal draw borrowed host address overflows");
        }
        memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(range.guestAddress, range.host, range.writable);
    }
    return memory.CaptureAfterPriorSnapshotsComplete();
}

auto textureKey(const Graphics::GuestTextureResource& resource, bool compare) {
    return std::tuple(resource.baseAddress, resource.width, resource.height, resource.depthOrLastArray,
        resource.baseArray, resource.mipCount, resource.baseLevel, resource.lastLevel, resource.tileMode,
        resource.dimension, resource.format, resource.dstSelX, resource.dstSelY, resource.dstSelZ,
        resource.dstSelW, resource.dccAddress, resource.dccAlphaOnMsb, resource.minLod, resource.allocatedMipCount, compare);
}

bool overlaps(std::uint64_t begin, std::uint64_t end, std::uint64_t otherBegin, std::uint64_t otherEnd) {
    return begin < otherEnd && otherBegin < end;
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
    std::span<const NativeGuestMemory::BorrowedRange> ranges, DepthTextureLookup depthLookup)
    : backend(backend), borrowed(ranges), borrowedRanges(ranges.begin(), ranges.end()),
      memory(backend.Device()), snapshot(capture(memory, ranges)),
      depthLookup(std::move(depthLookup)) {
    resident.push_back(snapshot.Table());
    resident.push_back(snapshot.FaultBuffer());
    for (id<MTLBuffer> buffer in snapshot.Buffers()) resident.push_back(buffer);
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
        throw std::invalid_argument("Metal storage texture has no writable committed guest intervals");
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
        const auto count = Graphics::DccKeyBytes(image.texture->GuestBytes());
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
    if (writable) bufferWrites.push_back({address, NativeGuestMemory::ContiguousBorrowedRange(address, bytes, true), buffer});
    return buffer;
}

Graphics::DccKeys MetalShaderResources::textureKeys(const Graphics::GuestTextureResource& descriptor, std::size_t bytes) {
    if (descriptor.dccAddress == 0) return Graphics::DccKeys::Uncompressed;
    const auto count = Graphics::DccKeyBytes(bytes);
    if (count > std::numeric_limits<std::uint64_t>::max() - descriptor.dccAddress) {
        throw std::invalid_argument("Metal texture DCC metadata range overflows");
    }
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

void MetalShaderResources::validateDccWrite(const Graphics::GuestTextureResource& descriptor, std::size_t bytes, Graphics::DccKeys keys) {
    if (descriptor.dccAddress == 0) return;
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.baseAddress), bytes, true)) {
        throw std::invalid_argument("Metal DCC image writes require a fully writable guest surface");
    }
    const auto count = Graphics::DccKeyBytes(bytes);
    if (count == 0) return;
    if (keys == Graphics::DccKeys::Uncompressed && !GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.dccAddress), count, true)) return;
    static_cast<void>(mirror(descriptor.dccAddress, count, true));
}

std::shared_ptr<MetalTexture> MetalShaderResources::Texture(const Graphics::GuestTextureResource& descriptor,
    bool written, bool compare, bool atomic) {
    return texture(descriptor, written, compare, atomic, false, false);
}

std::shared_ptr<MetalTexture> MetalShaderResources::texture(const Graphics::GuestTextureResource& descriptor,
    bool written, bool compare, bool atomic, bool minimumLodLowered, bool storage) {
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
        format == VK_FORMAT_R4G4B4A4_UNORM_PACK16)) {
        throw std::invalid_argument("Metal packed 16-bit textures do not support native storage or render writes");
    }
    if (atomic && (format != VK_FORMAT_R32_UINT && format != VK_FORMAT_R32_SINT && format != VK_FORMAT_R32_SFLOAT)) {
        throw std::invalid_argument("Metal texture atomics require one 32-bit component");
    }
    for (auto& image : images) {
        if (!image.texture->CanShareBacking(descriptor)) continue;
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
            if (image.original.empty()) {
                static_cast<void>(NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, image.texture->GuestBytes(), true));
            }
            validateDccWrite(descriptor, image.texture->GuestBytes(), image.keys);
            image.written = true;
        }
        for (const auto& view : imageViews) {
            if (textureKey(view.texture->Descriptor(), view.compare) == textureKey(descriptor, compare)) return view.texture;
        }
        auto view = image.texture->CreateView(descriptor, compare, minimumLodLowered);
        imageViews.push_back({view, compare});
        resident.push_back(view->Texture());
        return view;
    }
    const auto geometry = Graphics::DescribeSurface(descriptor);
    if (geometry.guestBytes > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("Metal texture guest surface exceeds native addressing");
    }
    const auto bytes = static_cast<std::size_t>(geometry.guestBytes);
    if (bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - descriptor.baseAddress) {
        throw std::invalid_argument("Metal texture guest byte range is invalid");
    }
    const bool staged = storage && !std::any_of(borrowedRanges.begin(), borrowedRanges.end(), [&](const auto& range) {
        return descriptor.baseAddress >= range.guestAddress && descriptor.baseAddress - range.guestAddress < range.host.size() &&
            bytes <= range.host.size() - (descriptor.baseAddress - range.guestAddress) && (!written || range.writable);
    });
    std::vector<ImageRange> writableRanges;
    auto buffer = staged ? stageStorageImage(descriptor.baseAddress, bytes, writableRanges) : mirror(descriptor.baseAddress, bytes, written);
    const auto end = descriptor.baseAddress + bytes;
    const auto keyCount = Graphics::DccKeyBytes(bytes);
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
            (overlaps(descriptor.baseAddress, end, other.dccAddress, other.dccAddress + Graphics::DccKeyBytes(image.texture->GuestBytes())) ||
             physicallyOverlaps(descriptor.baseAddress, end, other.dccAddress, other.dccAddress + Graphics::DccKeyBytes(image.texture->GuestBytes())))) {
            throw std::invalid_argument("Metal texture native image aliases active DCC metadata");
        }
        if (overlaps(descriptor.baseAddress, end, begin, begin + image.texture->GuestBytes()) ||
            physicallyOverlaps(descriptor.baseAddress, end, begin, begin + image.texture->GuestBytes())) {
            throw std::invalid_argument("Metal draw overlapping image views have incompatible captured backing");
        }
    }
    auto texture = std::shared_ptr<MetalTexture>(new MetalTexture(backend, descriptor, compare, minimumLodLowered));
    auto host = staged ? std::span<std::byte>{} : NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, bytes, written);
    std::vector<std::byte> original;
    if (staged) {
        const auto* captured = static_cast<const std::byte*>(buffer.buffer.contents) + buffer.offset;
        original.assign(captured, captured + bytes);
    }
    const auto keys = textureKeys(descriptor, bytes);
    if (written) validateDccWrite(descriptor, bytes, keys);
    if (keys == Graphics::DccKeys::Uncompressed) {
        texture->Upload({static_cast<const std::byte*>(buffer.buffer.contents) + buffer.offset, bytes});
    } else {
        std::vector<std::byte> clear(bytes);
        if (!Graphics::FillDccClear(Graphics::ResolveTextureFormat(descriptor.format), keys, descriptor.dccAlphaOnMsb, clear)) {
            throw std::runtime_error("Metal texture DCC clear encoding changed after classification");
        }
        texture->Upload(clear);
    }
    resident.push_back(texture->Texture());
    images.push_back({texture, buffer, host, written, keys, std::move(writableRanges), std::move(original)});
    imageViews.push_back({texture, compare});
    return texture;
}

std::vector<MetalShaderResourceBinding> MetalShaderResources::Bindings(const MetalBackend::Result& shader) {
    ValidatePixelSamplerBindings(shader.guest.bindings);
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
                    if (emptyBuffer == nil) {
                        const std::uint32_t zero = 0;
                        emptyBuffer = [backend.Device() newBufferWithBytes:&zero length:sizeof(zero) options:MTLResourceStorageModeShared];
                        if (emptyBuffer == nil) throw std::runtime_error("Metal draw empty descriptor allocation failed");
                    }
                    native.buffers.push_back({emptyBuffer, 0, 0});
                } else {
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
            const auto bytes = found->guestDescriptor.size() * sizeof(std::uint32_t);
            id<MTLBuffer> buffer = [backend.Device() newBufferWithBytes:found->guestDescriptor.data() length:bytes options:MTLResourceStorageModeShared];
            if (buffer == nil) throw std::runtime_error("Metal draw captured shader data allocation failed");
            native.buffers.push_back({buffer, 0, bytes});
            resident.push_back(buffer);
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
                (!found->imageDepthCompare.empty() && found->imageDepthCompare.size() != found->count)) {
                throw std::invalid_argument("Metal draw image access metadata has the wrong descriptor count");
            }
            std::uint32_t storageMipOffset = 0;
            for (std::uint32_t i = 0; i < found->count; ++i) {
                auto descriptor = Graphics::DecodeTextureResource(std::span(found->guestDescriptor).subspan(i * 8u, 8));
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
                if (view == nil) {
                    auto texture = this->texture(descriptor, written, depth, atomic, minimumLodLowered, storage);
                    const bool rawSintStorage = storage && written && !atomic && mapping.unsignedStorageImage &&
                        Graphics::ResolveTextureFormat(descriptor.format) == VK_FORMAT_R32_SINT;
                    view = rawSintStorage ? texture->RawSintStorageView() :
                        storage ? texture->StorageView(atomic) : texture->SampledView();
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
                auto sampler = std::make_shared<MetalSampler>(backend.Device(), std::span(found->guestDescriptor).subspan(i * 4u, 4), compare, pixel);
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

Abi::Fault MetalShaderResources::Complete(id<MTLCommandBuffer> commands) {
    if (commands == nil || commands.status != MTLCommandBufferStatusCompleted || commands.device != backend.Device()) {
        throw std::invalid_argument("Metal draw resource copyback requires successful GPU completion");
    }
    const auto* words = static_cast<const std::uint32_t*>(snapshot.FaultBuffer().contents);
    constexpr std::uint64_t pageBytes = std::uint64_t{1} << Abi::WrittenPageShift;
    for (std::uint32_t i = 0; i < Abi::WrittenPageSlots; ++i) {
        const auto encoded = words[Abi::WrittenSlotsWord + i];
        if (encoded == 0) continue;
        const auto page = std::uint64_t{encoded - 1u} << Abi::WrittenPageShift;
        for (const auto& range : borrowedRanges) {
            if (!range.writable) continue;
            const auto writeBegin = std::max(page, range.guestAddress);
            const auto writeEnd = std::min(page + pageBytes, range.guestAddress + range.host.size());
            if (writeBegin >= writeEnd) continue;
            for (const auto& image : images) {
                const auto& descriptor = image.texture->Descriptor();
                const auto begin = descriptor.baseAddress;
                if (descriptor.dccAddress != 0 &&
                    (overlaps(writeBegin, writeEnd, descriptor.dccAddress, descriptor.dccAddress + Graphics::DccKeyBytes(image.texture->GuestBytes())) ||
                     physicallyOverlaps(writeBegin, writeEnd, descriptor.dccAddress, descriptor.dccAddress + Graphics::DccKeyBytes(image.texture->GuestBytes())))) {
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
        if (image.written) {
            image.texture->Readback({static_cast<std::byte*>(image.buffer.buffer.contents) + image.buffer.offset, image.texture->GuestBytes()});
        }
    }
    const auto fault = snapshot.CompleteAndCopyDirtyPagesToBorrowedHost(commands);
    for (const auto& write : bufferWrites) {
        std::memcpy(write.host.data(), static_cast<const std::byte*>(write.buffer.buffer.contents) + write.buffer.offset, write.host.size());
    }
    for (const auto& image : images) {
        if (!image.written) continue;
        const auto* current = static_cast<const std::byte*>(image.buffer.buffer.contents) + image.buffer.offset;
        if (image.original.empty()) {
            std::memcpy(image.host.data(), current, image.texture->GuestBytes());
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
    for (const auto& image : images) {
        const auto& descriptor = image.texture->Descriptor();
        const auto count = Graphics::DccKeyBytes(image.texture->GuestBytes());
        if (!image.written || descriptor.dccAddress == 0 || count == 0 ||
            !GuestMemory::Accessible(reinterpret_cast<const void*>(descriptor.dccAddress), count, true)) continue;
        auto buffer = mirror(descriptor.dccAddress, count, true);
        std::memset(static_cast<std::byte*>(buffer.buffer.contents) + buffer.offset, 0xff, count);
        std::vector<std::byte> uncompressed(count, std::byte{0xff});
        GuestMemory::Write(descriptor.dccAddress, uncompressed);
    }
    return fault;
}

}
