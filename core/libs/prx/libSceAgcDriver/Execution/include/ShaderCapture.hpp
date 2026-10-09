#pragma once

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"

namespace AgcDriver::DriverDetail {

// The by-value forms leave the snapshot mutable so a caller can attach registered state and
// prepared artifacts before publishing it.
ShaderSnapshot ReadRegisteredShaderSnapshot(std::uint64_t guestHeaderAddress);
std::shared_ptr<const ShaderSnapshot> ReadRegisteredShader(std::uint64_t guestHeaderAddress);
std::shared_ptr<const ShaderSnapshot> CaptureRawComputeShader(std::uint64_t address, std::size_t contiguousBytes);
ShaderSnapshot NullPixelShaderSnapshot();
std::shared_ptr<const ShaderSnapshot> CaptureNullPixelShader();
std::uint64_t NullPixelProgramAddress();

}
