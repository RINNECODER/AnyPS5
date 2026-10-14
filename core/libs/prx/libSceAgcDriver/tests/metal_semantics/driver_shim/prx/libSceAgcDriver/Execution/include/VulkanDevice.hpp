// Metal stand-in for the Vulkan execution device that runs the upstream execution tests
// (core/libs/prx/libSceAgcDriver/tests/execution) through the native Metal driver: guest image,
// sampler and buffer descriptors are bound by MetalShaderResources and draws go through
// MetalDraw, as in a title. The tests' host arrays are the guest memory (guest address = host
// address); the device borrows every range the shader's descriptors and color target name.
#ifndef ANYPS5_METAL_DRIVER_SEMANTICS_VULKANDEVICE_HPP
#define ANYPS5_METAL_DRIVER_SEMANTICS_VULKANDEVICE_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver {
namespace Graphics {
struct GuestMemorySnapshot {
    std::uint64_t address;
    std::span<const std::byte> bytes;
};
}

class VulkanDevice {
public:
    VulkanDevice();
    ~VulkanDevice();
    [[nodiscard]] ShaderRecompiler::SpirvTarget Target() const;
    [[nodiscard]] ShaderRecompiler::SpirvTarget ComputeTarget(std::uint32_t) const { return Target(); }
    void Dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                  std::span<const Graphics::GuestMemorySnapshot> snapshots = {}, std::uint64_t programAddress = 0);
    void Draw(const Graphics::State& state, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders,
              std::span<const Graphics::GuestMemorySnapshot> snapshots = {});
    void WaitIdle() {}

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}

#endif
