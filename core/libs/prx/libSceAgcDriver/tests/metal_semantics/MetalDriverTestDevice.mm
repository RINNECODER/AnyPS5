#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalDraw.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalSampler.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderPipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "BdaAbi.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AgcDriver {
using namespace ShaderRecompiler;
using namespace Metal;

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using Span = std::pair<std::uint64_t, std::uint64_t>;

void AddSpan(std::vector<Span>& spans, std::uint64_t address, std::uint64_t bytes) {
    if (address == 0 || bytes == 0) return;
    Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "driver semantics test range overflows");
    spans.emplace_back(address, address + bytes);
}

// Every guest range a shader's descriptors name: buffers by their V# size, images by the
// surface their T# describes.
void AddShaderSpans(std::vector<Span>& spans, const RecompileResult& shader) {
    for (const auto& binding : shader.bindings) {
        if (binding.role == DescriptorRole::GuestBuffers) {
            for (std::uint32_t i = 0; i + 1u <= binding.guestDescriptor.size() / 4u; ++i) {
                ShaderBufferResource descriptor;
                std::copy_n(binding.guestDescriptor.begin() + i * 4u, 4, descriptor.fields.begin());
                AddSpan(spans, descriptor.Base48(), descriptor.GetSize());
            }
        } else if (binding.role == DescriptorRole::GuestImages) {
            for (std::uint32_t i = 0; i + 1u <= binding.guestDescriptor.size() / 8u; ++i) {
                const auto words = std::span(binding.guestDescriptor).subspan(i * 8u, 8);
                if (NullImageDescriptor(words)) continue;
                const auto resource = Graphics::DecodeTextureResource(words);
                AddSpan(spans, resource.baseAddress, Graphics::DescribeSurface(resource).guestBytes);
            }
        }
    }
}

// The tests' arrays are host memory at their own addresses; overlapping spans become one range.
std::vector<NativeGuestMemory::BorrowedRange> Borrow(std::vector<Span> spans) {
    std::sort(spans.begin(), spans.end());
    std::vector<Span> merged;
    for (const auto& span : spans) {
        if (!merged.empty() && span.first <= merged.back().second) merged.back().second = std::max(merged.back().second, span.second);
        else merged.push_back(span);
    }
    std::vector<NativeGuestMemory::BorrowedRange> ranges;
    for (const auto& [begin, end] : merged) {
        ranges.push_back({begin, std::span(reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(begin)), static_cast<std::size_t>(end - begin)), true});
    }
    return ranges;
}

void RequireNoFault(const BdaAbi::Fault& fault, const char* what) {
    Require(fault.state == BdaAbi::FaultState::Empty, std::string(what) + " faulted on guest memory at 0x" +
        [&] { char text[32]; std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(fault.address)); return std::string(text); }());
}

}

struct VulkanDevice::Impl {
    id<MTLDevice> device = nil;
    std::unique_ptr<MetalDevice> backend;
    std::unique_ptr<MetalDraw> draw;
};

VulkanDevice::VulkanDevice() : impl(std::make_unique<Impl>()) {
    impl->device = MTLCreateSystemDefaultDevice();
    Require(impl->device != nil, "no Metal device");
    const char* path = std::getenv("ANYPS5_METAL_UTILITY_LIBRARY");
    Require(path != nullptr, "ANYPS5_METAL_UTILITY_LIBRARY names no utility metallib");
    NSError* error = nil;
    id<MTLLibrary> library = [impl->device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]] error:&error];
    Require(library != nil, error != nil ? error.localizedDescription.UTF8String : "cannot load the utility metallib");
    impl->backend = std::make_unique<MetalDevice>(impl->device, library);
    impl->draw = std::make_unique<MetalDraw>(*impl->backend);
}

VulkanDevice::~VulkanDevice() = default;

SpirvTarget VulkanDevice::Target() const {
    static constexpr std::array<std::uint32_t, 4> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityFragmentBarycentricKHR};
    static constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto limit = impl->device.maxThreadsPerThreadgroup;
    const auto supported = std::span(capabilities).first(impl->device.supportsShaderBarycentricCoordinates ? 4u : 3u);
    return {0x00401000, 0x00010300, 32, BdaAbi::Version, supported, extensions, false,
        {static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(limit.height), static_cast<std::uint32_t>(limit.depth)},
        static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(impl->device.maxThreadgroupMemoryLength), {}, {}};
}

void VulkanDevice::Dispatch(const RecompileResult& guest, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                            std::span<const Graphics::GuestMemorySnapshot>, std::uint64_t) {
    @autoreleasepool {
        MetalBackend::TargetOptions options;
        options.supportsInt64 = true;
        options.supportsGpuAddresses = true;
        options.supportsSimdGroups = true;
        if (@available(macOS 15.0, *)) options.supportsWorkgroupAtomicFences = [impl->device supportsFamily:MTLGPUFamilyApple1];
        options.supportsArgumentBuffersTier2 = impl->device.argumentBuffersSupport == MTLArgumentBuffersTier2;
        const MetalComputePipeline pipeline(impl->device, MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options));
        std::vector<Span> spans;
        AddShaderSpans(spans, guest);
        const auto ranges = Borrow(std::move(spans));
        MetalShaderResources resources(*impl->backend, ranges);
        const auto bindings = resources.Bindings(pipeline.Reflection());
        const auto& threads = pipeline.Reflection().threadsPerThreadgroup;
        id<MTLCommandBuffer> commands = impl->backend->CommandBuffer();
        pipeline.Encode(commands, bindings, MTLSizeMake(x * threads[0], y * threads[1], z * threads[2]), {}, resources.Residency());
        resources.EncodeReadbacks(commands);
        [commands commit];
        impl->backend->Wait(commands);
        Require(commands.status == MTLCommandBufferStatusCompleted,
            std::string("Metal dispatch failed: ") + (commands.error.localizedDescription.UTF8String ?: "unknown"));
        RequireNoFault(resources.Complete(commands), "Metal dispatch");
    }
}

void VulkanDevice::Draw(const Graphics::State& state, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders,
                        std::span<const Graphics::GuestMemorySnapshot>) {
    @autoreleasepool {
        auto native = state;
        // The Vulkan driver takes the first color target from `color`; MetalDraw reads `colors`.
        if (native.hasColorTarget && native.colors.empty()) native.colors = {native.color};
        if (native.blends.empty()) native.blends.resize(native.colors.size(), native.blend);
        std::vector<Span> spans;
        for (const auto& shader : shaders) AddShaderSpans(spans, *shader.program);
        for (const auto& color : native.colors) AddSpan(spans, color.address, color.bytes);
        const auto ranges = Borrow(std::move(spans));
        RequireNoFault(impl->draw->DrawSynchronously(native, draw, shaders, ranges), "Metal draw");
    }
}
}
