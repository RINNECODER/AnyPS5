#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "SceShaders.hpp"
#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t GuardWords = 64, GuardBytes = GuardWords * 4, Sentinel = 0xdeadbeef;
constexpr std::uint64_t Base = 0x200000;
enum class Offset { None, Zero, Signed, Poisoned };
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string Hex(std::uint32_t value) {
    char text[16]; std::snprintf(text, sizeof(text), "0x%08x", value); return text;
}
const char* Name(Offset offset) {
    switch (offset) {
        case Offset::None: return "no-offset";
        case Offset::Zero: return "encoded-zero";
        case Offset::Signed: return "signed-clean";
        case Offset::Poisoned: return "signed-poison";
    }
    throw std::runtime_error("Invalid fixture offset mode");
}
std::uint32_t Texel(std::uint32_t x, std::uint32_t component) {
    return std::bit_cast<std::uint32_t>(float(101u + x * 7u + component * 1000u));
}
// The coordinate is generated as a dyadic texel centre. The oracle keeps the
// corresponding integer, independently of all shader emitter/texture helpers.
int CoordinateIndex(std::uint32_t lane, std::uint32_t batch, std::uint32_t width) {
    return int((lane * 2u + (lane / 32u) * 3u + batch * 11u) % (3u * width + 8u)) - int(width) - 4;
}
std::array<std::uint32_t, 4> Expected(int coordinateIndex, std::uint32_t packed,
    Offset offset, std::uint32_t width, bool edge) {
    const int field = int(packed & 63u);
    const int signedX = field >= 32 ? field - 64 : field;
    const int left = coordinateIndex + (offset == Offset::None ? 0 : signedX);
    const auto address = [&](int x) {
        return std::uint32_t(edge ? std::clamp(x, 0, int(width) - 1) : ((x % int(width)) + int(width)) % int(width));
    };
    const auto first = Texel(address(left), 1), second = Texel(address(left + 1), 1);
    return {first, second, second, first};
}
void Registers(std::vector<std::uint32_t>& words, std::uint32_t address, std::span<const std::uint32_t> values) {
    words.push_back(0xc0007600u | (std::uint32_t(values.size()) << 16u));
    words.push_back(address); words.insert(words.end(), values.begin(), values.end());
}
// Public f6ce77bb ImageGatherOneDimensional.cpp program, retaining its real
// input load, gather and four-word output store. Select green (dmask2), and
// move the address into a separated register for the NSA variant.
std::vector<std::uint32_t> Program(Offset offset, bool nsa, std::uint32_t opcode = 0,
    std::uint32_t dimension = 0) {
    const bool hasOffset = offset != Offset::None;
    std::vector<std::uint32_t> words{0x34020084, 0xe0381000, 0x80000201, 0xbf8c3f70};
    if (nsa) words.insert(words.end(), {0x7e280303, 0x7e0602ff, 0x7fc00001}); // v20=u; poison contiguous v3
    const auto operation = opcode ? opcode : hasOffset ? 0x57u : 0x47u;
    words.push_back(0xf0000200u | (operation << 18u) | (dimension << 3u) | (nsa ? 2u : 0u));
    words.push_back(0x00820600u | (hasOffset ? 2u : nsa ? 20u : 3u));
    if (nsa) words.push_back(0x17161514); // first NSA address is v20
    words.insert(words.end(), {0xbf8c3f70, 0xe0781000, 0x80010601, 0xbf810000});
    return words;
}
struct Replay {
    std::uint32_t lanes, width, batch;
    bool edge, linear, nsa;
    Offset offset;
    std::vector<std::uint32_t> input, output, texture, code, commands;
    std::array<std::byte, GuardBytes * 2 + sizeof(Shader) + sizeof(ShaderUserData)> header{};
    std::array<std::byte, GuardBytes * 2 + sizeof(Packet)> packetBytes{};
    std::array<std::uint32_t, 20> users{};
    std::size_t codeWords = 0, commandWords = 0;
    std::string label;

    Replay(std::uint32_t lanes, std::uint32_t width, bool edge, bool linear, bool nsa,
        Offset offset, std::uint32_t batch) : lanes(lanes), width(width), batch(batch), edge(edge),
        linear(linear), nsa(nsa), offset(offset), input(GuardWords * 2 + lanes * 4, Sentinel),
        output(GuardWords * 2 + lanes * 8, Sentinel) {
        label = "width=" + std::to_string(width) + " lanes=" + std::to_string(lanes) +
            (edge ? " edge" : " wrap") + (linear ? " linear" : " point") + (nsa ? " NSA " : " contiguous ") +
            Name(offset) + " batch=" + std::to_string(batch);
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            const auto field = (lane + batch * 32u) % 64u;
            const auto poison = (0xa5c37fc0u ^ (lane * 0x01010100u)) & ~63u;
            const auto packed = offset == Offset::Signed ? field :
                offset == Offset::Poisoned ? field | poison : poison;
            input[GuardWords + lane * 4] = packed;
            input[GuardWords + lane * 4 + 1] = std::bit_cast<std::uint32_t>(
                (float(CoordinateIndex(lane, batch, width)) + 0.5f) / float(width));
            input[GuardWords + lane * 4 + 2] = 0x3f000000; // comparison/LOD rejection addresses remain defined
            input[GuardWords + lane * 4 + 3] = 0;
        }
        users = {
            std::uint32_t(Base + 0x10100), 0, lanes * 16u, 0x31016fac,
            std::uint32_t(Base + 0x20100), 0, lanes * 32u, 0x31016fac,
            std::uint32_t((Base + 0x30100) >> 8u), (77u << 20u) | (((width - 1u) & 3u) << 30u),
            (width - 1u) >> 2u, 0xfacu | (8u << 28u), 0, 0, 0, 0,
            std::uint32_t(edge ? 2u : 0u) | (2u << 3u) | (2u << 6u), 0,
            linear ? (1u << 20u) | (1u << 22u) : 0u, 0};
        const auto surface = AgcDriver::Graphics::DescribeSurface(
            AgcDriver::Graphics::DecodeTextureResource(std::span<const std::uint32_t>(users).subspan(8, 8)));
        Require(surface.mips.size() == 1 && surface.mips[0].height == 1 && surface.guestBytes % 4 == 0,
            label + " fixture surface is not a single logical 1D float mip");
        texture.assign(GuardWords * 2 + surface.guestBytes / 4, Sentinel);
        const auto start = GuardWords + surface.mips[0].tiledOffset / 4;
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t channel = 0; channel < 4; ++channel) texture[start + x * 4 + channel] = Texel(x, channel);
        const auto program = Program(offset, nsa);
        codeWords = program.size(); code.assign(GuardWords * 2 + codeWords, Sentinel);
        std::copy(program.begin(), program.end(), code.begin() + GuardWords);
        header.fill(std::byte{0xa5});
        std::fill_n(header.begin() + GuardBytes, sizeof(Shader) + sizeof(ShaderUserData), std::byte{0});
        Shader shader{}; shader.file_header = 0x34333231; shader.version = 0x18;
        shader.code = reinterpret_cast<const volatile void*>(Base + 0x40100);
        shader.user_data = reinterpret_cast<ShaderUserData*>(Base + 0x50100 + sizeof(Shader));
        shader.header_size = sizeof(Shader) + sizeof(ShaderUserData);
        shader.shader_size = std::uint32_t(codeWords * 4); shader.type = 0;
        std::memcpy(header.data() + GuardBytes, &shader, sizeof(shader));
        std::vector<std::uint32_t> pm4;
        const std::array<std::uint32_t, 3> threads{lanes, 1, 1};
        const std::array<std::uint32_t, 2> address{std::uint32_t((Base + 0x40100) >> 8u), 0};
        const std::array<std::uint32_t, 1> resources{40};
        Registers(pm4, 0x207, threads); Registers(pm4, 0x20c, address);
        Registers(pm4, 0x213, resources); Registers(pm4, 0x240, users);
        pm4.insert(pm4.end(), {0xc0031500, 1, 1, 1, 0x8041});
        commandWords = pm4.size(); commands.assign(GuardWords * 2 + commandWords, Sentinel);
        std::copy(pm4.begin(), pm4.end(), commands.begin() + GuardWords);
        packetBytes.fill(std::byte{0x5a});
        Packet packet{};
        packet.addr = reinterpret_cast<std::uint32_t*>(Base + 0x60100);
        packet.dw_num = std::uint32_t(commandWords);
        std::memcpy(packetBytes.data() + GuardBytes, &packet, sizeof(packet));
    }
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> Ranges() {
        return {{Base + 0x10000, std::as_writable_bytes(std::span(input)), false},
            {Base + 0x20000, std::as_writable_bytes(std::span(output)), true},
            {Base + 0x30000, std::as_writable_bytes(std::span(texture)), false},
            {Base + 0x40000, std::as_writable_bytes(std::span(code)), false},
            {Base + 0x50000, header, false},
            {Base + 0x60000, std::as_writable_bytes(std::span(commands)), false},
            {Base + 0x70000, packetBytes, false}};
    }
    void Run(id<MTLDevice> device, id<MTLLibrary> library) {
        const auto originalInput = input, originalTexture = texture, originalCode = code, originalCommands = commands;
        const auto originalHeader = header;
        const auto originalPacket = packetBytes;
        const auto originalUsers = users;
        auto expected = output;
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            const auto words = Expected(CoordinateIndex(lane, batch, width), input[GuardWords + lane * 4], offset, width, edge);
            std::copy(words.begin(), words.end(), expected.begin() + GuardWords + lane * 4);
        }
        AgcDriver::Metal::MetalDriver driver;
        const auto ranges = Ranges();
        driver.Configure((__bridge void*)device, (__bridge void*)library, ranges);
        try {
            driver.RegisterShader(reinterpret_cast<const Shader*>(Base + 0x50100));
            driver.Submit(reinterpret_cast<const Packet*>(Base + 0x70100), 0x20);
            // The production dispatch waits and requires commands.status ==
            // MTLCommandBufferStatusCompleted before copying any native output.
            // Driver WaitIdle rethrows a worker's status/error failure.
            driver.WaitIdle();
            for (std::size_t word = 0; word < output.size(); ++word)
                Require(output[word] == expected[word], "GATHER ORACLE " + label + " output word=" + std::to_string(word) +
                    " actual=" + Hex(output[word]) + " expected=" + Hex(expected[word]));
            Require(input == originalInput && texture == originalTexture && users == originalUsers,
                label + " read-only input/texels/descriptors/padding/guards changed");
            Require(code == originalCode && commands == originalCommands && header == originalHeader && packetBytes == originalPacket,
                label + " read-only shader/header/PM4/packet/padding/guards changed");
            driver.Shutdown();
        } catch (...) { driver.Shutdown(); throw; }
        std::cout << label << " PASS: production MTLCommandBufferStatusCompleted; exact all output words, untouched suffix and all read-only bytes/canaries\n";
    }
};

void Native(id<MTLDevice> device, id<MTLLibrary> library) {
    std::uint32_t count = 0;
    for (const auto width : {8u, 16u}) for (const auto lanes : {32u, 64u, 256u})
        for (const bool edge : {false, true}) for (const bool linear : {false, true})
            for (const bool nsa : {false, true})
                for (const auto offset : {Offset::None, Offset::Zero, Offset::Signed, Offset::Poisoned}) {
                    const auto batches = lanes == 32 && (offset == Offset::Signed || offset == Offset::Poisoned) ? 2u : 1u;
                    for (std::uint32_t batch = 0; batch < batches; ++batch) {
                        Replay(lanes, width, edge, linear, nsa, offset, batch).Run(device, library); ++count;
                    }
                }
    std::cout << "native PASS: " << count << " real AGC RDNA->SPIRV->MSL->Metal Completed dispatches on "
        << device.name.UTF8String << "; signed -32..31; poison bits6..31; green RGBA32Float; dyadic centres; width8/16; "
        << "wrap/edge; point/linear; contiguous/NSA; lanes32/64/256. PS5/vendor parity and title reachability unqualified.\n";
}
ShaderRecompiler::RecompileResult CompileGather(id<MTLDevice> device, std::span<const std::uint32_t> code,
    std::span<const std::uint32_t> users) {
    using namespace ShaderRecompiler;
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto limit = device.maxThreadsPerThreadgroup;
    const SpirvTarget target{0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {std::uint32_t(limit.width), std::uint32_t(limit.height), std::uint32_t(limit.depth)},
        std::uint32_t(limit.width), std::uint32_t(device.maxThreadgroupMemoryLength), {}, {}};
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<MemoryRegion, 1> regions{{{address, std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{32, 1, 1}, 0, {true, false, false}, false, 1};
    RecompileRequest request{{ShaderStage::Compute, address, code, 0, {}}, {32, 0, users, compute, {}, {}, regions}, target, {0, 0, 0, 128}};
    request.useCache = false;
    return Recompile(request);
}
void CompileRejected(id<MTLDevice> device, std::span<const std::uint32_t> code,
    std::span<const std::uint32_t> users, const char* name, const char* reason) {
    try { static_cast<void>(CompileGather(device, code, users)); }
    catch (const std::exception& error) {
        Require(std::string(error.what()).find(reason) != std::string::npos,
            std::string(name) + " rejected for unrelated reason: " + error.what());
        std::cout << name << " PASS: preserved rejection: " << error.what() << '\n'; return;
    }
    throw std::runtime_error(std::string(name) + " silently accepted unsupported gather");
}
void Rejections(id<MTLDevice> device) {
    Replay fixture(32, 8, true, false, false, Offset::Signed, 0);
    const auto originalUsers = fixture.users;
    for (const auto opcode : {0x40u, 0x44u, 0x61u}) {
        const auto code = Program(Offset::None, false, opcode);
        CompileRejected(device, code, fixture.users, opcode == 0x61 ? "horizontal" : "non-level-zero", "unsupported 1D gather variant");
    }
    const auto clamped = Program(Offset::None, false, 0x41);
    CompileRejected(device, clamped, fixture.users, "LOD-clamp", "gather with an LOD clamp");
    const auto comparison = Program(Offset::None, false, 0x4f);
    auto comparisonUsers = fixture.users;
    comparisonUsers[9] = (comparisonUsers[9] & ~(0x1ffu << 20u)) | (22u << 20u);
    // R32Float reaches EmitGatherOp's direct 1D dref prohibition. Color
    // comparison emulation accepts only 2D views and would reject earlier.
    CompileRejected(device, comparison, comparisonUsers, "comparison", "unsupported 1D gather variant");
    fixture.users[11] = 0xfacu | (12u << 28u);
    const auto array = Program(Offset::Signed, false, 0x57, 4);
    // 1D-array T#s decode now (shared driver: 1D array textures), so the gather compiles against a
    // 1D-array image shape instead of stopping at the descriptor boundary.
    const auto compiled = CompileGather(device, array, fixture.users);
    Require(std::any_of(compiled.bindings.begin(), compiled.bindings.end(), [](const auto& binding) {
        return binding.role == ShaderRecompiler::DescriptorRole::GuestImages &&
            binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image1DArray;
    }), "1D-array gather lost its 1D-array image shape");
    fixture.users = originalUsers;
    std::cout << "rejections PASS: non-level-zero/explicit-LOD/horizontal/LOD-clamp/comparison emitter guards; 1D-array gather compiles with a 1D-array shape; no unsupported native substitution\n";
}
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            Require(argc == 3, "Gather offset replay requires matching utility metallib path and native|reject mode");
            const std::string mode = argv[2];
            Require(mode == "native" || mode == "reject", "Unknown gather offset mode");
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Gather offset replay requires Metal 3");
            if (mode == "reject") { Rejections(device); return 0; }
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, error ? error.localizedDescription.UTF8String : "Cannot load genuine utility metallib");
            Native(device, library); return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
