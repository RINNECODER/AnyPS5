#include "MetalHeapPool.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Metal {

bool MetalHeapPool::Enabled() {
    const char* value = std::getenv("ANYPS5_METAL_TEXTURE_HEAP");
    if (value == nullptr) return false;
    if (*value == '1' && *(value + 1) == '\0') return true;
    if (std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0 ||
        std::strcmp(value, "on") == 0 || std::strcmp(value, "ON") == 0 ||
        std::strcmp(value, "yes") == 0 || std::strcmp(value, "YES") == 0) return true;
    return false;
}

MetalHeapPool& MetalHeapPool::Shared() {
    static MetalHeapPool pool;
    return pool;
}

MetalHeapPool::MetalHeapPool(std::size_t slabSize) : slabSize(slabSize) {}

MetalHeapPool::~MetalHeapPool() {
    Clear();
}

void MetalHeapPool::Clear() {
    std::lock_guard lock(mutex);
    sharedSlabs.clear();
    privateSlabs.clear();
}

id<MTLHeap> MetalHeapPool::createSlab(id<MTLDevice> device, MTLStorageMode storageMode) {
    auto desc = [MTLHeapDescriptor new];
    desc.size = slabSize;
    desc.storageMode = storageMode;
    desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
    desc.type = MTLHeapTypeAutomatic;

    id<MTLHeap> heap = [device newHeapWithDescriptor:desc];
    if (heap != nil) {
        if (storageMode == MTLStorageModeShared) {
            sharedSlabs.push_back(heap);
        } else {
            privateSlabs.push_back(heap);
        }
    }
    return heap;
}

id<MTLTexture> MetalHeapPool::SuballocateTexture(id<MTLDevice> device, MTLTextureDescriptor* descriptor) {
    if (device == nil || descriptor == nil) return nil;
    if (descriptor.storageMode != MTLStorageModeShared && descriptor.storageMode != MTLStorageModePrivate) return nil;

    std::lock_guard lock(mutex);
    auto& slabs = (descriptor.storageMode == MTLStorageModeShared) ? sharedSlabs : privateSlabs;

    const auto sizeAndAlign = [device heapTextureSizeAndAlignWithDescriptor:descriptor];
    if (sizeAndAlign.size > slabSize) return nil;

    for (auto it = slabs.rbegin(); it != slabs.rend(); ++it) {
        id<MTLTexture> tex = [*it newTextureWithDescriptor:descriptor];
        if (tex != nil) return tex;
    }

    id<MTLHeap> newHeap = createSlab(device, descriptor.storageMode);
    if (newHeap == nil) return nil;
    return [newHeap newTextureWithDescriptor:descriptor];
}

id<MTLBuffer> MetalHeapPool::SuballocateBuffer(id<MTLDevice> device, std::size_t bytes, MTLResourceOptions options) {
    if (device == nil || bytes == 0 || bytes > slabSize) return nil;
    const MTLStorageMode storageMode = (options & MTLResourceStorageModeShared) ? MTLStorageModeShared :
                                       (options & MTLResourceStorageModePrivate) ? MTLStorageModePrivate :
                                       MTLStorageModeShared;

    std::lock_guard lock(mutex);
    auto& slabs = (storageMode == MTLStorageModeShared) ? sharedSlabs : privateSlabs;

    for (auto it = slabs.rbegin(); it != slabs.rend(); ++it) {
        id<MTLBuffer> buf = [*it newBufferWithLength:bytes options:options];
        if (buf != nil) return buf;
    }

    id<MTLHeap> newHeap = createSlab(device, storageMode);
    if (newHeap == nil) return nil;
    return [newHeap newBufferWithLength:bytes options:options];
}

void MetalHeapPool::CollectActiveHeaps(std::vector<id<MTLHeap>>& activeHeaps) {
    std::lock_guard lock(mutex);
    activeHeaps.reserve(activeHeaps.size() + sharedSlabs.size() + privateSlabs.size());
    activeHeaps.insert(activeHeaps.end(), sharedSlabs.begin(), sharedSlabs.end());
    activeHeaps.insert(activeHeaps.end(), privateSlabs.begin(), privateSlabs.end());
}

}
