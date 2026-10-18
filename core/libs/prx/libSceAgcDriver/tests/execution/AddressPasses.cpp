#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t VertexBuffers = 3;
constexpr std::size_t VertexTableOffset = 0x300;
constexpr std::size_t VertexDataOffset = 0x800;
constexpr std::size_t ColorOffset = 0;
constexpr std::size_t ColorBytes = 65536;
constexpr std::uint32_t RenderTargetTileMode = 0x1b;
constexpr std::size_t BlockBytes = 65536;

alignas(256) constexpr std::array<std::uint32_t, 40> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u,
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u,
    0xbf8cc07fu,
    0xe00c2000u, 0x80010a05u, 0xe00c2000u, 0x80020e05u, 0xe00c2000u, 0x80031205u,
    0xbf8c3f70u,
    0x0602030au, 0x0604050bu, 0x0606070cu, 0x0608090du,
    0x0602030eu, 0x0604050fu, 0x06060710u, 0x06080911u,
    0x06020312u, 0x06040513u, 0x06060714u, 0x06080915u,
    0xf80008cfu, 0x04030201u,
    0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 40> VertexCodeUser8 = [] {
    auto code = VertexCode;
    for (const std::size_t load : {10u, 12u, 14u}) code[load] |= 4u;
    return code;
}();

alignas(256) constexpr std::array<std::uint32_t, 11> PixelRead{0x7e1a0280u, 0xdc308000u, 0x0c02000du, 0xbf8c3f70u, 0x7e10030cu, 0x7e120280u, 0x7e140280u, 0x7e160280u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 11> PixelStore{0x7e1a0280u, 0x7e1802f2u, 0xdc708000u, 0x00020c0du, 0x7e1002f2u, 0x7e1202f2u, 0x7e1402f2u, 0x7e1602f2u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header ReadHeader{};
alignas(8) Header StoreHeader{};

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "address passes: cannot allocate a guest block");
        std::memset(block, 0, BlockBytes);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    void Map(bool writable) const { GuestAllocations::Mutation().Add(block, BlockBytes, true, writable, true); }
    std::uint64_t At(std::size_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block + offset)); }
    std::uint8_t* Bytes(std::size_t offset) const { return block + offset; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t stride, std::uint32_t records, std::uint32_t word3) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, word3};
}

void FillVertices(const GuestBlock& block) {
    for (std::uint32_t buffer = 0; buffer < VertexBuffers; ++buffer) {
        const auto vertices = BufferDescriptor(block.At(VertexDataOffset + buffer * 0x100u), 16, 3, 0x0104dfacu);
        std::memcpy(block.Bytes(VertexTableOffset + buffer * 16u), vertices.data(), sizeof(vertices));
    }
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

struct Step {
    const std::uint32_t* pixel;
    std::uint64_t pointer;
};

void Submit(const GuestBlock& vertices, const GuestBlock& target, std::initializer_list<Step> steps) {
    const auto color = target.At(ColorOffset);
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCodeUser8.data());
    const auto table = vertices.At(VertexTableOffset);
    const float scale = 32.0f;
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000u | (RenderTargetTileMode << 14u)}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(scale)}, {0x110, std::bit_cast<std::uint32_t>(scale)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 2u << 1u}, {0x8c, static_cast<std::uint32_t>(table)}, {0x8d, static_cast<std::uint32_t>(table >> 32u)}, {0x00b, 8u << 1u}, {0x00c, 0}, {0x00d, 0}}) Append(commands, 0x76, {offset, value});
    for (const auto& step : steps) {
        const auto pixel = reinterpret_cast<std::uintptr_t>(step.pixel);
        Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(pixel >> 8u), static_cast<std::uint32_t>(pixel >> 40u)});
        Append(commands, 0x76, {0x00e, static_cast<std::uint32_t>(step.pointer), static_cast<std::uint32_t>(step.pointer >> 32u)});
        Append(commands, 0x2d, {3, 2});
    }
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "address passes: the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

AgcDriver::Graphics::DrawPassCounts AddressDraws() {
    return AgcDriver::Graphics::DrawPassTotals()[1];
}

void Expect(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("address passes: " + what);
}

std::string Counts(const AgcDriver::Graphics::DrawPassCounts& before, const AgcDriver::Graphics::DrawPassCounts& after) {
    return std::to_string(after.draws - before.draws) + " address-based draws, " + std::to_string(after.passesBegun - before.passesBegun) + " began a pass, " + std::to_string(after.passesContinued - before.passesContinued) + " continued one";
}

void ReadersShareAPass(const GuestBlock& vertices, const GuestBlock& data, const GuestBlock& target) {
    const auto before = AddressDraws();
    Submit(vertices, target, {{PixelRead.data(), data.At(0)}, {PixelRead.data(), data.At(0)}});
    const auto after = AddressDraws();
    std::printf("two draws reading through the table: %s\n", Counts(before, after).c_str());
    Expect(after.draws - before.draws == 2 && after.passesContinued - before.passesContinued == 1, "a draw reading through the table after another did not continue its pass: " + Counts(before, after));
}

void StoreAfterTableReadBreaks(const GuestBlock& vertices, const GuestBlock& data, const GuestBlock& target) {
    const auto before = AddressDraws();
    Submit(vertices, target, {{PixelRead.data(), data.At(32)}, {PixelStore.data(), data.At(32)}});
    const auto after = AddressDraws();
    std::printf("a draw storing through the table after a draw reading through it: %s\n", Counts(before, after).c_str());
    Expect(after.draws - before.draws == 2 && after.passesBegun - before.passesBegun == 2, "a draw storing through the table continued the pass of a draw that read through it: " + Counts(before, after));
}

void ReadSeesStore(const GuestBlock& vertices, const GuestBlock& data, const GuestBlock& target) {
    std::memset(target.Bytes(ColorOffset), 0, ColorBytes);
    std::memset(data.Bytes(48), 0, 4);
    const auto before = AddressDraws();
    Submit(vertices, target, {{PixelStore.data(), data.At(48)}, {PixelRead.data(), data.At(48)}});
    const auto after = AddressDraws();
    AgcDriver::GuestMemory::FlushGpuWrites(static_cast<std::uintptr_t>(target.At(ColorOffset)), ColorBytes);
    float stored = 0;
    std::memcpy(&stored, data.Bytes(48), sizeof(stored));
    unsigned brightest = 0;
    for (std::size_t i = 0; i < ColorBytes; ++i) brightest = std::max(brightest, static_cast<unsigned>(target.Bytes(ColorOffset)[i]));
    std::printf("a draw reading through the table what the previous draw stored: stored %g, brightest color byte %u, %s\n", static_cast<double>(stored), brightest, Counts(before, after).c_str());
    Expect(stored == 1.0f, "the store through the table did not reach guest memory");
    Expect(brightest == 255u, "the read through the table did not see the previous draw's store");
    Expect(after.passesBegun - before.passesBegun == 2, "a draw reading through the table continued the pass of a draw that stored through it: " + Counts(before, after));
}

}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", "1");
#else
        setenv("APS5_PROFILE_DRAW", "1", 1);
#endif
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Register(VertexHeader, VertexCodeUser8.data(), sizeof(VertexCodeUser8), 2);
        Register(ReadHeader, PixelRead.data(), sizeof(PixelRead), 1);
        Register(StoreHeader, PixelStore.data(), sizeof(PixelStore), 1);
        GuestBlock readOnlyVertices;
        GuestBlock data;
        GuestBlock target;
        FillVertices(readOnlyVertices);
        readOnlyVertices.Map(false);
        data.Map(true);
        target.Map(true);
        const std::string only = argc > 1 ? argv[1] : "";
        if (only.empty() || only == "--readers") ReadersShareAPass(readOnlyVertices, data, target);
        if (only.empty() || only == "--store-after-read") StoreAfterTableReadBreaks(readOnlyVertices, data, target);
        if (only.empty() || only == "--read-sees-store") ReadSeesStore(readOnlyVertices, data, target);
        AgcDriverShutdown_nid_postfix();
        std::puts("address passes: draws reading through the BDA table share a render pass, and a store through it still begins one");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
