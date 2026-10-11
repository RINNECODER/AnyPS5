#include "MetalDevice.hpp"

#include <stdexcept>
#include <utility>

namespace AgcDriver::Metal {

namespace {

std::string errorMessage(const char* operation, NSError* error) {
    const char* detail = error.localizedDescription.UTF8String;
    return std::string(operation) + ": " + (detail == nullptr ? "Metal returned no error description" : detail);
}

}

MetalDevice::MetalDevice(id<MTLDevice> device, id<MTLLibrary> library)
    : device(device), library(library) {
    if (device == nil || library == nil) {
        throw std::invalid_argument("MetalDevice requires a device and a library");
    }
    if (library.device != device) {
        throw std::invalid_argument("Metal library belongs to a different device");
    }
    queue = [device newCommandQueue];
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

id<MTLCommandBuffer> MetalDevice::CommandBuffer() const {
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    if (commands == nil) {
        throw MetalGpuExecutionError("Metal command buffer creation failed");
    }
    return commands;
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
                         const std::array<std::uint32_t, 22>* constants) const {
    validateCommands(commands);
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
    [encoder setComputePipelineState:state];
    for (NSUInteger i = 0; i < buffers.count; ++i) {
        [encoder setBuffer:buffers[i] offset:0 atIndex:i];
    }
    if (parameterBytes != 0) {
        [encoder setBytes:parameters length:parameterBytes atIndex:buffers.count];
    }
    [encoder dispatchThreads:grid threadsPerThreadgroup:threads];
    [encoder endEncoding];
}

void MetalDevice::Wait(id<MTLCommandBuffer> commands) const {
    validateCommands(commands);
    if (commands.status < MTLCommandBufferStatusCommitted) {
        [commands commit];
    }
    [commands waitUntilCompleted];
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
