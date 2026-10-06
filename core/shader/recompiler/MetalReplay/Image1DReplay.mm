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

    Replay(std::uint32_t wave, std::uint64_t base) : wave(wave), base(base), code(Code1D),
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
            static_cast<std::uint32_t>(TextureAddress >> 8u), (22u << 20u) | (3u << 30u), 1u << 31u,
            0xfacu | (2u << 16u) | (8u << 28u), 0, 2u << 4u, 0, 0,
            0x92u, (4u * 256u) << 12u, (1u << 22u) | (2u << 26u), 0};
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
            std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges{
                {TextureAddress - 256, std::as_writable_bytes(std::span(texture)), false}};
            wave32.AddRanges(ranges);
            wave64.AddRanges(ranges);
            samplerBank.AddRanges(ranges);
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
            samplerBank.Run();
            Require(texture == originalTexture, "Public sampler bank changed another borrowed 1D texture");
            AgcDriver::Metal::MetalDriver::Get().Shutdown();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
