#pragma once

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"

namespace AgcDriver::DriverDetail {

std::shared_ptr<const ShaderSnapshot> ReadRegisteredShader(std::uint64_t guestHeaderAddress);
std::shared_ptr<const ShaderSnapshot> CaptureRawComputeShader(std::uint64_t address, std::size_t contiguousBytes);
std::shared_ptr<const ShaderSnapshot> CaptureNullPixelShader();
std::uint64_t NullPixelProgramAddress();

}
