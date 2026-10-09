#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "SceShaders.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
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

ShaderSnapshot ReadRegisteredShaderSnapshot(std::uint64_t guestHeaderAddress) {
    Shader header{};
    GuestMemory::Read(guestHeaderAddress, std::as_writable_bytes(std::span(&header, 1)), 1);
    require(header.file_header == 0x34333231u && header.version == 0x18u, "invalid shader header");
    require(header.header_size >= sizeof(Shader), "shader header is smaller than its fixed fields");
    require(header.shader_size != 0 && (header.shader_size & 3u) == 0, "invalid shader size");
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(header.code);
    GuestMemory::CheckRange(reinterpret_cast<const void*>(guestHeaderAddress), header.header_size, 1);
    GuestMemory::CheckRange(reinterpret_cast<const void*>(codeAddress), header.shader_size, 256);
    ShaderSnapshot snapshot{codeAddress, guestHeaderAddress, header.type, {}, {}};
    snapshot.code.resize(header.shader_size / sizeof(std::uint32_t));
    GuestMemory::Read(codeAddress, std::as_writable_bytes(std::span(snapshot.code)), 256);
    snapshot.header.resize(header.header_size);
    GuestMemory::Read(guestHeaderAddress, snapshot.header, 1);
    return snapshot;
}

std::shared_ptr<const ShaderSnapshot> ReadRegisteredShader(std::uint64_t guestHeaderAddress) {
    return std::make_shared<const ShaderSnapshot>(ReadRegisteredShaderSnapshot(guestHeaderAddress));
}

ShaderSnapshot NullPixelShaderSnapshot() {
    ShaderSnapshot snapshot{NullPixelProgramAddress(), reinterpret_cast<std::uintptr_t>(&nullPixelShader), nullPixelShader.type, {}, {}};
    snapshot.code.assign(std::begin(nullPixelCode), std::end(nullPixelCode));
    snapshot.header.resize(sizeof(Shader));
    std::memcpy(snapshot.header.data(), &nullPixelShader, sizeof(Shader));
    return snapshot;
}

std::shared_ptr<const ShaderSnapshot> CaptureNullPixelShader() {
    return std::make_shared<const ShaderSnapshot>(NullPixelShaderSnapshot());
}

std::shared_ptr<const ShaderSnapshot> CaptureRawComputeShader(std::uint64_t address, std::size_t contiguousBytes) {
    GuestMemory::CheckRange(reinterpret_cast<const void*>(address), sizeof(std::uint32_t), 256);
    constexpr std::size_t limit = 1024 * 1024;
    const auto bytes = std::min(contiguousBytes, limit);
    require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "raw compute code range overflows");
    const auto available = bytes / sizeof(std::uint32_t);
    ShaderSnapshot snapshot{address, 0, 0, {}, {}};
    while (snapshot.code.size() < available) {
        const auto previous = snapshot.code.size();
        snapshot.code.resize(std::min(available, std::max<std::size_t>(64, previous * 2)));
        GuestMemory::Read(address + previous * sizeof(std::uint32_t),
            std::as_writable_bytes(std::span(snapshot.code).subspan(previous)), alignof(std::uint32_t));
        try {
            const auto decoded = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(snapshot.code);
            const auto& last = decoded.instructions.back();
            snapshot.code.resize(last.programCounter / sizeof(std::uint32_t) + last.wordCount);
            return std::make_shared<const ShaderSnapshot>(std::move(snapshot));
        } catch (const std::out_of_range&) {
            if (snapshot.code.size() == available) break;
        }
    }
    throw std::runtime_error("AGC driver: raw compute program has no reachable end within mapped code or the size limit");
}

std::uint64_t NullPixelProgramAddress() {
    return reinterpret_cast<std::uintptr_t>(nullPixelCode);
}

std::optional<FloatModeRegister> FloatModeRegisterFor(std::uint8_t type) {
    switch (type) {
    case 0: return FloatModeRegister{0x212, 26};
    case 1: return FloatModeRegister{0x00a, 29};
    case 2: case 4: case 6: return FloatModeRegister{0x08a, 31};
    case 5: case 7: return FloatModeRegister{0x10a, 30};
    default: return std::nullopt;
    }
}

ShaderRecompiler::ShaderFloatMode DecodeFloatMode(const FloatModeRegister& reg, std::uint32_t rsrc1) {
    return {(rsrc1 >> 12u) & 0xffu, ((rsrc1 >> 21u) & 1u) != 0u, ((rsrc1 >> 23u) & 1u) != 0u,
        ((rsrc1 >> reg.fp16OverflowBit) & 1u) != 0u};
}

std::optional<ShaderRecompiler::ShaderFloatMode> HeaderFloatMode(std::uint8_t type, std::uint64_t headerAddress,
    std::span<const std::byte> header) {
    const auto reg = FloatModeRegisterFor(type);
    if (!reg || header.size() < sizeof(Shader)) return std::nullopt;
    Shader fixed;
    std::memcpy(&fixed, header.data(), sizeof(fixed));
    if (fixed.num_sh_registers == 0) return std::nullopt;
    // Same bounds as the registration-time decoder: the array must lie inside the captured header.
    const auto address = reinterpret_cast<std::uintptr_t>(fixed.sh_registers);
    require(address >= headerAddress && address - headerAddress <= header.size(), "shader metadata is outside the registered header");
    const auto offset = static_cast<std::size_t>(address - headerAddress);
    require(fixed.num_sh_registers <= (header.size() - offset) / sizeof(ShaderRegister), "truncated shader metadata");
    std::optional<std::uint32_t> rsrc1;
    for (std::size_t i = 0; i < fixed.num_sh_registers; ++i) {
        ShaderRegister value;
        std::memcpy(&value, header.data() + offset + i * sizeof(ShaderRegister), sizeof(value));
        // A repeated register keeps its last value, as DecodeRegisteredState's insert_or_assign does.
        if (value.offset == reg->offset) rsrc1 = value.value;
    }
    if (!rsrc1) return std::nullopt;
    return DecodeFloatMode(*reg, *rsrc1);
}

}
