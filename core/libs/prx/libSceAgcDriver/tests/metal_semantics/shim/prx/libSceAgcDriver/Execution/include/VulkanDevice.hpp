// Metal stand-in for the Vulkan execution device so the upstream execution tests
// (core/libs/prx/libSceAgcDriver/tests/execution) compile unchanged and run the
// recompiler's SPIR-V through the Metal bridge on the GPU. Only the surface those
// tests use is provided. Guest buffer descriptors point at host arrays; Dispatch
// copies each into a shared MTLBuffer, runs the converted shader and copies back.
#ifndef ANYPS5_METAL_SEMANTICS_VULKANDEVICE_HPP
#define ANYPS5_METAL_SEMANTICS_VULKANDEVICE_HPP

#include "Recompiler.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace AgcDriver {
namespace Graphics {
struct GuestMemorySnapshot {};
inline void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
}

class VulkanDevice {
public:
    VulkanDevice();
    ~VulkanDevice();
    [[nodiscard]] ShaderRecompiler::SpirvTarget Target() const;
    [[nodiscard]] ShaderRecompiler::SpirvTarget ComputeTarget(std::uint32_t) const { return Target(); }
    void Dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                  std::span<const Graphics::GuestMemorySnapshot> snapshots = {}, std::uint64_t programAddress = 0);
    void WaitIdle() {}

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}

#endif
