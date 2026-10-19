#pragma once

#include "MetalDevice.hpp"
#include "MetalDrawTranslationCache.hpp"
#include "MetalGuestMemory.hpp"
#include "MetalShaderPipeline.hpp"
#include "MetalTexture.hpp"
#include "MetalTimeline.hpp"
#include "GuestImageCache.hpp"
#include "MetalLazyCopyBacks.hpp"
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
    // them, before the commit), so Complete(commands) need not read them back separately. With
    // `lazy` (the work is committed to the timeline without waiting) and a registry
    // (SetLazyCopyBacks), the copy-backs that can be deferred are recorded there instead
    // (MetalLazyCopyBacks.hpp): the texture stays the newer copy until someone needs the bytes.
    void EncodeReadbacks(id<MTLCommandBuffer> commands, bool lazy = false);
    // The deferred copy-backs shared by draws and dispatches. Bindings that read guest bytes
    // through memory (buffers, uploads) materialize the overlapping ones first.
    void SetLazyCopyBacks(MetalLazyCopyBacks* registry) { lazyCopyBacks = registry; }
    // Materializes the lazy copy-backs over guest [address, address + bytes) for such a binding: the
    // driver encodes them and commits them to its timeline, which orders them before later signals
    // and CPU reads whatever happens to this object. Without one (tests), this object encodes them
    // itself and seals them in Complete.
    using MaterializeHook = std::function<void(std::uint64_t address, std::uint64_t bytes)>;
    void SetMaterializeHook(MaterializeHook hook) { materializeHook = std::move(hook); }
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
    // Guest ranges [begin, end) this work stores to: written images, writable buffers and the lazy
    // copy-backs it encoded ahead of itself.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> WrittenGuestRanges() const;
    // Whether the work's shaders may store to anything but the attachments of the render pass
    // `pass` describes: a writable buffer, or a written image that is not one of them (a storage
    // image; a draw's color targets are written images too). A later draw of the same pass could
    // read such a store unordered.
    [[nodiscard]] bool WritesBesides(const MetalEncoders::PassKey& pass) const;
    // Called before this object reads guest bytes on the CPU (an upload that cannot run in place,
    // a staged storage image), so the caller can first finish GPU work that may write them.
    using CpuReadHook = std::function<void(std::uint64_t address, std::uint64_t bytes)>;
    void SetCpuReadHook(CpuReadHook hook) { cpuRead = std::move(hook); }
    // Non-waiting views of the GPU work committed before this one: whether it may store to a guest
    // range (MetalTimeline::MayHaveWritten), and what a range holds once it completes when that is
    // known now (MetalTimeline::PendingBytes, given the bytes the CPU sees there). With them, DCC
    // keys a pending guest fill kernel stores are read without waiting for it.
    struct PendingStores {
        std::function<bool(std::uint64_t address, std::uint64_t bytes)> mayHaveWritten;
        std::function<bool(std::uint64_t address, std::span<const std::byte> visible, std::span<std::byte> out)> known;
    };
    void SetPendingStores(PendingStores stores) { pendingStores = std::move(stores); }
    [[nodiscard]] static PendingStores PendingStoresOf(const MetalTimeline& timeline) {
        return {[&timeline](std::uint64_t address, std::uint64_t bytes) {
                    const auto end = bytes > ~std::uint64_t{0} - address ? ~std::uint64_t{0} : address + bytes;
                    return timeline.MayHaveWritten(address, end);
                },
                [&timeline](std::uint64_t address, std::span<const std::byte> visible, std::span<std::byte> out) {
                    return timeline.PendingBytes(address, visible, out);
                }};
    }
    // The stores this work makes when it completes whose bytes are known now, for
    // MetalTimeline::Commit after CommittedWithoutWaiting: the DCC keys of written images it leaves
    // uncompressed while earlier pending work may still store to them.
    [[nodiscard]] std::vector<MetalTimeline::KnownStore> KnownStores() const;
    // The guest images this work read, for a draw translation record to re-prove before it reuses
    // its bindings (DrawImageDependency): `cached` are the ones the image cache holds an entry for,
    // `uncached` the rest (staged or partially borrowed surfaces), which have no proof to re-run and
    // so tie a record to the image cache generation it was built at.
    struct DrawImageSet {
        std::vector<DrawImageDependency> cached;
        std::uint32_t uncached = 0;
    };
    [[nodiscard]] DrawImageSet DrawImages() const;
    // The guest ranges this work had to encode lazy copy-backs for ahead of its own bindings
    // (materializeBefore): a reused draw translation record encodes them again.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> MaterializedRanges() const;
    // Encodes the lazy copy-backs over guest [address, address + bytes) ahead of this work, as a
    // binding about to read them through memory would.
    void MaterializeBefore(std::uint64_t address, std::uint64_t bytes) { materializeBefore(address, bytes); }
    // Guest bytes the snapshot copied instead of aliasing (#291).
    [[nodiscard]] std::uint64_t CopiedBytes() const { return snapshot.CopiedBytes(); }
    [[nodiscard]] const std::vector<id<MTLBuffer>>& UploadChunks() const { return uploadChunks; }

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
        // The copy-back was recorded in the lazy registry instead of encoded.
        bool deferred = false;
        bool compare = false;
        // Uploaded from (or, written, read back into) its whole guest surface in place: what the
        // image cache may hold once the work completes.
        bool cacheable = false;
        // What the cached texels stand for (GuestImageCache::Find).
        std::uint64_t content = 0;
        // The levels and layers this work's bindings may write (copied back), empty when not written.
        MetalTexture::Subresources writtenPart{~0u, 0, ~0u, 0};
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
    // `committing`: called from CommittedWithoutWaiting, where keys that pending work may still store
    // to are left to Complete (deferredKeyStores).
    void markDccUncompressed(bool committing);
    struct DeferredKeyStore {
        MetalTimeline::KnownStore known;
        MetalBufferBinding mirror;
        std::span<std::byte> host;
    };
    std::vector<DeferredKeyStore> deferredKeyStores;
    PendingStores pendingStores;
    bool sharesPlainTexels(const Image& image, const Graphics::GuestTextureResource& descriptor);
    std::string dccKeysNote(const Image& image, const Graphics::GuestTextureResource& descriptor);
    CpuReadHook cpuRead;
    MetalLazyCopyBacks* lazyCopyBacks = nullptr;
    MaterializeHook materializeHook;
    // Lazy copy-backs this work encoded ahead of its readers: sealed in the image cache once the
    // work completed.
    std::vector<MetalLazyCopyBacks::Materialized> materialized;
    // Encodes, ahead of this work, the lazy copy-backs over guest [address, address + bytes) that a
    // binding is about to read through memory (`keep`: a texture binding of that very texture).
    void materializeBefore(std::uint64_t address, std::uint64_t bytes, const MetalTexture* keep = nullptr);
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
    [[nodiscard]] Graphics::DccKeys textureKeys(const Graphics::GuestTextureResource& descriptor, std::size_t bytes);
    void validateDccWrite(const Graphics::GuestTextureResource& descriptor, std::size_t bytes, Graphics::DccKeys keys);
    [[nodiscard]] bool physicallyOverlaps(std::uint64_t begin, std::uint64_t end,
        std::uint64_t otherBegin, std::uint64_t otherEnd) const;
    [[nodiscard]] MetalBufferBinding stageStorageImage(std::uint64_t address, std::size_t bytes,
        std::vector<ImageRange>& writableRanges);
    [[nodiscard]] MetalBufferBinding mirror(std::uint64_t address, std::size_t bytes, bool writable);
};

}
