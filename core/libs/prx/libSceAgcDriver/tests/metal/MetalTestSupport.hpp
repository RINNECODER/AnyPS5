#pragma once

#include "MetalDevice.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace MetalTests {

void Require(bool condition, const char* message);

class Context {
public:
    Context(id<MTLDevice> device, id<MTLLibrary> library);
    id<MTLBuffer> Buffer(std::size_t bytes, std::uint8_t fill = 0) const;
    void Dispatch(NSString* name, NSArray<id<MTLBuffer>>* buffers,
                  const void* parameters, std::size_t parameterBytes, MTLSize grid,
                  const std::array<std::uint32_t, 22>* constants = nullptr) const;

    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;

private:
    AgcDriver::Metal::MetalDevice backend;
};

}
