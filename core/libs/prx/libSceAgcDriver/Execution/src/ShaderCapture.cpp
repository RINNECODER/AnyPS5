#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "SceShaders.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AGC driver: ") + reason);
}

alignas(256) const std::uint32_t nullPixelCode[64] = {0xbf810000u};
const Shader nullPixelShader = [] {
    Shader shader{};
    shader.file_header = 0x34333231u;
    shader.version = 0x18u;
    shader.code = nullPixelCode;
    shader.header_size = sizeof(Shader);
    shader.shader_size = sizeof(nullPixelCode);
    shader.type = 1;
    return shader;
}();

}

std::shared_ptr<const ShaderSnapshot> ReadRegisteredShader(std::uint64_t guestHeaderAddress) {
    Shader header{};
    GuestMemory::Read(guestHeaderAddress, std::as_writable_bytes(std::span(&header, 1)), alignof(Shader));
    require(header.file_header == 0x34333231u && header.version == 0x18u, "invalid shader header");
    require(header.header_size >= sizeof(Shader), "shader header is smaller than its fixed fields");
    require(header.shader_size != 0 && (header.shader_size & 3u) == 0, "invalid shader size");
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(header.code);
    GuestMemory::CheckRange(reinterpret_cast<const void*>(guestHeaderAddress), header.header_size, alignof(Shader));
    GuestMemory::CheckRange(reinterpret_cast<const void*>(codeAddress), header.shader_size, 256);
    ShaderSnapshot snapshot{codeAddress, guestHeaderAddress, header.type, {}, {}};
    snapshot.code.resize(header.shader_size / sizeof(std::uint32_t));
    GuestMemory::Read(codeAddress, std::as_writable_bytes(std::span(snapshot.code)), 256);
    snapshot.header.resize(header.header_size);
    GuestMemory::Read(guestHeaderAddress, snapshot.header, alignof(Shader));
    return std::make_shared<const ShaderSnapshot>(std::move(snapshot));
}

std::shared_ptr<const ShaderSnapshot> CaptureNullPixelShader() {
    ShaderSnapshot snapshot{NullPixelProgramAddress(), reinterpret_cast<std::uintptr_t>(&nullPixelShader), nullPixelShader.type, {}, {}};
    snapshot.code.assign(std::begin(nullPixelCode), std::end(nullPixelCode));
    snapshot.header.resize(sizeof(Shader));
    std::memcpy(snapshot.header.data(), &nullPixelShader, sizeof(Shader));
    return std::make_shared<const ShaderSnapshot>(std::move(snapshot));
}

std::uint64_t NullPixelProgramAddress() {
    return reinterpret_cast<std::uintptr_t>(nullPixelCode);
}

}
