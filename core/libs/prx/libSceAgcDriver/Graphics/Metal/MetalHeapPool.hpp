#pragma once

#import <Metal/Metal.h>
#include <cstddef>
#include <mutex>
#include <vector>

namespace AgcDriver::Metal {

class MetalHeapPool {
public:
    static MetalHeapPool& Shared();
    [[nodiscard]] static bool Enabled();

    explicit MetalHeapPool(std::size_t slabSize = 128 * 1024 * 1024);
    ~MetalHeapPool();

    MetalHeapPool(const MetalHeapPool&) = delete;
    MetalHeapPool& operator=(const MetalHeapPool&) = delete;

    [[nodiscard]] id<MTLTexture> SuballocateTexture(id<MTLDevice> device, MTLTextureDescriptor* descriptor);
    [[nodiscard]] id<MTLBuffer> SuballocateBuffer(id<MTLDevice> device, std::size_t bytes, MTLResourceOptions options);
    void CollectActiveHeaps(std::vector<id<MTLHeap>>& activeHeaps);
    void Clear();

private:
    id<MTLHeap> createSlab(id<MTLDevice> device, MTLStorageMode storageMode);

    std::size_t slabSize;
    std::mutex mutex;
    std::vector<id<MTLHeap>> sharedSlabs;
    std::vector<id<MTLHeap>> privateSlabs;
};

}
