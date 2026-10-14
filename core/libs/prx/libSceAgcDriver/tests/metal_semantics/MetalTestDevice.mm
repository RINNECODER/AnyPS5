#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "MetalShaderPipeline.hpp"
#include "BdaAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

namespace AgcDriver {
using namespace ShaderRecompiler;
using namespace Metal;

struct VulkanDevice::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
};

VulkanDevice::VulkanDevice() : impl(std::make_unique<Impl>()) {
    impl->device = MTLCreateSystemDefaultDevice();
    if (impl->device == nil) throw std::runtime_error("no Metal device");
    impl->queue = [impl->device newCommandQueue];
}
VulkanDevice::~VulkanDevice() = default;

SpirvTarget VulkanDevice::Target() const {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto limit = impl->device.maxThreadsPerThreadgroup;
    return {0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(limit.height), static_cast<std::uint32_t>(limit.depth)},
        static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(impl->device.maxThreadgroupMemoryLength), {}, {}};
}

void VulkanDevice::Dispatch(const RecompileResult& guest, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                            std::span<const Graphics::GuestMemorySnapshot>, std::uint64_t) {
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true;
    options.supportsGpuAddresses = true;
    options.supportsSimdGroups = true;
    if (@available(macOS 15.0, *)) options.supportsWorkgroupAtomicFences = [impl->device supportsFamily:MTLGPUFamilyApple1];
    const auto shader = MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
    MetalComputePipeline pipeline(impl->device, shader);
    struct Mirror { void* host; std::size_t bytes; id<MTLBuffer> buffer; };
    std::vector<Mirror> mirrors;
    std::vector<MetalShaderResourceBinding> bindings;
    for (const auto& binding : shader.guest.bindings) {
        Graphics::Require(binding.role == DescriptorRole::GuestBuffers, "unexpected descriptor role in a semantics test");
        MetalShaderResourceBinding native{binding.descriptorSet, binding.binding};
        for (std::uint32_t i = 0; i < binding.count; ++i) {
            const auto* words = binding.guestDescriptor.data() + i * 4u;
            const auto address = static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1] & 0xffffu) << 32u);
            const auto stride = (words[1] >> 16u) & 0x3fffu;
            const std::size_t bytes = stride != 0u ? static_cast<std::size_t>(words[2]) * stride : words[2];
            void* host = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
            id<MTLBuffer> buffer = nil;
            for (const auto& mirror : mirrors) {
                if (mirror.host == host && mirror.bytes == bytes) buffer = mirror.buffer;
            }
            if (buffer == nil) {
                buffer = [impl->device newBufferWithBytes:host length:bytes options:MTLResourceStorageModeShared];
                Graphics::Require(buffer != nil, "semantics test buffer allocation failed");
                mirrors.push_back({host, bytes, buffer});
            }
            native.buffers.push_back({buffer, 0, bytes});
        }
        bindings.push_back(std::move(native));
    }
    const auto& threads = shader.threadsPerThreadgroup;
    auto commands = [impl->queue commandBuffer];
    pipeline.Encode(commands, bindings, MTLSizeMake(x * threads[0], y * threads[1], z * threads[2]));
    [commands commit];
    [commands waitUntilCompleted];
    Graphics::Require(commands.status == MTLCommandBufferStatusCompleted,
        std::string("Metal dispatch failed: ") + (commands.error.localizedDescription.UTF8String ?: "unknown"));
    for (const auto& mirror : mirrors) std::memcpy(mirror.host, mirror.buffer.contents, mirror.bytes);
}
}
