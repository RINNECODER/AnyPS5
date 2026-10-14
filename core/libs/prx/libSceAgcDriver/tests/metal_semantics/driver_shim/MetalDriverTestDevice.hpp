// Replaces tests/execution/VulkanTestDevice.hpp for the tests that run through the Metal driver
// (its include guard is defined by the wrapper translation unit before the upstream test is included).
#ifndef ANYPS5_METAL_DRIVER_SEMANTICS_TESTDEVICE_HPP
#define ANYPS5_METAL_DRIVER_SEMANTICS_TESTDEVICE_HPP

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <memory>

constexpr int VulkanTestSkipped = 77;

inline bool TargetHasCapability(const ShaderRecompiler::SpirvTarget& target, spv::Capability capability) {
    if (capability == spv::CapabilityFloat64) return true;
    const auto& capabilities = target.supportedCapabilities;
    return std::find(capabilities.begin(), capabilities.end(), static_cast<std::uint32_t>(capability)) != capabilities.end();
}

inline std::unique_ptr<AgcDriver::VulkanDevice> OpenVulkanTestDevice() {
    return std::make_unique<AgcDriver::VulkanDevice>();
}

inline std::uint32_t PixelPushOffset(std::uint32_t vertexPush, const ShaderRecompiler::SpirvTarget& target) {
    return AgcDriver::Graphics::StagePushOffset(vertexPush, ShaderRecompiler::ShaderStage::Fragment, target.fixedPushSlots);
}

inline ShaderRecompiler::BindingLayout PixelPushLayout(std::uint32_t vertexPush, const ShaderRecompiler::SpirvTarget& target) {
    const auto offset = PixelPushOffset(vertexPush, target);
    return {0, 0, offset, AgcDriver::Graphics::PipelinePushSlotBytes - offset % AgcDriver::Graphics::PipelinePushSlotBytes};
}

#endif
