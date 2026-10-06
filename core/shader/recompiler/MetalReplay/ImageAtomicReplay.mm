#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "MetalShaderResources.hpp"
#include "MetalShaderPipeline.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;
constexpr std::uint32_t GuardWords = 64, Sentinel = 0xdeadbeef;
constexpr std::uint64_t InputAddress = 0x100100, BufferAddress = 0x200100;
constexpr std::uint64_t TextureAddress = 0x300100, ReaderAddress = 0x400100;
void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<std::size_t N>
void CheckWords(const std::array<std::uint32_t, N>& actual, const std::array<std::uint32_t, N>& expected, const std::string& reason) {
    for (std::size_t i = 0; i < N; ++i) {
        Require(actual[i] == expected[i], reason + " at word " + std::to_string(i) + ", actual=" + std::to_string(actual[i]) + ", expected=" + std::to_string(expected[i]));
    }
}

constexpr std::array<std::uint32_t, 102> IntegerCode{
    0x34020084, 0x34060086, 0xe0301000, 0x80000401, 0xe0301004, 0x80000501, 0xe0301008, 0x80000601,
    0xbf8c3f70, 0x7e100300, 0x7e120280, 0x7e140304, 0xf03c0108, 0x00020a08, 0x7e160305, 0xf0482108,
    0x00020b08, 0x7e180280, 0xf0442108, 0x00020c08, 0xbf8c3f70, 0xe0701000, 0x80010b03, 0xe0701004,
    0x80010c03, 0x7e120281, 0x7e140304, 0xf03c0108, 0x00020a08, 0x7e160305, 0xf0502108, 0x00020b08,
    0x7e180280, 0xf0442108, 0x00020c08, 0xbf8c3f70, 0xe0701008, 0x80010b03, 0xe070100c, 0x80010c03,
    0x7e120282, 0x7e140304, 0xf03c0108, 0x00020a08, 0x7e160305, 0xf0582108, 0x00020b08, 0x7e180280,
    0xf0442108, 0x00020c08, 0xbf8c3f70, 0xe0701010, 0x80010b03, 0xe0701014, 0x80010c03, 0x7e120283,
    0x7e140304, 0xf03c0108, 0x00020a08, 0x7e160305, 0xf06c2108, 0x00020b08, 0x7e180280, 0xf0442108,
    0x00020c08, 0xbf8c3f70, 0xe0701018, 0x80010b03, 0xe070101c, 0x80010c03, 0x7e120284, 0x7e140304,
    0xf03c0108, 0x00020a08, 0x7e160305, 0xf0702108, 0x00020b08, 0x7e180280, 0xf0442108, 0x00020c08,
    0xbf8c3f70, 0xe0701020, 0x80010b03, 0xe0701024, 0x80010c03, 0x7e120285, 0x7e140304, 0xf03c0108,
    0x00020a08, 0x7e160305, 0x7e180306, 0xf0402308, 0x00020b08, 0x7e180280, 0xf0442108, 0x00020c08,
    0xbf8c3f70, 0xe0701028, 0x80010b03, 0xe070102c, 0x80010c03, 0xbf810000,
};

constexpr std::array<std::uint32_t, 84> FloatCode{
    0x34020084u, 0x34060086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xbe8f030bu, 0xbe8e030au, 0xbe8d0309u,
    0xbe8c0308u, 0xbe8b0307u, 0xbe8a0306u, 0xbe890305u, 0xbe880304u, 0x7e100300u, 0x7e120280u, 0x7e3c0304u,
    0xf03c0108u, 0x00021e08u, 0xbf8c3f70u, 0x7e140305u, 0xf0782108u, 0x00020a08u, 0xbf8c3f70u, 0x7e160280u,
    0xf0442108u, 0x00020b08u, 0xbf8c3f70u, 0x7e120281u, 0x7e3c0304u, 0xf03c0108u, 0x00021e08u, 0xbf8c3f70u,
    0x7e180305u, 0xf07c2108u, 0x00020c08u, 0xbf8c3f70u, 0x7e1a0280u, 0xf0442108u, 0x00020d08u, 0xbf8c3f70u,
    0x7e120282u, 0x7e3c0304u, 0xf03c0108u, 0x00021e08u, 0xbf8c3f70u, 0x7e1c0305u, 0x7e1e0306u, 0xf0742308u,
    0x00020e08u, 0xbf8c3f70u, 0x7e200280u, 0xf0442108u, 0x00021008u, 0xbf8c3f70u, 0x7e120283u, 0x7e3c0304u,
    0xf03c0108u, 0x00021e08u, 0xbf8c3f70u, 0x7e220305u, 0xf0780108u, 0x00021108u, 0xbf8c3f70u, 0x7e240280u,
    0xf0442108u, 0x00021208u, 0xbf8c3f70u, 0xe0701400u, 0x80000a03u, 0xe0701404u, 0x80000b03u, 0xe0701408u,
    0x80000c03u, 0xe070140cu, 0x80000d03u, 0xe0701410u, 0x80000e03u, 0xe0701414u, 0x80001003u, 0xe0701418u,
    0x80001103u, 0xe070141cu, 0x80001203u, 0xbf810000u,
};

constexpr std::array<std::array<std::uint32_t, 6>, 32> FloatCases{{
    {0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u, 0x80000000u, 0x80000000u},
    {0x80000000u, 0x00000000u, 0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x3f800000u, 0xbf800000u, 0xbf800000u, 0x3f800000u, 0x3f800000u, 0xbf800000u},
    {0xbf800000u, 0x3f800000u, 0xbf800000u, 0x3f800000u, 0xbf800000u, 0x3f800000u},
    {0x7f800000u, 0xff800000u, 0xff800000u, 0x7f800000u, 0x7f800000u, 0xff800000u},
    {0xff800000u, 0x7f800000u, 0xff800000u, 0x7f800000u, 0xff800000u, 0x7f800000u},
    {0x7fc00000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x7fc00000u, 0x7fc00000u},
    {0x3f800000u, 0x7fc00000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x7fc00000u},
    {0xffc00000u, 0xbf800000u, 0xbf800000u, 0xbf800000u, 0xffc00000u, 0xffc00000u},
    {0xbf800000u, 0xffc00000u, 0xbf800000u, 0xbf800000u, 0xbf800000u, 0xffc00000u},
    {0x7f800001u, 0x3f800000u, 0x7fc00001u, 0x7fc00001u, 0x7f800001u, 0x7f800001u},
    {0x3f800000u, 0x7f800001u, 0x7fc00001u, 0x7fc00001u, 0x3f800000u, 0x7f800001u},
    {0xff800001u, 0xbf800000u, 0xffc00001u, 0xffc00001u, 0xff800001u, 0xff800001u},
    {0xbf800000u, 0xff800001u, 0xffc00001u, 0xffc00001u, 0xbf800000u, 0xff800001u},
    {0x7fa00000u, 0x3f800000u, 0x7fe00000u, 0x7fe00000u, 0x7fa00000u, 0x7fa00000u},
    {0x3f800000u, 0x7fa00000u, 0x7fe00000u, 0x7fe00000u, 0x3f800000u, 0x7fa00000u},
    {0x00000001u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u, 0x00000000u},
    {0x00000000u, 0x00000001u, 0x00000000u, 0x00000001u, 0x00000000u, 0x00000001u},
    {0x80000001u, 0x80000000u, 0x80000001u, 0x80000000u, 0x80000001u, 0x80000000u},
    {0x80000000u, 0x80000001u, 0x80000001u, 0x80000000u, 0x80000000u, 0x80000001u},
    {0x007fffffu, 0x00800000u, 0x007fffffu, 0x00800000u, 0x007fffffu, 0x00800000u},
    {0x00800000u, 0x007fffffu, 0x007fffffu, 0x00800000u, 0x00800000u, 0x007fffffu},
    {0x40490fdbu, 0xc0490fdbu, 0xc0490fdbu, 0x40490fdbu, 0x40490fdbu, 0xc0490fdbu},
    {0xc0490fdbu, 0x40490fdbu, 0xc0490fdbu, 0x40490fdbu, 0xc0490fdbu, 0x40490fdbu},
    {0x7f7fffffu, 0x7f800000u, 0x7f7fffffu, 0x7f800000u, 0x7f7fffffu, 0x7f800000u},
    {0x7f800000u, 0x7f7fffffu, 0x7f7fffffu, 0x7f800000u, 0x7f800000u, 0x7f7fffffu},
    {0x7fc00001u, 0x3f800001u, 0x3f800001u, 0x3f800001u, 0x7fc00001u, 0x7fc00001u},
    {0x3f800001u, 0x7fc00001u, 0x3f800001u, 0x3f800001u, 0x3f800001u, 0x7fc00001u},
    {0x00000001u, 0x80000001u, 0x80000001u, 0x00000001u, 0x00000001u, 0x80000001u},
    {0x80000001u, 0x00000001u, 0x80000001u, 0x00000001u, 0x80000001u, 0x00000001u},
    {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u},
}};

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t bytes) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), bytes, 0x01016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    return {static_cast<std::uint32_t>(TextureAddress >> 8u),
        (format << 20u) | (((width - 1u) & 3u) << 30u),
        ((width - 1u) >> 2u) | ((height - 1u) << 14u), 0xfacu | (9u << 28u), 0, 0, 0, 0};
}

std::vector<std::uint32_t> UserData(std::uint64_t inputAddress, std::uint32_t inputBytes,
    std::uint32_t outputBytes, std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    std::vector<std::uint32_t> words(16);
    const auto input = BufferDescriptor(inputAddress, inputBytes);
    const auto output = BufferDescriptor(BufferAddress, outputBytes);
    const auto texture = TextureDescriptor(width, height, format);
    std::copy(input.begin(), input.end(), words.begin());
    std::copy(output.begin(), output.end(), words.begin() + 4);
    std::copy(texture.begin(), texture.end(), words.begin() + 8);
    return words;
}

SpirvTarget Target(id<MTLDevice> device) {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto maximum = device.maxThreadsPerThreadgroup;
    return {0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height),
         static_cast<std::uint32_t>(maximum.depth)}, static_cast<std::uint32_t>(maximum.width),
        static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
}

MetalBackend::Result Compile(id<MTLDevice> device, std::span<const std::uint32_t> code,
    std::span<const std::uint32_t> users, std::uint32_t waveSize) {
    const std::array<MemoryRegion, 1> memory{{{0x500000, std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{waveSize, 1, 1}, 0, {false, false, false}, false, 1, {}};
    RecompileRequest request{{ShaderStage::Compute, 0x500000, code, 0, {}},
        {waveSize, 0, users, compute, {}, {}, memory}, Target(device), {0, 0, 0, 128}, {}, false};
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true;
    options.supportsGpuAddresses = true;
    options.supportsSimdGroups = true;
    return MetalBackend::ConvertToMetal(Recompile(request), ShaderStage::Compute, options);
}

std::vector<std::uint32_t> ReaderCode(std::uint32_t rows) {
    std::vector<std::uint32_t> words{0x34060086, 0x7e100300};
    for (std::uint32_t row = 0; row < rows; ++row) {
        const std::array<std::uint32_t, 6> read{
            0x7e120280u + row, 0xf0000108, 0x00020c08, 0xbf8c3f70,
            0xe0701000u + row * 4u, 0x80010c03};
        words.insert(words.end(), read.begin(), read.end());
    }
    words.push_back(0xbf810000);
    return words;
}

struct ReplayMemory {
    std::array<std::uint32_t, 64 * 4 + 2 * GuardWords> input;
    std::array<std::uint32_t, 64 * 20 + 2 * GuardWords> buffer;
    std::array<std::uint32_t, 4096 + 2 * GuardWords> texture;
    std::array<std::uint32_t, 64 * 16 + 2 * GuardWords> reader;
    ReplayMemory() {
        input.fill(Sentinel);
        buffer.fill(Sentinel);
        texture.fill(Sentinel);
        reader.fill(Sentinel);
    }
    auto Ranges() {
        using Range = AgcDriver::NativeGuestMemory::BorrowedRange;
        return std::array<Range, 4>{{
            {InputAddress - 256, std::as_writable_bytes(std::span(input)), false},
            {BufferAddress - 256, std::as_writable_bytes(std::span(buffer)), true},
            {TextureAddress - 256, std::as_writable_bytes(std::span(texture)), true},
            {ReaderAddress - 256, std::as_writable_bytes(std::span(reader)), true}}};
    }
};

void Execute(const AgcDriver::Metal::MetalDevice& backend, ReplayMemory& memory,
    std::span<const std::uint32_t> code, std::vector<std::uint32_t> users,
    std::uint32_t waveSize, std::uint32_t rows, std::uint32_t textureWord = 8) {
    const auto atomic = Compile(backend.Device(), code, users, waveSize);
    std::array<std::uint32_t, 8> texture;
    std::copy_n(users.begin() + textureWord, 8, texture.begin());
    const auto output = BufferDescriptor(ReaderAddress, 64 * 16 * 4);
    std::copy(output.begin(), output.end(), users.begin() + 4);
    std::copy(texture.begin(), texture.end(), users.begin() + 8);
    const auto reader = Compile(backend.Device(), ReaderCode(rows), users, waveSize);
    AgcDriver::Metal::MetalComputePipeline atomicPipeline(backend.Device(), atomic);
    AgcDriver::Metal::MetalComputePipeline readerPipeline(backend.Device(), reader);
    const auto ranges = memory.Ranges();
    AgcDriver::Metal::MetalShaderResources resources(backend, ranges);
    const auto atomicBindings = resources.Bindings(atomic);
    const auto readerBindings = resources.Bindings(reader);
    auto commands = backend.CommandBuffer();
    const auto grid = [](const MetalBackend::Result& shader) {
        return MTLSizeMake(shader.threadsPerThreadgroup[0], shader.threadsPerThreadgroup[1], shader.threadsPerThreadgroup[2]);
    };
    atomicPipeline.Encode(commands, atomicBindings, grid(atomic), {}, resources.Residency());
    readerPipeline.Encode(commands, readerBindings, grid(reader), {}, resources.Residency());
    [commands commit];
    backend.Wait(commands);
    Require(resources.Complete(commands).state == BdaAbi::FaultState::Empty,
        "Original image atomics published a guest GPU fault");
}

void PublicExecute(const AgcDriver::Metal::MetalDevice& backend, id<MTLLibrary> library,
    ReplayMemory& memory, std::span<const std::uint32_t> users, const std::function<void(bool)>& check) {
    constexpr std::uint64_t CodeAddress = 0x500000, RawCodeAddress = 0x510000, CommandsAddress = 0x600000, PacketAddress = 0x610000, HeaderAddress = 0x620000;
    auto code = IntegerCode;
    auto rawCode = IntegerCode;
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> headerBytes{};
    Shader header{};
    header.file_header = 0x34333231;
    header.version = 0x18;
    header.code = reinterpret_cast<const volatile void*>(CodeAddress);
    header.user_data = reinterpret_cast<ShaderUserData*>(HeaderAddress + sizeof(Shader));
    header.header_size = headerBytes.size();
    header.shader_size = sizeof(IntegerCode);
    std::memcpy(headerBytes.data(), &header, sizeof(header));
    const auto originalHeader = headerBytes;
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> threads{32, 1, 1};
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(CodeAddress >> 8u), 0};
    const std::array<std::uint32_t, 1> resources{static_cast<std::uint32_t>(users.size()) << 1u};
    registers(0x207, threads);
    const auto programWord = words.size() + 2;
    registers(0x20c, program);
    registers(0x213, resources);
    registers(0x240, users);
    const std::array<std::uint32_t, 5> dispatch{0xc0031500, 1, 1, 1, 0x8041};
    words.insert(words.end(), dispatch.begin(), dispatch.end());
    std::array<std::uint32_t, 256> commands;
    commands.fill(Sentinel);
    std::copy(words.begin(), words.end(), commands.begin());
    ::Packet packet{reinterpret_cast<std::uint32_t*>(CommandsAddress), static_cast<std::uint32_t>(words.size()), 0, {}};
    const auto memoryRanges = memory.Ranges();
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges(memoryRanges.begin(), memoryRanges.end());
    ranges.push_back({HeaderAddress, headerBytes, false});
    ranges.push_back({CodeAddress, std::as_writable_bytes(std::span(code)), false});
    ranges.push_back({RawCodeAddress, std::as_writable_bytes(std::span(rawCode)), false});
    ranges.push_back({CommandsAddress, std::as_writable_bytes(std::span(commands)), false});
    ranges.push_back({PacketAddress, std::as_writable_bytes(std::span(&packet, 1)), false});
    auto& driver = AgcDriver::Metal::MetalDriver::Get();
    driver.Configure((__bridge void*)backend.Device(), (__bridge void*)library, ranges);
    try {
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(HeaderAddress));
        for (const bool registered : {true, false}) {
            memory.buffer.fill(Sentinel);
            memory.texture.fill(Sentinel);
            memory.reader.fill(Sentinel);
            commands[programWord] = static_cast<std::uint32_t>((registered ? CodeAddress : RawCodeAddress) >> 8u);
            const auto originalCommands = commands;
            AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0);
            AgcDriverWaitIdle_nid_postfix();
            check(registered);
            CheckWords(commands, originalCommands, "Public AGC image atomic execution changed command words or guards");
        }
        driver.Shutdown();
    } catch (...) {
        try { driver.Shutdown(); } catch (...) {}
        throw;
    }
    Require(headerBytes == originalHeader, "Public AGC image atomic execution changed its shader header");
    CheckWords(code, IntegerCode, "Public AGC image atomic execution changed borrowed shader code");
    CheckWords(rawCode, IntegerCode, "Public AGC raw capture changed borrowed shader code");
}

void IntegerReplay(const AgcDriver::Metal::MetalDevice& backend, std::uint32_t format, id<MTLLibrary> publicLibrary = nil) {
    ReplayMemory memory;
    constexpr std::array<std::array<std::uint32_t, 2>, 12> edges{{
        {0, 0}, {0, 5}, {5, 0}, {5, 5}, {4, 5}, {6, 5}, {0x7fffffff, 0x80000000},
        {0x80000000, 0x7fffffff}, {0xffffffff, 1}, {1, 0xffffffff}, {0xffffffff, 0xffffffff}, {0xfffffffe, 0xffffffff}}};
    std::uint64_t random = 0x9e3779b97f4a7c15ull;
    auto expectedBuffer = memory.buffer;
    auto expectedTexture = memory.texture;
    auto expectedReader = memory.reader;
    for (std::uint32_t lane = 0; lane < 32; ++lane) {
        auto* input = memory.input.data() + GuardWords + lane * 4;
        for (std::uint32_t j = 0; j < 3; ++j) {
            random = random * 6364136223846793005ull + 1442695040888963407ull;
            input[j] = static_cast<std::uint32_t>(random >> 32u);
        }
        if (lane < edges.size()) std::copy(edges[lane].begin(), edges[lane].end(), input);
        if ((lane & 1u) != 0) input[2] = input[0];
        const auto a = input[0], b = input[1], c = input[2];
        const auto sa = std::bit_cast<std::int32_t>(a), sb = std::bit_cast<std::int32_t>(b);
        const std::array<std::uint32_t, 6> values{a - b,
            std::bit_cast<std::uint32_t>(std::min(sa, sb)), std::bit_cast<std::uint32_t>(std::max(sa, sb)),
            a >= b ? 0u : a + 1u, a == 0 || a > b ? b : a - 1u, a == c ? b : a};
        for (std::uint32_t row = 0; row < values.size(); ++row) {
            expectedBuffer[GuardWords + lane * 16 + row * 2] = a;
            expectedBuffer[GuardWords + lane * 16 + row * 2 + 1] = values[row];
            expectedTexture[GuardWords + row * 64 + lane] = values[row];
            expectedReader[GuardWords + lane * 16 + row] = values[row];
        }
    }
    const auto originalInput = memory.input;
    const auto users = UserData(InputAddress, 32 * 4 * 4, 32 * 16 * 4, 32, 8, format);
    if (publicLibrary != nil) expectedReader.fill(Sentinel);
    const auto check = [&](bool registered) {
        Require(memory.input == originalInput, "Original integer image atomics modified read-only inputs or guards");
        CheckWords(memory.buffer, expectedBuffer, "Original integer image atomics returned wrong old/final values or changed output padding");
        CheckWords(memory.texture, expectedTexture, "Original integer image atomics guest writeback changed texels, padding or guards");
        CheckWords(memory.reader, expectedReader, "Original typed RDNA image_load did not see the same atomic backing bits");
        const auto label = publicLibrary == nil ? "Integer image atomics and coherent original typed image_load" :
            registered ? "Public AGC registered RDNA image atomics" : "Public AGC raw RDNA image atomics with MIMG capture growth";
        std::cout << label << " format=" << format << " passed\n";
    };
    if (publicLibrary != nil) PublicExecute(backend, publicLibrary, memory, users, check);
    else {
        Execute(backend, memory, IntegerCode, users, 32, 6);
        check(false);
    }
}

void FloatReplay(const AgcDriver::Metal::MetalDevice& backend, std::uint32_t format, std::uint32_t waveSize, bool compareInitial) {
    ReplayMemory memory;
    auto expectedBuffer = memory.buffer;
    auto expectedTexture = memory.texture;
    auto expectedReader = memory.reader;
    for (std::uint32_t lane = 0; lane < waveSize; ++lane) {
        const auto& row = FloatCases[lane % FloatCases.size()];
        const bool initial = waveSize == 64 ? lane >= 32 : compareInitial;
        const auto inputOffset = GuardWords + lane * 4;
        memory.buffer[inputOffset] = row[0];
        memory.buffer[inputOffset + 1] = row[1];
        memory.buffer[inputOffset + 2] = initial ? row[0] : row[1];
        memory.buffer[inputOffset + 3] = 0;
        std::copy_n(memory.buffer.begin() + inputOffset, 4, expectedBuffer.begin() + inputOffset);
        const auto cmp = row[initial ? 5 : 4];
        const std::array<std::uint32_t, 8> values{row[0], row[2], row[0], row[3], row[0], cmp, row[1], row[2]};
        std::copy(values.begin(), values.end(), expectedBuffer.begin() + GuardWords + 64 * 4 + lane * 16);
        const std::array<std::uint32_t, 4> texels{row[2], row[3], cmp, row[2]};
        for (std::uint32_t y = 0; y < texels.size(); ++y) {
            expectedTexture[GuardWords + y * 64 + lane] = texels[y];
            expectedReader[GuardWords + lane * 16 + y] = texels[y];
        }
    }
    const auto input = memory.input;
    auto users = UserData(BufferAddress, 64 * 20 * 4, 0, 64, 4, format);
    const auto texture = TextureDescriptor(64, 4, format);
    std::copy(texture.begin(), texture.end(), users.begin() + 4);
    Execute(backend, memory, FloatCode, users, waveSize, 4, 4);
    Require(memory.input == input, "Float image atomics changed an unrelated borrow");
    CheckWords(memory.buffer, expectedBuffer, "Original float image atomics changed IEEE old/final bits, no-glc value, inputs or padding");
    CheckWords(memory.texture, expectedTexture, "Original float image atomics IEEE texel writeback or guards are incorrect");
    CheckWords(memory.reader, expectedReader, "Original typed image_load lost float atomic IEEE bits before guest copyback");
    std::cout << "Float IEEE image atomics and coherent typed image_load format=" << format << " wave=" << waveSize << " compare=" << (waveSize == 64 ? "both" : compareInitial ? "initial" : "source") << " passed\n";
}

}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            Require(argc == 2, "Image atomic replay requires the utility metallib path");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Image atomic replay requires a Metal 3 device");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, std::string("Image atomic utility library load failed: ") + (error.localizedDescription.UTF8String ?: "unknown error"));
            AgcDriver::Metal::MetalDevice backend(device, library);
            for (const auto format : {20u, 21u, 22u}) {
                IntegerReplay(backend, format);
                FloatReplay(backend, format, 32, false);
                FloatReplay(backend, format, 32, true);
                FloatReplay(backend, format, 64, false);
            }
            IntegerReplay(backend, 21, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
