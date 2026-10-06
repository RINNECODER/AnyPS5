#include "MetalTestSupport.hpp"

namespace MetalTests {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Context::Context(id<MTLDevice> device, id<MTLLibrary> library)
    : device(device), library(library), backend(device, library) {
    queue = backend.Queue();
}

id<MTLBuffer> Context::Buffer(std::size_t bytes, std::uint8_t fill) const {
    auto buffer = backend.Buffer(bytes);
    std::memset(buffer.contents, fill, bytes);
    return buffer;
}

void Context::Dispatch(NSString* name, NSArray<id<MTLBuffer>>* buffers,
                       const void* parameters, std::size_t parameterBytes, MTLSize grid,
                       const std::array<std::uint32_t, 22>* constants) const {
    auto commands = backend.CommandBuffer();
    backend.Encode(commands, name, buffers, parameters, parameterBytes, grid, constants);
    backend.Wait(commands);
}

}
