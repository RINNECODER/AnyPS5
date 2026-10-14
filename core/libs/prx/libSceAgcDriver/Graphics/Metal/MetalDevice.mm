#include "MetalDevice.hpp"
#include "GpuStallWatch.hpp"
#include "GpuTiming.hpp"
#include "FrameProfile.hpp"

#include <stdexcept>
#include <utility>

namespace AgcDriver::Metal {

namespace {

std::string errorMessage(const char* operation, NSError* error) {
    const char* detail = error.localizedDescription.UTF8String;
    return std::string(operation) + ": " + (detail == nullptr ? "Metal returned no error description" : detail);
}

}

MetalDevice::MetalDevice(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> sharedQueue)
    : device(device), library(library) {
    if (device == nil || library == nil) {
        throw std::invalid_argument("MetalDevice requires a device and a library");
    }
    if (library.device != device) {
        throw std::invalid_argument("Metal library belongs to a different device");
    }
    if (sharedQueue != nil && sharedQueue.device != device) {
        throw std::invalid_argument("Metal command queue belongs to a different device");
    }
    queue = sharedQueue != nil ? sharedQueue : [device newCommandQueue];
    if (queue == nil) {
        throw std::runtime_error("Metal command queue creation failed");
    }
}

id<MTLBuffer> MetalDevice::Buffer(std::size_t bytes) const {
    if (bytes == 0 || bytes > device.maxBufferLength) {
        throw std::invalid_argument("Metal buffer length must be nonzero and within the device limit");
    }
    id<MTLBuffer> buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        throw std::runtime_error("Metal buffer allocation failed");
    }
    return buffer;
}

MetalBufferPool& MetalBufferPool::Shared() {
    static MetalBufferPool* const pool = new MetalBufferPool();
    return *pool;
}

id<MTLBuffer> MetalBufferPool::Take(id<MTLDevice> device, std::size_t bytes) {
    if (bytes == 0 || bytes > device.maxBufferLength) {
        throw std::invalid_argument("Metal buffer length must be nonzero and within the device limit");
    }
    {
        std::lock_guard lock(mutex);
        const auto found = free.find({reinterpret_cast<std::uintptr_t>((__bridge void*)device), bytes});
        if (found != free.end() && !found->second.empty()) {
            id<MTLBuffer> buffer = found->second.back();
            found->second.pop_back();
            freeBytes -= bytes;
            return buffer;
        }
    }
    id<MTLBuffer> buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buffer == nil || buffer.contents == nullptr) throw std::runtime_error("Metal buffer allocation failed");
    return buffer;
}

void MetalBufferPool::Give(id<MTLBuffer> buffer) {
    // Enough for a few frames of surface staging; beyond it buffers are simply released. Small
    // buffers (per-draw shader data chunks) are kept in larger numbers: one per draw in flight.
    constexpr std::size_t LimitBytes = std::size_t{768} << 20u;
    constexpr std::size_t LimitPerLength = 32;
    constexpr std::size_t SmallBytes = std::size_t{64} << 10u;
    constexpr std::size_t LimitPerSmallLength = 512;
    if (buffer == nil) return;
    std::lock_guard lock(mutex);
    if (freeBytes + buffer.length > LimitBytes) return;
    auto& list = free[{reinterpret_cast<std::uintptr_t>((__bridge void*)buffer.device), buffer.length}];
    if (list.size() >= (buffer.length <= SmallBytes ? LimitPerSmallLength : LimitPerLength)) return;
    list.push_back(buffer);
    freeBytes += buffer.length;
}

void MetalBufferPool::GiveWhenCompleted(id<MTLBuffer> buffer, id<MTLCommandBuffer> commands) {
    if (buffer == nil || commands == nil) return;
    [commands addCompletedHandler:^(id<MTLCommandBuffer>) { MetalBufferPool::Shared().Give(buffer); }];
}

id<MTLBuffer> MetalDevice::TransientBuffer(std::size_t bytes, id<MTLCommandBuffer> commands) const {
    validateCommands(commands);
    if (commands.status >= MTLCommandBufferStatusCommitted) throw std::invalid_argument("Metal transient buffer needs an uncommitted command buffer");
    id<MTLBuffer> buffer = MetalBufferPool::Shared().Take(device, bytes);
    MetalBufferPool::Shared().GiveWhenCompleted(buffer, commands);
    return buffer;
}

id<MTLCommandBuffer> MetalDevice::CommandBuffer() const {
    if (batching && open != nil && open.status == MTLCommandBufferStatusNotEnqueued) {
        ++openUses;
        return open;
    }
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    if (commands == nil) {
        throw MetalGpuExecutionError("Metal command buffer creation failed");
    }
    created.fetch_add(1, std::memory_order_relaxed);
    if (GpuTiming::ProfileTags()) {
        // With batching the tag is the first work's: the command buffer's GPU time is the batch's.
        const auto tag = GpuTiming::Tag() != 0 ? GpuTiming::Tag() : GpuTiming::MakeTag(GpuTiming::Current(), 0);
        [commands addCompletedHandler:^(id<MTLCommandBuffer> done) { GpuTiming::RecordTag(tag, done.GPUEndTime - done.GPUStartTime); }];
    }
    if (FrameProfile::Enabled()) {
        const auto kind = GpuTiming::Current();
        const auto tag = GpuTiming::Tag();
        [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
            FrameProfile::Shared().Completed(kind, tag, done.GPUStartTime, done.GPUEndTime);
        }];
    }
    if (batching) {
        // The previous batch, if any, was committed (or enqueued) by its last user.
        if (open != nil) flushed = open;
        open = commands;
        openUses = 1;
    }
    return commands;
}

MetalBatchedWork::~MetalBatchedWork() {
    if (done || !backend.OpenBatch(commands)) return;
    try {
        backend.Wait(commands);
    } catch (...) {
        // The work is already failing or skipped; the next wait on the queue reports a GPU error.
    }
}

void MetalDevice::SetBatching(bool on) {
    if (!on) Flush();
    batching = on;
}

bool MetalDevice::OpenBatch(id<MTLCommandBuffer> commands) const {
    return batching && commands != nil && commands == open && open.status < MTLCommandBufferStatusCommitted;
}

void MetalDevice::Flush() const {
    if (open == nil) return;
    if (open.status < MTLCommandBufferStatusCommitted) [open commit];
    flushed = open;
    open = nil;
    openUses = 0;
}

void MetalDevice::Submit(id<MTLCommandBuffer> commands) const {
    validateCommands(commands);
    if (OpenBatch(commands)) return;
    if (commands.status < MTLCommandBufferStatusCommitted) [commands commit];
}

bool MetalDevice::GpuStarved(std::uint32_t minimum) const {
    if (!batching || open == nil || openUses < minimum || open.status >= MTLCommandBufferStatusCommitted) return false;
    return flushed == nil || flushed.status >= MTLCommandBufferStatusCompleted;
}

void MetalDevice::validateCommands(id<MTLCommandBuffer> commands) const {
    if (commands == nil || commands.commandQueue != queue) {
        throw std::invalid_argument("Metal command buffer must belong to this command queue");
    }
}

id<MTLComputePipelineState> MetalDevice::pipeline(NSString* kernel,
                                               const std::array<std::uint32_t, 22>* constants) const {
    const char* name = kernel.UTF8String;
    if (name == nullptr || kernel.length == 0) {
        throw std::invalid_argument("Metal kernel name must be nonempty UTF-8");
    }
    PipelineKey key{std::string(name), constants == nullptr ? std::array<std::uint32_t, 22>{} : *constants,
                    constants != nullptr};
    if (key.kernel.size() != [kernel lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) {
        throw std::invalid_argument("Metal kernel name must not contain embedded null bytes");
    }
    std::lock_guard lock(pipelineMutex);
    if (auto found = pipelines.find(key); found != pipelines.end()) {
        return found->second;
    }
    NSError* error = nil;
    id<MTLFunction> function;
    if (constants == nullptr) {
        function = [library newFunctionWithName:kernel];
    } else {
        MTLFunctionConstantValues* values = [[MTLFunctionConstantValues alloc] init];
        for (std::size_t i = 0; i < constants->size(); ++i) {
            [values setConstantValue:&(*constants)[i] type:MTLDataTypeUInt atIndex:i];
        }
        function = [library newFunctionWithName:kernel constantValues:values error:&error];
    }
    if (function == nil) {
        throw std::runtime_error(errorMessage("Metal kernel resolution failed", error) + " (" + key.kernel + ")");
    }
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:function error:&error];
    if (state == nil) {
        throw std::runtime_error(errorMessage("Metal compute pipeline creation failed", error) + " (" + key.kernel + ")");
    }
    pipelines.emplace(std::move(key), state);
    return state;
}

void MetalDevice::Encode(id<MTLCommandBuffer> commands, NSString* kernel, NSArray<id<MTLBuffer>>* buffers,
                         const void* parameters, std::size_t parameterBytes, MTLSize grid,
                         const std::array<std::uint32_t, 22>* constants, std::span<const NSUInteger> offsets) const {
    validateCommands(commands);
    if (!offsets.empty() && (buffers == nil || offsets.size() != buffers.count)) {
        throw std::invalid_argument("Metal encoding buffer offsets must match the buffers");
    }
    if (commands.status >= MTLCommandBufferStatusCommitted) {
        throw std::invalid_argument("Metal command buffer has already been committed");
    }
    if (kernel == nil || kernel.length == 0 || buffers == nil) {
        throw std::invalid_argument("Metal encoding requires a kernel name and buffer array");
    }
    if (parameterBytes > 4096 || (parameterBytes != 0 && parameters == nullptr)) {
        throw std::invalid_argument("Metal inline parameters require a valid pointer and at most 4096 bytes");
    }
    if (buffers.count > (parameterBytes == 0 ? 31u : 30u)) {
        throw std::invalid_argument("Metal encoding exceeds the buffer argument limit");
    }
    for (id<MTLBuffer> buffer in buffers) {
        if (buffer == nil || ![buffer conformsToProtocol:@protocol(MTLBuffer)] || buffer.device != device) {
            throw std::invalid_argument("Metal buffers must belong to this device");
        }
    }
    if (grid.width == 0 || grid.height == 0 || grid.depth == 0) {
        return;
    }
    id<MTLComputePipelineState> state = pipeline(kernel, constants);
    const bool tileKernel = [kernel isEqualToString:@"TextureDetile"] || [kernel isEqualToString:@"ColorTransfer"];
    const MTLSize threads = tileKernel ? MTLSizeMake(8, 8, 1) : MTLSizeMake(1, 1, 1);
    if (threads.width > device.maxThreadsPerThreadgroup.width ||
        threads.height > device.maxThreadsPerThreadgroup.height ||
        threads.width * threads.height > state.maxTotalThreadsPerThreadgroup) {
        throw std::runtime_error("Metal compute pipeline cannot support the required threadgroup size");
    }
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (encoder == nil) {
        throw MetalGpuExecutionError("Metal compute command encoder creation failed");
    }
    // Named for GPU traces (Metal System Trace) when ANYPS5_METAL_GPU_TIMING is on.
    if (GpuTiming::Enabled()) {
        encoder.label = ![kernel isEqualToString:@"TextureDetile"] || constants == nullptr ? kernel :
            (*constants)[3] != 0 ? @"readback retile" : @"upload detile";
    }
    [encoder setComputePipelineState:state];
    for (NSUInteger i = 0; i < buffers.count; ++i) {
        [encoder setBuffer:buffers[i] offset:offsets.empty() ? 0 : offsets[i] atIndex:i];
    }
    if (parameterBytes != 0) {
        [encoder setBytes:parameters length:parameterBytes atIndex:buffers.count];
    }
    [encoder dispatchThreads:grid threadsPerThreadgroup:threads];
    [encoder endEncoding];
}

void MetalDevice::Wait(id<MTLCommandBuffer> commands) const {
    validateCommands(commands);
    if (OpenBatch(commands)) Flush();
    if (commands.status < MTLCommandBufferStatusCommitted) {
        [commands commit];
    }
    GpuStallWatch::Scope stallWatch;
    if (GpuTiming::Enabled()) {
        const auto start = std::chrono::steady_clock::now();
        const double hostStart = FrameProfile::Now();
        [commands waitUntilCompleted];
        FrameProfile::Shared().Waited(0x200u + static_cast<std::uint32_t>(GpuTiming::Current()), hostStart, FrameProfile::Now());
        GpuTiming::Record(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
            commands.GPUEndTime - commands.GPUStartTime, commands.GPUStartTime, commands.GPUEndTime);
    } else {
        [commands waitUntilCompleted];
    }
    if (commands.status == MTLCommandBufferStatusError) {
        throw MetalGpuExecutionError(errorMessage("Metal GPU execution failed", commands.error));
    }
    if (commands.status != MTLCommandBufferStatusCompleted) {
        throw MetalGpuExecutionError("Metal command buffer did not complete");
    }
}

id<MTLDevice> MetalDevice::Device() const {
    return device;
}

id<MTLCommandQueue> MetalDevice::Queue() const {
    return queue;
}

}
