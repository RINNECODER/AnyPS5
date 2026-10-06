#include "MetalShaderResources.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
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
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> physical;
    for (const auto& range : ranges) {
        const auto begin = reinterpret_cast<std::uintptr_t>(range.host.data());
        if (range.host.size() > std::numeric_limits<std::uintptr_t>::max() - begin) {
            throw std::invalid_argument("Metal draw borrowed host address overflows");
        }
        physical.emplace_back(begin, begin + range.host.size());
        memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(range.guestAddress, range.host, range.writable);
    }
    std::sort(physical.begin(), physical.end());
    for (std::size_t i = 1; i < physical.size(); ++i) {
        if (physical[i].first < physical[i - 1].second) {
            throw std::invalid_argument("Metal draw cannot mirror physically aliased guest borrows independently");
        }
    }
    return memory.CaptureAfterPriorSnapshotsComplete();
}

auto textureKey(const Graphics::GuestTextureResource& resource) {
    return std::tuple(resource.baseAddress, resource.width, resource.height, resource.depthOrLastArray,
        resource.baseArray, resource.mipCount, resource.baseLevel, resource.lastLevel, resource.tileMode,
        resource.dimension, resource.format, resource.dstSelX, resource.dstSelY, resource.dstSelZ,
        resource.dstSelW, resource.dccAddress, resource.dccAlphaOnMsb, resource.minLod, resource.allocatedMipCount);
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
    : backend(backend), borrowed(ranges), memory(backend.Device()), snapshot(capture(memory, ranges)),
      depthLookup(std::move(depthLookup)) {
    resident.push_back(snapshot.Table());
    resident.push_back(snapshot.FaultBuffer());
    for (id<MTLBuffer> buffer in snapshot.Buffers()) resident.push_back(buffer);
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

MetalBufferBinding MetalShaderResources::Buffer(std::uint64_t address, std::size_t bytes, bool writable) {
    auto buffer = mirror(address, bytes, writable);
    for (const auto& image : images) {
        const auto begin = image.texture->Descriptor().baseAddress;
        if (overlaps(address, address + bytes, begin, begin + image.texture->GuestBytes())) {
            throw std::invalid_argument("Metal draw guest buffer aliases an active native image");
        }
    }
    bufferRanges.emplace_back(address, address + bytes);
    if (writable) bufferWrites.push_back({address, NativeGuestMemory::ContiguousBorrowedRange(address, bytes, true), buffer});
    return buffer;
}

std::shared_ptr<MetalTexture> MetalShaderResources::Texture(const Graphics::GuestTextureResource& descriptor, bool written) {
    for (auto& image : images) {
        if (textureKey(image.texture->Descriptor()) == textureKey(descriptor)) {
            if (written) {
                static_cast<void>(NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, image.texture->GuestBytes(), true));
                image.written = true;
            }
            return image.texture;
        }
    }
    auto texture = std::make_shared<MetalTexture>(backend, descriptor);
    const auto bytes = texture->GuestBytes();
    auto buffer = mirror(descriptor.baseAddress, bytes, written);
    const auto end = descriptor.baseAddress + bytes;
    for (const auto& [begin, bufferEnd] : bufferRanges) {
        if (overlaps(descriptor.baseAddress, end, begin, bufferEnd)) {
            throw std::invalid_argument("Metal draw native image aliases an active guest buffer");
        }
    }
    for (const auto& image : images) {
        const auto begin = image.texture->Descriptor().baseAddress;
        if (overlaps(descriptor.baseAddress, end, begin, begin + image.texture->GuestBytes())) {
            throw std::invalid_argument("Metal draw overlapping image views require shared native view resolution");
        }
    }
    auto host = NativeGuestMemory::ContiguousBorrowedRange(descriptor.baseAddress, bytes, written);
    texture->Upload({static_cast<const std::byte*>(buffer.buffer.contents) + buffer.offset, bytes});
    resident.push_back(texture->Texture());
    images.push_back({texture, buffer, host, written});
    return texture;
}

std::vector<MetalShaderResourceBinding> MetalShaderResources::Bindings(const MetalBackend::Result& shader) {
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
        case DescriptorRole::GuestImages:
            if (!mapping.texture || found->guestDescriptor.size() != static_cast<std::size_t>(found->count) * 8) {
                throw std::invalid_argument("Metal draw image descriptor words are invalid");
            }
            if ((!found->imageWritten.empty() && found->imageWritten.size() != found->count) ||
                (!found->imageAtomic.empty() && found->imageAtomic.size() != found->count) ||
                (!found->imageDepthCompare.empty() && found->imageDepthCompare.size() != found->count)) {
                throw std::invalid_argument("Metal draw image access metadata has the wrong descriptor count");
            }
            for (std::uint32_t i = 0; i < found->count; ++i) {
                const auto descriptor = Graphics::DecodeTextureResource(std::span(found->guestDescriptor).subspan(i * 8u, 8));
                const bool storage = found->kind == DescriptorKind::StorageImage || found->kind == DescriptorKind::StorageTexelBuffer;
                const bool written = (i < found->imageWritten.size() && found->imageWritten[i]) ||
                    (i < found->imageAtomic.size() && found->imageAtomic[i]);
                if (written && !storage) throw std::invalid_argument("Metal draw sampled image has writable metadata");
                const bool depth = i < found->imageDepthCompare.size() && found->imageDepthCompare[i];
                id<MTLTexture> view = depthLookup ? depthLookup(descriptor, storage, depth) : nil;
                if (view == nil) {
                    auto texture = Texture(descriptor, written);
                    view = storage ? texture->StorageView() : texture->SampledView();
                }
                native.textures.push_back(view);
            }
            break;
        case DescriptorRole::GuestSamplers:
            if (!mapping.sampler || found->guestDescriptor.size() != static_cast<std::size_t>(found->count) * 4) {
                throw std::invalid_argument("Metal draw sampler descriptor words are invalid");
            }
            if (!found->samplerDepthCompare.empty() && found->samplerDepthCompare.size() != found->count) {
                throw std::invalid_argument("Metal draw sampler comparison metadata has the wrong descriptor count");
            }
            for (std::uint32_t i = 0; i < found->count; ++i) {
                const bool compare = i < found->samplerDepthCompare.size() && found->samplerDepthCompare[i];
                auto sampler = std::make_shared<MetalSampler>(backend.Device(), std::span(found->guestDescriptor).subspan(i * 4u, 4), compare);
                native.samplers.push_back(sampler->Handle());
                samplers.push_back(std::move(sampler));
            }
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
        for (const auto& image : images) {
            const auto begin = image.texture->Descriptor().baseAddress;
            if (overlaps(page, page + pageBytes, begin, begin + image.texture->GuestBytes())) {
                throw std::runtime_error("Metal draw BDA writes alias an active image and require native image-buffer coherence");
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
        if (image.written) {
            std::memcpy(image.host.data(), static_cast<const std::byte*>(image.buffer.buffer.contents) + image.buffer.offset, image.texture->GuestBytes());
        }
    }
    return fault;
}

}
