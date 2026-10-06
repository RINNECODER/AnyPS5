#pragma once

#include "MetalDevice.hpp"
#include "MetalGuestMemory.hpp"
#include "MetalShaderPipeline.hpp"
#include "MetalTexture.hpp"
#include "MetalSampler.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <memory>
#include <functional>

namespace AgcDriver::Metal {

class MetalShaderResources {
public:
    using DepthTextureLookup = std::function<id<MTLTexture>(const Graphics::GuestTextureResource&, bool, bool)>;
    explicit MetalShaderResources(const MetalDevice& backend,
        std::span<const NativeGuestMemory::BorrowedRange> ranges, DepthTextureLookup depthLookup = {});
    [[nodiscard]] std::vector<MetalShaderResourceBinding> Bindings(const ShaderRecompiler::MetalBackend::Result& shader);
    [[nodiscard]] MetalBufferBinding Buffer(std::uint64_t address, std::size_t bytes, bool writable = false);
    [[nodiscard]] std::shared_ptr<MetalTexture> Texture(const Graphics::GuestTextureResource& descriptor, bool written = false);
    [[nodiscard]] std::span<const id<MTLResource>> Residency() const;
    [[nodiscard]] ShaderRecompiler::BdaAbi::Fault Complete(id<MTLCommandBuffer> commands);

private:
    struct BufferWrite {
        std::uint64_t address;
        std::span<std::byte> host;
        MetalBufferBinding buffer;
    };
    struct Image {
        std::shared_ptr<MetalTexture> texture;
        MetalBufferBinding buffer;
        std::span<std::byte> host;
        bool written;
    };
    const MetalDevice& backend;
    NativeGuestMemory::BorrowedRangesScope borrowed;
    MetalGuestMemory memory;
    MetalGuestMemory::DispatchSnapshot snapshot;
    std::vector<id<MTLResource>> resident;
    std::vector<BufferWrite> bufferWrites;
    std::vector<Image> images;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bufferRanges;
    std::vector<std::shared_ptr<MetalSampler>> samplers;
    id<MTLBuffer> emptyBuffer = nil;
    DepthTextureLookup depthLookup;
    [[nodiscard]] MetalBufferBinding mirror(std::uint64_t address, std::size_t bytes, bool writable);
};

}
