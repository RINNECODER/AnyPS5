#pragma once

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace AgcDriver::DriverDetail {

// The by-value forms leave the snapshot mutable so a caller can attach registered state and
// prepared artifacts before publishing it.
ShaderSnapshot ReadRegisteredShaderSnapshot(std::uint64_t guestHeaderAddress);
std::shared_ptr<const ShaderSnapshot> ReadRegisteredShader(std::uint64_t guestHeaderAddress);
std::shared_ptr<const ShaderSnapshot> CaptureRawComputeShader(std::uint64_t address, std::size_t contiguousBytes);
ShaderSnapshot NullPixelShaderSnapshot();
std::shared_ptr<const ShaderSnapshot> CaptureNullPixelShader();
std::uint64_t NullPixelProgramAddress();

// SPI_SHADER_PGM_RSRC1 (COMPUTE_PGM_RSRC1 for compute) of the hardware stage a registered shader
// type runs as, and the bit that stage keeps FP16_OVFL in. nullopt for an unknown type.
struct FloatModeRegister {
    std::uint32_t offset;
    std::uint32_t fp16OverflowBit;
};
std::optional<FloatModeRegister> FloatModeRegisterFor(std::uint8_t type);
ShaderRecompiler::ShaderFloatMode DecodeFloatMode(const FloatModeRegister& reg, std::uint32_t rsrc1);
// The float mode (FLOAT_MODE, DX10_CLAMP, IEEE_MODE, FP16_OVFL) programmed by the SH registers of a
// registered AGC shader header: the same registers the Vulkan driver decodes at registration for
// RegisteredFloatMode, read straight from the captured header for backends that never build a
// RegisteredShaderState. nullopt when the header carries no RSRC1 (raw compute, the null pixel program).
std::optional<ShaderRecompiler::ShaderFloatMode> HeaderFloatMode(std::uint8_t type, std::uint64_t headerAddress,
    std::span<const std::byte> header);

}
