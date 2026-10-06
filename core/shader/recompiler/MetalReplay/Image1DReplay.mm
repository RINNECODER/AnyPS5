#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t Inputs = 12, Results = 16, SampleCases = 5, GuardWords = 64, Sentinel = 0xdeadbeef;
constexpr std::uint64_t TextureAddress = 0x100100;

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

void RegisterPacket(std::vector<std::uint32_t>& commands, std::uint32_t offset,
                    std::span<const std::uint32_t> words) {
    commands.push_back(0xc0007600u | (static_cast<std::uint32_t>(words.size()) << 16u));
    commands.push_back(offset);
    commands.insert(commands.end(), words.begin(), words.end());
}

alignas(256) constexpr std::array<std::uint32_t, 87> Code1D{
    0x160200b0, 0x34060086, 0xe0381000, 0x80000401, 0xe0381010, 0x80002801, 0xe0381020, 0x80003201,
    0xbe900404, 0xbe920406, 0xbe940408, 0xbe96040a, 0xbe98040c, 0xbe9a040e, 0x7e140280, 0x7e160280,
    0x7e180280, 0x7e1a0280, 0x7e1c0280, 0x7e1e0280, 0x7e200280, 0x7e220280, 0x7e240280, 0x7e260280,
    0x7e280280, 0x7e2a0280, 0x7e2c0280, 0x7e2e0280, 0x7e300280, 0x7e320280, 0xbf8c3f70, 0x5e585328,
    0xd52f002d, 0x0201012a, 0x5e5c6732, 0xd52f002f, 0x02010134, 0x5e600b04, 0xd52f0031, 0x02010106,
    0xd52f0036, 0x02010128, 0xd52f0037, 0x02010132, 0xd52f003b, 0x02010104, 0x7e780281, 0x7e7a02f6,
    0x7e7c02ff, 0x00000101, 0x7e800328, 0x7e820332, 0x7e840304, 0xf0880100, 0x00c40a40, 0x7e800336,
    0x7e820337, 0x7e840304, 0xf0880101, 0x00c40b40, 0x7e800336, 0x7e820337, 0x7e840304, 0xf1a00101,
    0x00c40c40, 0x7e800328, 0x7e820332, 0x7e84033b, 0xf0880100, 0x40c40d40, 0x7e800336, 0x7e820337,
    0x7e84033b, 0xf1a00101, 0x40c40e40, 0xbf8c3f70, 0xe0701600, 0x80000a03, 0xe0701604, 0x80000b03,
    0xe0701608, 0x80000c03, 0xe070160c, 0x80000d03, 0xe0701610, 0x80000e03, 0xbf810000,
};

struct Replay {
    std::uint32_t wave;
    std::uint64_t base;
    std::array<std::uint32_t, Code1D.size()> code;
    std::vector<std::uint32_t> buffer;
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> header{};
    std::vector<std::uint32_t> commands;
    Packet packet{};

    Replay(std::uint32_t wave, std::uint64_t base, std::uint32_t viewFloor = 0, bool aniso = false) : wave(wave), base(base), code(Code1D),
        buffer(GuardWords + wave * (Inputs + Results) + GuardWords, Sentinel) {
        for (auto& word : code) {
            if ((word & 0xfffff000u) == 0xe0701000u && (word & 0xfffu) >= 32u * Inputs * 4u)
                word += (wave - 32u) * Inputs * 4u;
        }
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.user_data = reinterpret_cast<ShaderUserData*>(base + 0x10000 + sizeof(Shader));
        shader.code = reinterpret_cast<const volatile void*>(base);
        shader.header_size = static_cast<std::uint32_t>(header.size());
        shader.shader_size = sizeof(code);
        shader.type = 0;
        std::memcpy(header.data(), &shader, sizeof(shader));
        std::array<std::uint32_t, 16> users{
            static_cast<std::uint32_t>(base + 0x20100), 0, wave * (Inputs + Results) * 4u, 0x01016fac,
            static_cast<std::uint32_t>(TextureAddress >> 8u), (22u << 20u) | (3u << 30u) | (viewFloor << 8u), 1u << 31u,
            0xfacu | (2u << 16u) | (8u << 28u), 0, 2u << 4u, 0, 0,
            0x92u | (aniso ? 4u << 9u : 0u), (4u * 256u) << 12u,
            (1u << 22u) | (2u << 26u) | (aniso ? 3u << 20u : 0u), 0};
        const std::array<std::uint32_t, 3> threads{wave, 1, 1};
        const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(base >> 8u), 0};
        const std::array<std::uint32_t, 1> resources{32};
        RegisterPacket(commands, 0x207, threads);
        RegisterPacket(commands, 0x20c, program);
        RegisterPacket(commands, 0x213, resources);
        RegisterPacket(commands, 0x240, users);
        commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, wave == 32 ? 0x8041u : 0x41u});
        packet = {reinterpret_cast<std::uint32_t*>(base + 0x30000), static_cast<std::uint32_t>(commands.size()), 0, {}};
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        ranges.push_back({base, std::as_writable_bytes(std::span(code)), false});
        ranges.push_back({base + 0x10000, header, false});
        ranges.push_back({base + 0x20000, std::as_writable_bytes(std::span(buffer)), true});
        ranges.push_back({base + 0x30000, std::as_writable_bytes(std::span(commands)), false});
        ranges.push_back({base + 0x40000, std::as_writable_bytes(std::span(&packet, 1)), false});
    }

    void Run(float dx, float wanted) {
        std::fill(buffer.begin(), buffer.end(), Sentinel);
        for (std::uint32_t lane = 0; lane < wave; ++lane) {
            auto start = buffer.begin() + GuardWords + lane * Inputs;
            std::fill_n(start, Inputs, 0u);
            start[0] = std::bit_cast<std::uint32_t>(0.5f);
            start[4] = std::bit_cast<std::uint32_t>(dx);
        }
        auto expected = buffer;
        for (std::uint32_t lane = 0; lane < wave; ++lane)
            std::fill_n(expected.begin() + GuardWords + wave * Inputs + lane * Results, SampleCases,
                std::bit_cast<std::uint32_t>(wanted));
        AgcDriver::Submit(reinterpret_cast<const Packet*>(base + 0x40000), 0x20);
        AgcDriverWaitIdle_nid_postfix();
        for (std::size_t word = 0; word < buffer.size(); ++word) {
            Require(buffer[word] == expected[word], "Public original RDNA logical 1D mip gradient wave" +
                std::to_string(wave) + " dx=" + std::to_string(dx) + " word=" + std::to_string(word) +
                " actual=" + std::to_string(buffer[word]) + " expected=" + std::to_string(expected[word]));
        }
        std::cout << "Public original RDNA logical 1D mip gradient wave" << wave
                  << ", dx=" << dx << ", five variants=" << wanted << ", inputs and guards passed\n";
    }
};

alignas(256) constexpr std::array<std::uint32_t, 41> BiasCode{
    0x34020087, 0xe0301000, 0x80000501, 0xe0301004, 0x80000601, 0xbf8c3f70, 0xf0940f08, 0x40610c05,
    0xe0701020, 0x80000c01, 0xe0701024, 0x80000d01, 0xe0701028, 0x80000e01, 0xe070102c, 0x80000f01,
    0x7e0802ff, 0x00000201, 0xf0d40f08, 0x40610c04, 0xe0701030, 0x80000c01, 0xe0701034, 0x80000d01,
    0xe0701038, 0x80000e01, 0xe070103c, 0x80000f01, 0x7e0802ff, 0x00003e3f, 0xf0d40f08, 0x40610c04,
    0xe0701040, 0x80000c01, 0xe0701044, 0x80000d01, 0xe0701048, 0x80000e01, 0xe070104c, 0x80000f01,
    0xbf810000,
};
alignas(256) constexpr std::array<std::uint32_t, 19> BiasClampCode{
    0x34020087, 0xe0301000, 0x80000501, 0xe0301004, 0x80000601, 0xe0301008, 0x80000701, 0xbf8c3f70,
    0xf0980f08, 0x40610c05, 0xe0701050, 0x80000c01, 0xe0701054, 0x80000d01, 0xe0701058, 0x80000e01,
    0xe070105c, 0x80000f01, 0xbf810000,
};
struct MinimumLodFamilies {
    static constexpr std::uint32_t Threads = 32, Words = 32;
    std::uint64_t base;
    bool clamp;
    std::vector<std::uint32_t> code;
    std::array<std::uint32_t, GuardWords + Threads * Words + GuardWords> buffer;
    std::array<std::uint32_t, GuardWords + 2048 + GuardWords> texture;
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> header{};
    std::vector<std::uint32_t> commands;
    Packet packet{};

    MinimumLodFamilies(std::uint64_t base = 0x800000, bool clamp = true, std::uint32_t floor = 0) :
        base(base), clamp(clamp), code(clamp ? std::vector<std::uint32_t>(BiasClampCode.begin(), BiasClampCode.end()) :
            std::vector<std::uint32_t>(BiasCode.begin(), BiasCode.end())) {
        texture.fill(Sentinel);
        constexpr std::array<std::uint32_t, 5> offsets{3840, 1792, 768, 256, 0};
        for (std::uint32_t mip = 0; mip < 5; ++mip)
            for (std::uint32_t y = 0; y < (16u >> mip); ++y)
                std::fill_n(texture.begin() + GuardWords + offsets[mip] / 4 + y * 64,
                    16u >> mip, std::bit_cast<std::uint32_t>(float(mip * 16u)));
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.code = reinterpret_cast<const volatile void*>(base + 0x30000);
        shader.user_data = reinterpret_cast<ShaderUserData*>(base + 0x40000 + sizeof(Shader));
        shader.header_size = header.size();
        shader.shader_size = code.size() * 4;
        shader.type = 0;
        std::memcpy(header.data(), &shader, sizeof(shader));
        const std::array<std::uint32_t, 16> users{
            static_cast<std::uint32_t>(base + 0x20100), 0, Threads * Words * 4, 0x01016fac,
            static_cast<std::uint32_t>((base + 0x100) >> 8), (22u << 20) | (3u << 30) | (floor << 8), 3u | (15u << 14),
            0xfacu | (4u << 16) | (9u << 28), 0, 4u << 4, 0, 0,
            0x92, (4u * 256u) << 12, (1u << 22) | (2u << 26), 0};
        const std::array<std::uint32_t, 3> threads{Threads, 1, 1};
        const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>((base + 0x30000) >> 8), 0};
        const std::array<std::uint32_t, 1> resources{32};
        RegisterPacket(commands, 0x207, threads);
        RegisterPacket(commands, 0x20c, program);
        RegisterPacket(commands, 0x213, resources);
        RegisterPacket(commands, 0x240, users);
        commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, 0x8041});
        packet = {reinterpret_cast<std::uint32_t*>(base + 0x50000), static_cast<std::uint32_t>(commands.size()), 0, {}};
        buffer.fill(Sentinel);
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            auto* input = buffer.data() + GuardWords + lane * Words;
            input[0] = 0x38000000;
            input[1] = 0x38003800;
            input[2] = (lane % 4 == 0 ? 0u : lane % 4 == 1 ? 0x3c00u : lane % 4 == 2 ? 0x4000u : 0x4200u) | 0x44000000;
        }
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        ranges.push_back({base, std::as_writable_bytes(std::span(texture)), false});
        ranges.push_back({base + 0x20000, std::as_writable_bytes(std::span(buffer)), true});
        ranges.push_back({base + 0x30000, std::as_writable_bytes(std::span(code)), false});
        ranges.push_back({base + 0x40000, header, false});
        ranges.push_back({base + 0x50000, std::as_writable_bytes(std::span(commands)), false});
        ranges.push_back({base + 0x60000, std::as_writable_bytes(std::span(&packet, 1)), false});
    }

    void Run(float viewFloor = 0.0f) {
        const auto textureGolden = texture;
        auto expected = buffer;
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            const auto level = clamp ? std::max(viewFloor, float(lane % 4)) : viewFloor;
            for (std::uint32_t sample = 0; sample < (clamp ? 1u : 3u); ++sample) {
                const auto word = GuardWords + lane * Words + (clamp ? 20u : 8u + sample * 4u);
                expected[word] = std::bit_cast<std::uint32_t>(level * 16.0f);
                expected[word + 1] = expected[word + 2] = 0;
                expected[word + 3] = std::bit_cast<std::uint32_t>(1.0f);
            }
        }
        const auto originalCode = code, originalCommands = commands;
        const auto originalHeader = header;
        const auto originalPacket = packet;
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(base + 0x40000));
        AgcDriver::Submit(reinterpret_cast<const Packet*>(base + 0x60000), 0x20);
        AgcDriverWaitIdle_nid_postfix();
        for (std::size_t word = 0; word < buffer.size(); ++word)
            Require(buffer[word] == expected[word], "Public original Bias/CL minimum LOD family=" + std::to_string(clamp) +
                " floor=" + std::to_string(viewFloor) + " word=" + std::to_string(word) +
                " observed=" + std::to_string(buffer[word]) + " expected=" + std::to_string(expected[word]));
        Require(texture == textureGolden, "Instruction minimum LOD changed read-only texture or mip padding");
        Require(code == originalCode && commands == originalCommands && header == originalHeader &&
            std::memcmp(&packet, &originalPacket, sizeof(packet)) == 0,
            "Instruction minimum LOD changed read-only shader or PM4 bytes");
        std::cout << "Public original Bias/CL minimum LOD family=" << clamp << " floor=" << viewFloor
                  << ": 32 exact mip results, full inputs, padding and guards passed\n";
    }
};

struct DynamicStorageMipReplay {
    static constexpr std::uint32_t Threads = 32, TexelWords = 2048;
    static constexpr std::uint64_t Base = 0xb00000;
    std::array<std::uint32_t, 17> code{
        0x4a500081, 0xd5690014, 0x0201ff28, 0x9e3779b1, 0x4a2a28ff, 0x7f4a7c15, 0x4a2c28ff, 0xfe94f82a,
        0x4a2e28ff, 0x7ddf743f, 0x2c3c0081, 0x36400081, 0x34424081, 0x4c3e4284, 0xf02c1b08, 0x0001141e,
        0xbf810000,
    };
    std::array<std::uint32_t, GuardWords + TexelWords + GuardWords> texture;
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> header{};
    std::vector<std::uint32_t> commands;
    Packet packet{};

    DynamicStorageMipReplay() {
        texture.fill(Sentinel);
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.code = reinterpret_cast<const volatile void*>(Base + 0x30000);
        shader.user_data = reinterpret_cast<ShaderUserData*>(Base + 0x40000 + sizeof(Shader));
        shader.header_size = static_cast<std::uint32_t>(header.size());
        shader.shader_size = static_cast<std::uint32_t>(code.size() * 4u);
        shader.type = 0;
        std::memcpy(header.data(), &shader, sizeof(shader));
        const std::array<std::uint32_t, 16> users{
            0, 0, 0, 0,
            static_cast<std::uint32_t>((Base + 0x100) >> 8u), (20u << 20u) | (3u << 30u),
            7u | (7u << 14u), 0xfacu | (1u << 16u) | (9u << 28u), 0, 1u << 4u, 0, 0,
            0, 0, 0, 0};
        const std::array<std::uint32_t, 3> threads{Threads, 1, 1};
        const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>((Base + 0x30000) >> 8u), 0};
        const std::array<std::uint32_t, 1> resources{static_cast<std::uint32_t>(users.size()) << 1u};
        RegisterPacket(commands, 0x207, threads);
        RegisterPacket(commands, 0x20c, program);
        RegisterPacket(commands, 0x213, resources);
        RegisterPacket(commands, 0x240, users);
        commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, 0x8041});
        packet = {reinterpret_cast<std::uint32_t*>(Base + 0x50000), static_cast<std::uint32_t>(commands.size()), 0, {}};
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        ranges.push_back({Base, std::as_writable_bytes(std::span(texture)), true});
        ranges.push_back({Base + 0x30000, std::as_writable_bytes(std::span(code)), false});
        ranges.push_back({Base + 0x40000, header, false});
        ranges.push_back({Base + 0x50000, std::as_writable_bytes(std::span(commands)), false});
        ranges.push_back({Base + 0x60000, std::as_writable_bytes(std::span(&packet, 1)), false});
    }

    void Run() {
        auto expected = texture;
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            const auto level = lane & 1u;
            const auto offsetBytes = (level == 0u ? 1024u : 0u) + (4u - 2u * level) * 256u + (lane >> 1u) * 4u;
            expected[GuardWords + offsetBytes / 4u] = (lane + 1u) * 0x9e3779b1u;
        }
        const auto originalCode = code;
        const auto originalCommands = commands;
        const auto originalHeader = header;
        const auto originalPacket = packet;
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(Base + 0x40000));
        AgcDriver::Submit(reinterpret_cast<const Packet*>(Base + 0x60000), 0x20);
        AgcDriverWaitIdle_nid_postfix();
        for (std::size_t word = 0; word < texture.size(); ++word)
            Require(texture[word] == expected[word], "Public retained ImageStoreMipPck physical mip routing word=" +
                std::to_string(word) + " actual=" + std::to_string(texture[word]) +
                " expected=" + std::to_string(expected[word]));
        Require(code == originalCode && commands == originalCommands && header == originalHeader &&
            std::memcmp(&packet, &originalPacket, sizeof(packet)) == 0,
            "Public ImageStoreMipPck changed read-only guest shader, descriptor or PM4 bytes");
        std::cout << "Public retained ImageStoreMipPck: 32 independent texels in two physical mips, full padding and guards passed\n";
    }
};

template<bool AcrossWaves = false>
struct DynamicImageReplay {
    static constexpr std::uint32_t Threads = AcrossWaves ? 64u : 32u, Layers = 3, LayerWords = 1984;
    static constexpr std::uint32_t MaterialRecords = AcrossWaves ? 64u : 3u;
    static constexpr std::uint64_t Base = AcrossWaves ? 0xf00000u : 0xc00000u;
    std::array<std::uint32_t, 27> code{
        0xf4080100, 0xfa000000, 0xf4080200, 0xfa000010, 0xf4080300, 0xfa000020, 0xf4080700, 0xfa000030,
        0x7e200500, 0x93109010, 0xf4200406, 0x20000004, 0x8f108510, 0xf42c0502, 0x20000000,
        0x34140082, 0x7e0802ff, 0x3f000000, 0x7e0a02ff, 0x3f000000, 0x7e0c0280,
        0xf09c0f18, 0x00450004, 0xbf8c3f70, 0xe0701000, 0x8007000a, 0xbf810000,
    };
    std::array<std::array<std::uint32_t, GuardWords + Layers * LayerWords + GuardWords>, 2> textures;
    std::array<std::uint32_t, GuardWords + 16 + GuardWords> heap, srt;
    std::array<std::uint32_t, GuardWords + MaterialRecords * 4u + GuardWords> materials;
    std::array<std::uint32_t, GuardWords + Threads + GuardWords> output;
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> header{};
    std::vector<std::uint32_t> commands;
    Packet packet{};

    DynamicImageReplay() {
        constexpr std::array<std::uint32_t, 5> mipOffsets{3840, 1792, 768, 256, 0};
        heap.fill(Sentinel);
        materials.fill(Sentinel);
        srt.fill(Sentinel);
        output.fill(Sentinel);
        for (std::uint32_t image = 0; image < textures.size(); ++image) {
            textures[image].fill(Sentinel);
            for (std::uint32_t layer = 0; layer < Layers; ++layer)
                for (std::uint32_t mip = 0; mip < mipOffsets.size(); ++mip)
                    for (std::uint32_t y = 0; y < (16u >> mip); ++y)
                        std::fill_n(textures[image].begin() + GuardWords + layer * LayerWords + mipOffsets[mip] / 4u + y * 64u,
                            16u >> mip, std::bit_cast<std::uint32_t>(float(image * 256u + layer * 64u + mip * 16u)));
            const std::array<std::uint32_t, 8> descriptor{
                static_cast<std::uint32_t>((Base + image * 0x10000u + 0x100u) >> 8u),
                (22u << 20u) | (3u << 30u),
                3u | (15u << 14u), 0xfacu | (1u << 12u) | (4u << 16u) | (13u << 28u),
                2u | ((image + 1u) << 16u), 4u << 4u, 0, 0};
            std::copy(descriptor.begin(), descriptor.end(), heap.begin() + GuardWords + image * 8u);
        }
        for (std::uint32_t record = 0; record < MaterialRecords; ++record) {
            auto start = materials.begin() + GuardWords + record * 4u;
            std::fill_n(start, 4, 0u);
            if constexpr (AcrossWaves) start[1] = record < 32u ? 0u : 1u;
            else start[1] = record == 2 ? 1u : 0u;
        }
        const std::array<std::uint32_t, 16> table{
            static_cast<std::uint32_t>(Base + 0x20100u), 32u << 16u, 2, 0xfac,
            0x92, (4u * 256u) << 12u, (1u << 22u) | (2u << 26u), 0,
            static_cast<std::uint32_t>(Base + 0x21100u), 16u << 16u, MaterialRecords, 0xfac,
            static_cast<std::uint32_t>(Base + 0x30100u), 0, Threads * 4u, 0xfac};
        std::copy(table.begin(), table.end(), srt.begin() + GuardWords);
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.code = reinterpret_cast<const volatile void*>(Base + 0x40000u);
        shader.user_data = reinterpret_cast<ShaderUserData*>(Base + 0x50000u + sizeof(Shader));
        shader.header_size = static_cast<std::uint32_t>(header.size());
        shader.shader_size = static_cast<std::uint32_t>(code.size() * 4u);
        shader.type = 0;
        std::memcpy(header.data(), &shader, sizeof(shader));
        const std::array<std::uint32_t, 2> users{static_cast<std::uint32_t>(Base + 0x22100u), 0};
        const std::array<std::uint32_t, 3> threads{Threads, 1, 1};
        const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>((Base + 0x40000u) >> 8u), 0};
        const std::array<std::uint32_t, 1> resources{static_cast<std::uint32_t>(users.size()) << 1u};
        RegisterPacket(commands, 0x207, threads);
        RegisterPacket(commands, 0x20c, program);
        RegisterPacket(commands, 0x213, resources);
        RegisterPacket(commands, 0x240, users);
        commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, 0x8041});
        packet = {reinterpret_cast<std::uint32_t*>(Base + 0x60000u), static_cast<std::uint32_t>(commands.size()), 0, {}};
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        for (std::uint32_t image = 0; image < textures.size(); ++image)
            ranges.push_back({Base + image * 0x10000u, std::as_writable_bytes(std::span(textures[image])), false});
        ranges.push_back({Base + 0x20000u, std::as_writable_bytes(std::span(heap)), false});
        ranges.push_back({Base + 0x21000u, std::as_writable_bytes(std::span(materials)), false});
        ranges.push_back({Base + 0x22000u, std::as_writable_bytes(std::span(srt)), false});
        ranges.push_back({Base + 0x30000u, std::as_writable_bytes(std::span(output)), true});
        ranges.push_back({Base + 0x40000u, std::as_writable_bytes(std::span(code)), false});
        ranges.push_back({Base + 0x50000u, header, false});
        ranges.push_back({Base + 0x60000u, std::as_writable_bytes(std::span(commands)), false});
        ranges.push_back({Base + 0x70000u, std::as_writable_bytes(std::span(&packet, 1)), false});
    }

    void Run(bool minimumLod = false) {
        if constexpr (AcrossWaves) {
            heap[GuardWords + 1u] = (22u << 20u) | (3u << 30u) | (minimumLod ? 640u << 8u : 0u);
            heap[GuardWords + 9u] = (22u << 20u) | (3u << 30u) | (minimumLod ? 768u << 8u : 0u);
            const auto originalTextures = textures;
            const auto originalHeap = heap, originalSrt = srt;
            const auto originalMaterials = materials;
            const auto originalCode = code;
            const auto originalHeader = header;
            const auto originalCommands = commands;
            const auto originalPacket = packet;
            output.fill(Sentinel);
            auto expected = output;
            for (std::uint32_t lane = 0; lane < Threads; ++lane)
                expected[GuardWords + lane] = std::bit_cast<std::uint32_t>(lane < 32u ?
                    (minimumLod ? 104.0f : 80.0f) : (minimumLod ? 432.0f : 400.0f));
            AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(Base + 0x50000u));
            AgcDriver::Submit(reinterpret_cast<const Packet*>(Base + 0x70000u), 0x20);
            AgcDriverWaitIdle_nid_postfix();
            for (std::size_t word = 0; word < output.size(); ++word)
                Require(output[word] == expected[word], "Public original RDNA two-wave sampled images mixed-floor=" +
                    std::to_string(minimumLod) + " word=" + std::to_string(word) +
                    " actual=" + std::to_string(output[word]) + " expected=" + std::to_string(expected[word]));
            Require(textures == originalTextures && heap == originalHeap && materials == originalMaterials && srt == originalSrt,
                "Public two-wave sampled images changed read-only texels, layers, mip padding, table descriptors or keys");
            Require(code == originalCode && header == originalHeader && commands == originalCommands &&
                std::memcmp(&packet, &originalPacket, sizeof(packet)) == 0,
                "Public two-wave sampled images changed shader, header or PM4 bytes");
            std::cout << "Public original RDNA two-wave sampled images: baseMip1, baseArray1/2, relative floors="
                      << (minimumLod ? "1.5/2" : "0/0")
                      << ", all 64 lane outputs, inputs, padding and guards passed\n";
            return;
        }
        heap[GuardWords + 9u] = (22u << 20u) | (3u << 30u) | (minimumLod ? 768u << 8u : 0u);
        const auto originalTextures = textures;
        const auto originalHeap = heap, originalSrt = srt;
        const auto originalCode = code;
        const auto originalHeader = header;
        const auto originalCommands = commands;
        const auto originalPacket = packet;
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(Base + 0x50000u));
        for (std::uint32_t key = 0; key < 2; ++key) {
            materials[GuardWords + 1u] = key;
            const auto originalMaterials = materials;
            output.fill(Sentinel);
            auto expected = output;
            std::fill_n(expected.begin() + GuardWords, Threads, std::bit_cast<std::uint32_t>(key == 0 ? 80.0f : minimumLod ? 432.0f : 400.0f));
            AgcDriver::Submit(reinterpret_cast<const Packet*>(Base + 0x70000u), 0x20);
            AgcDriverWaitIdle_nid_postfix();
            for (std::size_t word = 0; word < output.size(); ++word)
                Require(output[word] == expected[word], "Public original RDNA dynamic sampled image key=" + std::to_string(key) +
                    " word=" + std::to_string(word) + " actual=" + std::to_string(output[word]) +
                    " expected=" + std::to_string(expected[word]));
            Require(textures == originalTextures && heap == originalHeap && materials == originalMaterials && srt == originalSrt,
                "Public dynamic sampled image changed read-only texels, layers, mip padding, table descriptors or keys");
            Require(code == originalCode && header == originalHeader && commands == originalCommands &&
                std::memcmp(&packet, &originalPacket, sizeof(packet)) == 0,
                "Public dynamic sampled image changed shader, header or PM4 bytes");
            std::cout << "Public original RDNA dynamic sampled image key=" << key
                      << ": baseMip1, baseArray" << key + 1u
                      << ", relative floor=" << (minimumLod && key == 1 ? 2 : 0)
                      << ", all 32 lane outputs, inputs, padding and guards passed\n";
        }
    }
};

struct SamplerBankReplay {
    static constexpr std::uint32_t Threads = 32, Samplers = 32;
    static constexpr std::uint64_t Base = 0x400000;
    std::vector<std::uint32_t> code{0x34020087, 0x7e0402ff, 0x3f000000, 0x7e060280};
    std::array<std::uint32_t, GuardWords + Samplers * 4 + GuardWords> table;
    std::array<std::uint32_t, GuardWords + 832 + GuardWords> texture;
    std::array<std::uint32_t, GuardWords + Threads * Samplers + GuardWords> output;
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> header{};
    std::vector<std::uint32_t> commands;
    Packet packet{};

    SamplerBankReplay() {
        for (std::uint32_t index = 0; index < Samplers; ++index) {
            code.insert(code.end(), {0xf4080406, 0xfa000000u | (index * 16u), 0xbf8cc07f,
                0xf0900100, 0x00810402, 0xbf8c3f70, 0xe0701000u | (index * 4u), 0x80000401});
        }
        code.push_back(0xbf810000);
        table.fill(Sentinel);
        for (std::uint32_t index = 0; index < Samplers; ++index) {
            const std::array<std::uint32_t, 4> sampler{
                0x92u, (index * 64u) | ((index * 64u) << 12u), (1u << 22u) | (2u << 26u), 0};
            std::copy(sampler.begin(), sampler.end(), table.begin() + GuardWords + index * 4);
        }
        texture.fill(Sentinel);
        constexpr std::array<std::uint32_t, 9> offsets{2304, 1792, 1536, 1280, 1024, 768, 512, 256, 0};
        for (std::uint32_t level = 0; level < offsets.size(); ++level)
            std::fill_n(texture.begin() + GuardWords + offsets[level] / 4, 256u >> level,
                std::bit_cast<std::uint32_t>(float(level * 16u)));
        output.fill(Sentinel);
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.code = reinterpret_cast<const volatile void*>(Base + 0x30000);
        shader.user_data = reinterpret_cast<ShaderUserData*>(Base + 0x40000 + sizeof(Shader));
        shader.header_size = static_cast<std::uint32_t>(header.size());
        shader.shader_size = static_cast<std::uint32_t>(code.size() * 4);
        shader.type = 0;
        std::memcpy(header.data(), &shader, sizeof(shader));
        const std::array<std::uint32_t, 14> users{
            static_cast<std::uint32_t>(Base + 0x20100), 0, Threads * Samplers * 4, 0x01016fac,
            static_cast<std::uint32_t>((Base + 0x100) >> 8u), (22u << 20u) | (3u << 30u), 63u | (1u << 31u),
            0xfacu | (8u << 16u) | (8u << 28u), 0, 8u << 4u, 0, 0,
            static_cast<std::uint32_t>(Base + 0x10100), 0};
        const std::array<std::uint32_t, 3> threads{Threads, 1, 1};
        const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>((Base + 0x30000) >> 8u), 0};
        const std::array<std::uint32_t, 1> resources{static_cast<std::uint32_t>(users.size()) << 1u};
        RegisterPacket(commands, 0x207, threads);
        RegisterPacket(commands, 0x20c, program);
        RegisterPacket(commands, 0x213, resources);
        RegisterPacket(commands, 0x240, users);
        commands.insert(commands.end(), {0xc0031500u, 1, 1, 1, 0x8041});
        packet = {reinterpret_cast<std::uint32_t*>(Base + 0x50000), static_cast<std::uint32_t>(commands.size()), 0, {}};
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        ranges.push_back({Base, std::as_writable_bytes(std::span(texture)), false});
        ranges.push_back({Base + 0x10000, std::as_writable_bytes(std::span(table)), false});
        ranges.push_back({Base + 0x20000, std::as_writable_bytes(std::span(output)), true});
        ranges.push_back({Base + 0x30000, std::as_writable_bytes(std::span(code)), false});
        ranges.push_back({Base + 0x40000, header, false});
        ranges.push_back({Base + 0x50000, std::as_writable_bytes(std::span(commands)), false});
        ranges.push_back({Base + 0x60000, std::as_writable_bytes(std::span(&packet, 1)), false});
    }

    void Run() {
        const auto originalTable = table;
        const auto originalTexture = texture;
        const auto originalCode = code, originalCommands = commands;
        const auto originalHeader = header;
        const auto originalPacket = packet;
        AgcDriver::Submit(reinterpret_cast<const Packet*>(Base + 0x60000), 0x20);
        AgcDriverWaitIdle_nid_postfix();
        for (std::size_t word = 0; word < output.size(); ++word) {
            const auto wanted = word >= GuardWords && word < GuardWords + Threads * Samplers ?
                std::bit_cast<std::uint32_t>(float(((word - GuardWords) % Samplers) * 4u)) : Sentinel;
            Require(output[word] == wanted, "Public original RDNA 32 sampler identities word=" + std::to_string(word) +
                " actual=" + std::to_string(output[word]) + " expected=" + std::to_string(wanted));
        }
        Require(table == originalTable && texture == originalTexture,
            "Public 32 sampler identities changed read-only descriptors, texels or padding");
        Require(code == originalCode && commands == originalCommands && header == originalHeader &&
            std::memcmp(&packet, &originalPacket, sizeof(packet)) == 0,
            "Public 32 sampler identities changed read-only shader or PM4 submission memory");
        std::cout << "Public original RDNA 32 sampler identities: 1024 quarter-LOD results, source padding and guards passed\n";
    }
};
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            Require(argc == 2, "Image 1D replay requires the utility metallib path");
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Image 1D replay requires Metal 3");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, error ? error.localizedDescription.UTF8String : "Cannot load utility metallib");
            std::array<std::uint32_t, GuardWords + 3 * 64 + GuardWords> texture;
            texture.fill(Sentinel);
            std::fill_n(texture.begin() + GuardWords, 1, std::bit_cast<std::uint32_t>(32.0f));
            std::fill_n(texture.begin() + GuardWords + 64, 2, std::bit_cast<std::uint32_t>(16.0f));
            std::fill_n(texture.begin() + GuardWords + 128, 4, 0u);
            const auto originalTexture = texture;
            Replay wave32(32, 0x200000), wave64(64, 0x300000);
            SamplerBankReplay samplerBank;
            DynamicImageReplay<> dynamicImages;
            DynamicImageReplay<true> nonuniformImages;
            MinimumLodFamilies instructionClamp;
            MinimumLodFamilies biasZero(0x700000, false, 0), biasFloor(0x900000, false, 384),
                clampFloor(0xa00000, true, 384);
            Replay floorGrad(32, 0x500000, 384), anisoZero(32, 0xd00000, 0, true), anisoFloor(32, 0xe00000, 384, true);
            DynamicStorageMipReplay storageMips;
            std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges{
                {TextureAddress - 256, std::as_writable_bytes(std::span(texture)), false}};
            wave32.AddRanges(ranges);
            wave64.AddRanges(ranges);
            samplerBank.AddRanges(ranges);
            dynamicImages.AddRanges(ranges);
            nonuniformImages.AddRanges(ranges);
            instructionClamp.AddRanges(ranges);
            for (auto* replay : {&biasZero, &biasFloor, &clampFloor}) replay->AddRanges(ranges);
            for (auto* replay : {&floorGrad, &anisoZero, &anisoFloor}) replay->AddRanges(ranges);
            storageMips.AddRanges(ranges);
            AgcDriver::Metal::MetalDriver::Get().Configure((__bridge void*)device, (__bridge void*)library, ranges);
            AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(wave32.base + 0x10000));
            AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(wave64.base + 0x10000));
            AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(SamplerBankReplay::Base + 0x40000));
            for (auto* replay : {&wave32, &wave64}) {
                for (const auto dx : {0.25f, 0.5f, 1.0f}) {
                    replay->Run(dx, dx == 0.25f ? 0.0f : dx == 0.5f ? 16.0f : 32.0f);
                    Require(texture == originalTexture, "Public 1D sampling changed read-only texels or mip-row padding");
                }
            }
            instructionClamp.Run();
            samplerBank.Run();
            dynamicImages.Run();
            dynamicImages.Run(true);
            storageMips.Run();
            biasZero.Run();
            AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(floorGrad.base + 0x10000));
            for (const auto dx : {0.25f, 0.5f, 1.0f}) floorGrad.Run(dx, dx == 1.0f ? 32.0f : 24.0f);
            for (auto* replay : {&anisoZero, &anisoFloor}) {
                AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(replay->base + 0x10000));
                for (const auto dx : {0.25f, 0.5f, 1.0f}) replay->Run(dx, replay == &anisoZero ? 0.0f : 24.0f);
            }
            biasFloor.Run(1.5f);
            clampFloor.Run(1.5f);
            nonuniformImages.Run();
            nonuniformImages.Run(true);
            Require(texture == originalTexture, "Public sampler bank changed another borrowed 1D texture");
            AgcDriver::Metal::MetalDriver::Get().Shutdown();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
