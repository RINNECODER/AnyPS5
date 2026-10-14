#pragma once

#include "MetalDevice.hpp"
#include "MetalGuestMemory.hpp"
#include "MetalShaderPipeline.hpp"
#include "MetalTexture.hpp"
#include "GuestImageCache.hpp"
#include "MetalSampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <memory>
#include <functional>
#include <optional>

namespace AgcDriver::Metal {

class MetalShaderResources {
public:
    using DepthTextureLookup = std::function<id<MTLTexture>(const Graphics::GuestTextureResource&, bool, bool)>;
    using ImageCache = GuestImageCache<MetalTexture>;
    // `imageCache`, when given, supplies textures for surfaces an earlier draw or dispatch already
    // uploaded or read back (GuestImageCache.hpp) and learns this work's images once it completes.
    explicit MetalShaderResources(const MetalDevice& backend,
        std::span<const NativeGuestMemory::BorrowedRange> ranges, DepthTextureLookup depthLookup = {},
        MetalGuestHostAliases* aliases = nullptr, ImageCache* imageCache = nullptr);
    [[nodiscard]] std::vector<MetalShaderResourceBinding> Bindings(const ShaderRecompiler::MetalBackend::Result& shader);
    [[nodiscard]] MetalBufferBinding Buffer(std::uint64_t address, std::size_t bytes, bool writable = false);
    // `writtenLevel`: the one level a written binding renders into (a color attachment), so only
    // that level is copied back.
    [[nodiscard]] std::shared_ptr<MetalTexture> Texture(const Graphics::GuestTextureResource& descriptor, bool written = false, bool compare = false, bool atomic = false,
        std::optional<std::uint32_t> writtenLevel = std::nullopt);
    [[nodiscard]] std::span<const id<MTLResource>> Residency() const;
    // Encodes the copy-back of every written image into `commands` (after the work that writes
    // them, before the commit), so Complete(commands) need not read them back separately.
    void EncodeReadbacks(id<MTLCommandBuffer> commands);
    [[nodiscard]] ShaderRecompiler::BdaAbi::Fault Complete(id<MTLCommandBuffer> commands);
    // Asynchronous completion (MetalTimeline). True when Complete must copy bytes into guest
    // memory on the CPU (a staged or copied range, a copy-back that is not encoded, DCC metadata):
    // later work would read those bytes before a deferred Complete wrote them, so such work waits.
    [[nodiscard]] bool NeedsSynchronousCompletion() const;
    // Called once the work is committed without waiting: records the images it binds in the image
    // cache now (later work on the same queue runs after it, so it sees what this work leaves in
    // them), so Complete does not, and ends this object's borrowed-range scope so it can outlive
    // the caller's frame.
    void CommittedWithoutWaiting();
    // Guest ranges [begin, end) this work stores to: written images and writable buffers.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> WrittenGuestRanges() const;
    // Called before this object reads guest bytes on the CPU (an upload that cannot run in place,
    // a staged storage image), so the caller can first finish GPU work that may write them.
    using CpuReadHook = std::function<void(std::uint64_t address, std::uint64_t bytes)>;
    void SetCpuReadHook(CpuReadHook hook) { cpuRead = std::move(hook); }
    // Guest bytes the snapshot copied instead of aliasing (#291).
    [[nodiscard]] std::uint64_t CopiedBytes() const { return snapshot.CopiedBytes(); }

private:
    [[nodiscard]] std::shared_ptr<MetalTexture> texture(const Graphics::GuestTextureResource& descriptor,
        bool written, bool compare, bool atomic, bool minimumLodLowered, bool storage,
        std::optional<std::uint32_t> writtenLevel = std::nullopt);
    struct BufferWrite {
        std::uint64_t address;
        std::span<std::byte> host;
        MetalBufferBinding buffer;
    };
    struct ImageRange {
        std::size_t offset;
        std::span<std::byte> host;
        MetalBufferBinding mirror;
    };
    struct Image {
        std::shared_ptr<MetalTexture> texture;
        MetalBufferBinding buffer;
        std::span<std::byte> host;
        bool written;
        Graphics::DccKeys keys;
        std::vector<ImageRange> writableRanges;
        std::vector<std::byte> original;
        id<MTLCommandBuffer> readbackIn = nil;
        bool compare = false;
        // Uploaded from (or, written, read back into) its whole guest surface in place: what the
        // image cache may hold once the work completes.
        bool cacheable = false;
        // What the cached texels stand for (GuestImageCache::Find).
        std::uint64_t content = 0;
        // The levels and layers this work's bindings may write (copied back), empty when not written.
        MetalTexture::Subresources writtenPart{~0u, 0, ~0u, 0};
        // Written as a storage image (not only as a render target): its copy-back is never deferred.
        bool storageWrite = false;
    };
    // Widens `image.writtenPart` by what binding `descriptor` (as storage, or as a render target
    // writing `level`) may write.
    static void noteWrite(Image& image, const Graphics::GuestTextureResource& descriptor, bool storage, std::optional<std::uint32_t> level);
    struct ImageView {
        std::shared_ptr<MetalTexture> texture;
        bool compare;
    };
    const MetalDevice& backend;
    std::optional<NativeGuestMemory::BorrowedRangesScope> borrowed;
    bool committedWithoutWaiting = false;
    void markDccUncompressed();
    bool sharesPlainTexels(const Image& image, const Graphics::GuestTextureResource& descriptor);
    std::string dccKeysNote(const Image& image, const Graphics::GuestTextureResource& descriptor);
    CpuReadHook cpuRead;
    std::vector<NativeGuestMemory::BorrowedRange> borrowedRanges;
    MetalGuestMemory memory;
    MetalGuestMemory::DispatchSnapshot snapshot;
    std::vector<id<MTLResource>> resident;
    std::vector<BufferWrite> bufferWrites;
    std::vector<Image> images;
    std::vector<ImageView> imageViews;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bufferRanges;
    std::vector<std::shared_ptr<MetalSampler>> samplers;
    // Small per-draw buffers (captured shader data, the empty descriptor buffer) are carved from
    // recycled chunks instead of each being a new Metal buffer (a kernel allocation per draw); the
    // chunks return to MetalBufferPool once Complete sees the work finished.
    [[nodiscard]] MetalBufferBinding upload(const void* data, std::size_t bytes);
    std::vector<id<MTLBuffer>> uploadChunks;
    std::size_t uploadUsed = 0;
    std::optional<MetalBufferBinding> emptyBuffer;
    DepthTextureLookup depthLookup;
    MetalGuestHostAliases* aliases = nullptr;
    ImageCache* imageCache = nullptr;
    // Drops cached images whose guest bytes overlap [begin, end) through any borrowed range.
    void forgetCachedImages(std::uint64_t begin, std::uint64_t end);
    // Encodes the deferred copy-backs (MetalDeferredCopyBacks.hpp) over guest [address, +bytes),
    // matched at the bytes' canonical host addresses and at `alias`, the buffer this work binds
    // them through; `keep`'s own stays deferred.
    void encodeDeferredCopyBacks(std::uint64_t address, std::size_t bytes, const MetalBufferBinding& alias, const MetalTexture* keep = nullptr);
    [[nodiscard]] Graphics::DccKeys textureKeys(const Graphics::GuestTextureResource& descriptor, std::size_t bytes);
    void validateDccWrite(const Graphics::GuestTextureResource& descriptor, std::size_t bytes, Graphics::DccKeys keys);
    [[nodiscard]] bool physicallyOverlaps(std::uint64_t begin, std::uint64_t end,
        std::uint64_t otherBegin, std::uint64_t otherEnd) const;
    [[nodiscard]] MetalBufferBinding stageStorageImage(std::uint64_t address, std::size_t bytes,
        std::vector<ImageRange>& writableRanges);
    [[nodiscard]] MetalBufferBinding mirror(std::uint64_t address, std::size_t bytes, bool writable);
};

}
