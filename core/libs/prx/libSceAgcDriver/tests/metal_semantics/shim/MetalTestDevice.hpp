// Replaces tests/execution/VulkanTestDevice.hpp (its include guard is defined by the wrapper
// translation unit before the upstream test is included).
#ifndef ANYPS5_METAL_SEMANTICS_TESTDEVICE_HPP
#define ANYPS5_METAL_SEMANTICS_TESTDEVICE_HPP

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <memory>

constexpr int VulkanTestSkipped = 77;

// Metal has no 64-bit floats: the bridge lowers them without the Float64 capability, so the
// capability is reported as present and the tests exercise the lowering.
inline bool TargetHasCapability(const ShaderRecompiler::SpirvTarget& target, spv::Capability capability) {
    if (capability == spv::CapabilityFloat64) return true;
    const auto& capabilities = target.supportedCapabilities;
    return std::find(capabilities.begin(), capabilities.end(), static_cast<std::uint32_t>(capability)) != capabilities.end();
}

inline std::unique_ptr<AgcDriver::VulkanDevice> OpenVulkanTestDevice() {
    return std::make_unique<AgcDriver::VulkanDevice>();
}

#endif
