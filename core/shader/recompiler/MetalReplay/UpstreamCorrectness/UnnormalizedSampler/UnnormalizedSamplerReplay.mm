#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "SceShaders.hpp"
#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include "ShaderDiskCache.hpp"
#include "MetalBackend/MetalShaderBridge.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "SpirvBackend/SpirvEmitter.hpp"
#include "Optimization/BindingAllocator.hpp"
#include "Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/ShaderInfoCollector.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "Translation/ShaderInputInfoBuilder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderResources.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <filesystem>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
extern char** environ;

// Test-audit authoring contract: the public AGC boundary must preserve native
// pixel addressing, filtering, paired sampler identity and read-only bytes.
// A normalized native sampler, stale descriptor memo, lost cache certificate,
// wrong NSA address or incorrect copyback makes exact independent results fail.
// Existing normalized gather/minLOD fixtures do not exercise this contract.
// No test-only production seam, handwritten MSL or CPU device-output writes.
namespace {
using namespace ShaderRecompiler;
constexpr std::uint32_t GuardWords = 64, GuardBytes = 256, Sentinel = 0xdeadbeef;
constexpr std::uint64_t Base = 0x1800000;
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::string Hex(std::uint32_t word) { char text[16]; std::snprintf(text, sizeof(text), "0x%08x", word); return text; }
std::uint64_t Hash(std::span<const std::uint32_t> words) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto word : words) for (unsigned byte = 0; byte < 4; ++byte) { hash ^= (word >> (byte * 8)) & 255; hash *= 1099511628211ull; }
    return hash;
}
int FloorQuarter(int q) { return q >= 0 ? q / 4 : -((-q + 3) / 4); }
int CoordinateQuarter(std::uint32_t lane, std::uint32_t extent, bool y) {
    const int last = int(extent) * 4;
    const std::array<int, 16> values{-9, -3, -1, 0, 1, 2, 3, 5, 7, last - 5, last - 3,
        last - 1, last, last + 1, last + 3, last + 9};
    // Distinct wave coordinate schedules, with both axes permuted independently.
    return values[(lane * (y ? 5u : 1u) + (lane / 32u) * (y ? 7u : 3u)) % values.size()];
}
std::uint32_t TexelInteger(int x, int y, std::uint32_t channel) { return 64u + std::uint32_t(x) * 8u + std::uint32_t(y) * 64u + channel * 2048u; }
std::array<std::uint32_t, 4> Expected(int qx, int qy, std::uint32_t width, std::uint32_t height, bool linear, bool edge) {
    const auto addressed = [&](int x, int y, std::uint32_t c) -> std::uint32_t {
        if (!edge && (x < 0 || y < 0 || x >= int(width) || y >= int(height))) return 0;
        return TexelInteger(std::clamp(x, 0, int(width) - 1), std::clamp(y, 0, int(height) - 1), c);
    };
    std::array<std::uint32_t, 4> out{};
    for (std::uint32_t c = 0; c < 4; ++c) {
        float value;
        if (!linear) value = float(addressed(FloorQuarter(qx), height == 1 ? 0 : FloorQuarter(qy), c));
        else {
            const int x = FloorQuarter(qx - 2), rx = qx - 2 - 4 * x;
            const int y = height == 1 ? 0 : FloorQuarter(qy - 2), ry = height == 1 ? 0 : qy - 2 - 4 * y;
            // Integer bilinear numerator /16. Values and weights are dyadic and
            // exactly representable in f32; no production filtering code used.
            const auto numerator = addressed(x, y, c) * std::uint32_t((4 - rx) * (4 - ry)) +
                addressed(x + 1, y, c) * std::uint32_t(rx * (4 - ry)) +
                addressed(x, y + (height != 1), c) * std::uint32_t((4 - rx) * ry) +
                addressed(x + 1, y + (height != 1), c) * std::uint32_t(rx * ry);
            value = float(numerator) / 16.0f;
        }
        out[c] = std::bit_cast<std::uint32_t>(value);
    }
    return out;
}
void Registers(std::vector<std::uint32_t>& words, std::uint32_t address, std::span<const std::uint32_t> values) {
    words.push_back(0xc0007600u | (std::uint32_t(values.size()) << 16u)); words.push_back(address);
    words.insert(words.end(), values.begin(), values.end());
}
std::vector<std::uint32_t> Program(bool twoD, bool nsa, bool mixed, std::uint32_t opcode = 0x24) {
    // v0 is lane, v1 byte offset; public V# loads supply coordinates and LOD.
    std::vector<std::uint32_t> words{mixed ? 0x34020085u : 0x34020084u, 0xe0381000, 0x80000201};
    if (mixed) words.insert(words.end(), {0xe0381010, 0x80000a01});
    words.push_back(0xbf8c3f70);
    const auto sample = [&](std::uint32_t source, std::uint32_t destination, std::uint32_t sampler) {
        const auto address = nsa ? 20u : source;
        if (nsa) {
            words.push_back(0x7e280300u | source); words.push_back(0x7e2c0300u | (source + 1));
            words.push_back(0x7e300300u | (source + 2));
            // Poison contiguous followers: only the independently specified NSA
            // register list can read the live y/LOD in this case.
            words.insert(words.end(), {0x7e2a02ff, 0x7fc00001, 0x7e2e02ff, 0x7fc00002});
        }
        words.push_back(0xf0000f00u | ((opcode & 0x7fu) << 18u) | (opcode & 0x80u ? 1u : 0u) | (twoD ? 8u : 0u) | (nsa ? 2u : 0u));
        words.push_back((sampler / 4u << 21u) | (2u << 16u) | (destination << 8u) | address);
        if (nsa) words.push_back(0x1c1a1816); // vaddr20, then v22/v24/v26/v28
    };
    sample(2, 6, 16);
    if (mixed) sample(10, 14, 20);
    words.insert(words.end(), {0xbf8c3f70, 0xe0781000, 0x80010601});
    if (mixed) words.insert(words.end(), {0xe0781010, 0x80010e01});
    words.push_back(0xbf810000); return words;
}
struct Replay {
    std::uint32_t lanes, width, height, stride;
    bool edge, linear, nsa, mixed, pixelFirst, secondOutput;
    bool firstOutput = true;
    std::vector<std::uint32_t> input, output, texture, code, commands, users;
    std::array<std::byte, GuardBytes * 2 + sizeof(Shader) + sizeof(ShaderUserData)> header{};
    std::array<std::byte, GuardBytes * 2 + sizeof(Packet)> packetBytes{};
    std::size_t codeWords = 0, commandWords = 0;
    std::string label;
    Replay(std::uint32_t lanes, std::uint32_t width, std::uint32_t height, bool edge,
        bool linear, bool nsa, bool pixel = true, bool mixed = false) : lanes(lanes), width(width), height(height),
        stride(mixed ? 8u : 4u), edge(edge), linear(linear), nsa(nsa), mixed(mixed), pixelFirst(pixel), secondOutput(mixed),
        input(GuardWords * 2 + lanes * stride, Sentinel), output(GuardWords * 2 + lanes * stride * 2, Sentinel) {
        label = std::string(height == 1 ? "1D" : "2D") + " " + std::to_string(width) + "x" + std::to_string(height) +
            " lanes=" + std::to_string(lanes) + (edge ? " edge" : " zero") + (linear ? " linear" : " point") +
            (nsa ? " NSA" : " contiguous") + (mixed ? (pixel ? " pixel/normalized" : " normalized/pixel") : pixel ? " pixel" : " normalized");
        users = {std::uint32_t(Base + 0x10100), 0, lanes * stride * 4u, 0x01016fac,
            std::uint32_t(Base + 0x20100), 0, lanes * stride * 8u, 0x01016fac,
            std::uint32_t((Base + 0x30100) >> 8u), (77u << 20u) | (((width - 1u) & 3u) << 30u),
            ((width - 1u) >> 2u) | ((height - 1u) << 14u), 0xfacu | ((height == 1 ? 8u : 9u) << 28u), 0, 0, 0, 0};
        for (std::uint32_t pair = 0; pair < (mixed ? 2u : 1u); ++pair) {
            const bool policy = pair == 0 ? pixel : !pixel;
            const auto clamp = edge ? 2u : 6u;
            users.insert(users.end(), {clamp | (clamp << 3u) | (2u << 6u) | (policy ? 1u << 15u : 0u), 0,
                linear ? (1u << 20u) | (1u << 22u) : 0u, 0});
        }
        SetInputs();
        const auto surface = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(std::span(users).subspan(8, 8)));
        Require(surface.mips.size() == 1 && surface.guestBytes % 4 == 0, "Invalid independent single-mip fixture surface");
        texture.assign(GuardWords * 2 + surface.guestBytes / 4, Sentinel);
        const auto& mip = surface.mips[0];
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t c = 0; c < 4; ++c)
                texture[GuardWords + mip.tiledOffset / 4 + y * mip.pitchBytes / 4 + x * 4 + c] = std::bit_cast<std::uint32_t>(float(TexelInteger(x, y, c)));
        const auto program = Program(height != 1, nsa, mixed);
        codeWords = program.size(); code.assign(GuardWords * 2 + codeWords, Sentinel);
        std::copy(program.begin(), program.end(), code.begin() + GuardWords);
        header.fill(std::byte{0xa5}); std::fill_n(header.begin() + GuardBytes, sizeof(Shader) + sizeof(ShaderUserData), std::byte{0});
        Shader shader{}; shader.file_header = 0x34333231; shader.version = 0x18;
        shader.code = reinterpret_cast<const volatile void*>(Base + 0x40100);
        shader.user_data = reinterpret_cast<ShaderUserData*>(Base + 0x50100 + sizeof(Shader));
        shader.header_size = sizeof(Shader) + sizeof(ShaderUserData); shader.shader_size = std::uint32_t(codeWords * 4);
        shader.type = 0; std::memcpy(header.data() + GuardBytes, &shader, sizeof(shader));
        RebuildCommands();
    }
    void SetInputs() {
        for (std::uint32_t lane = 0; lane < lanes; ++lane) for (std::uint32_t pair = 0; pair < (mixed ? 2u : 1u); ++pair) {
            const bool pixel = (users[16 + pair * 4] & (1u << 15u)) != 0;
            auto* values = input.data() + GuardWords + lane * stride + pair * 4;
            values[0] = std::bit_cast<std::uint32_t>(float(CoordinateQuarter(lane, width, false)) / 4.0f / (pixel ? 1.0f : float(width)));
            values[1] = height == 1 ? 0 : std::bit_cast<std::uint32_t>(float(CoordinateQuarter(lane, height, true)) / 4.0f / (pixel ? 1.0f : float(height)));
            values[2] = values[3] = 0;
        }
    }
    void RebuildCommands() {
        std::vector<std::uint32_t> pm4;
        const std::array<std::uint32_t, 3> threads{lanes, 1, 1};
        const std::array<std::uint32_t, 2> address{std::uint32_t((Base + 0x40100) >> 8u), 0};
        const std::array<std::uint32_t, 1> resources{std::uint32_t(users.size()) * 2u};
        Registers(pm4, 0x207, threads); Registers(pm4, 0x20c, address); Registers(pm4, 0x213, resources); Registers(pm4, 0x240, users);
        pm4.insert(pm4.end(), {0xc0031500, 1, 1, 1, 0x8041}); commandWords = pm4.size();
        commands.assign(GuardWords * 2 + commandWords, Sentinel); std::copy(pm4.begin(), pm4.end(), commands.begin() + GuardWords);
        packetBytes.fill(std::byte{0x5a}); Packet packet{}; packet.addr = reinterpret_cast<std::uint32_t*>(Base + 0x60100);
        packet.dw_num = std::uint32_t(commandWords); std::memcpy(packetBytes.data() + GuardBytes, &packet, sizeof(packet));
    }
    void SetProgram(std::span<const std::uint32_t> program) {
        codeWords = program.size(); code.assign(GuardWords * 2 + codeWords, Sentinel);
        std::copy(program.begin(),program.end(),code.begin()+GuardWords);
        Shader shader; std::memcpy(&shader,header.data()+GuardBytes,sizeof(shader));
        shader.shader_size = std::uint32_t(codeWords*4); std::memcpy(header.data()+GuardBytes,&shader,sizeof(shader));
    }
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> Ranges() {
        return {{Base + 0x10000, std::as_writable_bytes(std::span(input)), false},
            {Base + 0x20000, std::as_writable_bytes(std::span(output)), true},
            {Base + 0x30000, std::as_writable_bytes(std::span(texture)), false},
            {Base + 0x40000, std::as_writable_bytes(std::span(code)), false}, {Base + 0x50000, header, false},
            {Base + 0x60000, std::as_writable_bytes(std::span(commands)), false}, {Base + 0x70000, packetBytes, false}};
    }
    void RunAction(const std::function<void()>& execute) {
        std::fill(output.begin(), output.end(), Sentinel);
        const auto originalInput = input, originalTexture = texture, originalCode = code, originalCommands = commands, originalUsers = users;
        const auto originalHeader = header;
        const auto originalPacket = packetBytes;
        auto expected = output;
        for (std::uint32_t lane = 0; lane < lanes; ++lane) for (std::uint32_t pair = 0; pair < (secondOutput ? 2u : 1u); ++pair) {
            if (pair == 0 && !firstOutput) continue;
            const auto values = Expected(CoordinateQuarter(lane, width, false), CoordinateQuarter(lane, height, true), width, height, linear, edge);
            std::copy(values.begin(), values.end(), expected.begin() + GuardWords + lane * stride + pair * 4);
        }
        execute();
        // Both native paths wait for Completed before production resource copyback.
        for (std::size_t word = 0; word < output.size(); ++word)
            Require(output[word] == expected[word], "PIXEL ORACLE " + label + " word=" + std::to_string(word) + " actual=" + Hex(output[word]) + " expected=" + Hex(expected[word]));
        Require(input == originalInput && texture == originalTexture && users == originalUsers, label + " read-only input/texels/descriptors/padding/canaries changed");
        Require(code == originalCode && commands == originalCommands && header == originalHeader && packetBytes == originalPacket,
            label + " read-only code/header/PM4/Packet/padding/canaries changed");
        std::cout << label << " PASS Completed outputWords=" << output.size() << " sampledWords=" << lanes * 4u * (unsigned(firstOutput) + unsigned(secondOutput))
            << " outputHash=" << Hash(output) << " untouchedReadOnlyBytes=" << input.size()*4 + texture.size()*4 + code.size()*4 + commands.size()*4 + header.size() + packetBytes.size() << '\n';
    }
    void RunConfigured(AgcDriver::Metal::MetalDriver& driver, bool registerShader = true) {
        RunAction([&] {
            if (registerShader) driver.RegisterShader(reinterpret_cast<const Shader*>(Base + 0x50100));
            driver.Submit(reinterpret_cast<const Packet*>(Base + 0x70100), 0x20); driver.WaitIdle();
        });
    }
    void Run(id<MTLDevice> device, id<MTLLibrary> library) {
        AgcDriver::Metal::MetalDriver driver; const auto ranges = Ranges();
        driver.Configure((__bridge void*)device, (__bridge void*)library, ranges);
        try { RunConfigured(driver); driver.Shutdown(); } catch (...) { driver.Shutdown(); throw; }
    }
};
SpirvTarget Target(id<MTLDevice> device) {
    static constexpr std::array<std::uint32_t, 3> capabilities{spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto limit = device.maxThreadsPerThreadgroup;
    return {0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {std::uint32_t(limit.width), std::uint32_t(limit.height), std::uint32_t(limit.depth)}, std::uint32_t(limit.width), std::uint32_t(device.maxThreadgroupMemoryLength), {}, {}};
}
RecompileRequest Request(id<MTLDevice> device, std::span<const std::uint32_t> code, std::span<const std::uint32_t> users,
    std::span<const MemoryRegion> regions, bool cache) {
    const ShaderComputeStageInfo compute{{32, 1, 1}, 0, {true, false, false}, false, 1};
    RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, users, compute, {}, {}, regions}, Target(device), {0, 0, 0, 128}};
    request.useCache = cache; return request;
}
RecompileResult Compile(id<MTLDevice> device, std::span<const std::uint32_t> code, std::span<const std::uint32_t> users) {
    const std::array<MemoryRegion, 1> regions{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    return Recompile(Request(device, code, users, regions, false));
}
void Reject(const std::string& name, std::string_view reason, const std::function<void()>& action) {
    try { action(); } catch (const std::exception& error) {
        Require(std::string(error.what()).find(reason) != std::string::npos, name + " WRONG OWNER/REASON " + error.what());
        std::cout << name << " PASS owner refusal: " << error.what() << '\n'; return;
    }
    throw std::runtime_error(name + " accepted an unsupported pixel sampler");
}
void Native(id<MTLDevice> device, id<MTLLibrary> library) {
    std::uint32_t runs = 0;
    for (const bool twoD : {false, true}) for (const auto width : {8u, 16u}) for (const auto lanes : {32u, 64u, 256u})
        for (const bool linear : {false, true}) for (const bool edge : {false, true}) for (const bool nsa : {false, true}) {
            Replay(lanes, width, twoD ? width / 2 : 1, edge, linear, nsa).Run(device, library); ++runs;
        }
    std::cout << "native PASS dispatches=" << runs << " AGC PM4/capture->production Recompile->SPIRV->MSL->Metal->guest copyback; independent exact RGBA f32 oracle; no title/PS5 parity claim\n";
}
void Mixed(id<MTLDevice> device, id<MTLLibrary> library) {
    std::uint32_t runs = 0;
    for (const bool twoD : {false, true}) for (const bool linear : {false, true}) for (const bool edge : {false, true})
        for (const bool nsa : {false, true}) {
            Replay(64, 16, twoD ? 8 : 1, edge, linear, nsa, false).Run(device, library); ++runs;
            for (const bool first : {false, true}) { Replay(64, 16, twoD ? 8 : 1, edge, linear, nsa, first, true).Run(device, library); ++runs; }
        }
    std::cout << "mixed PASS dispatches=" << runs << " same image/shader normalized control and both paired sampler orders\n";
}

// Templates keep the exact fixture source compilable against the old source:
// baseline native still reaches its FORCE_UNNORMALIZED refusal. Other modes
// require the real new certificate API and fail clearly if absent.
template<class Binding> void CheckProofs(const std::vector<Binding>& bindings, std::uint32_t expectedLiveUses = 0) {
    if constexpr (requires(Binding b) { b.samplerPixelProof; b.imagePixelProof; b.resourceSources; b.samplerUnnormalized; b.imageUnnormalized; }) {
        unsigned samplers = 0, images = 0;
        for (const auto& b : bindings) {
            if (b.role == DescriptorRole::GuestSamplers) {
                Require(b.samplerUnnormalized.size() == b.count && b.samplerPixelProof.size() == b.count && b.resourceSources.size() == b.count, "sampler proof vector counts");
                for (std::uint32_t i = 0; i < b.count; ++i) if (b.samplerUnnormalized[i]) {
                    Require(!b.samplerPixelProof[i].empty(), "missing pixel sampler certificate");
                    for (const auto& p : b.samplerPixelProof[i]) {
                        Require(p.samplerSource == b.resourceSources[i] && p.liveUseMask == 1 && p.liveUseCount > 0 && p.samplerLiveUseCount > 0,
                            "wrong live source or live sample count");
                        Require(expectedLiveUses == 0 || (p.liveUseCount == expectedLiveUses && p.samplerLiveUseCount == expectedLiveUses),
                            "dead sample incorrectly contributed to live proof count");
                        Require(std::equal(p.samplerDescriptor.begin(), p.samplerDescriptor.end(), b.guestDescriptor.begin() + i * 4), "sampler snapshot certificate mismatch");
                    }
                    ++samplers;
                }
            }
            if (b.role == DescriptorRole::GuestImages) for (std::uint32_t i = 0; i < b.count; ++i) if (b.imageUnnormalized.at(i)) {
                Require(!b.imagePixelProof.at(i).empty(), "missing image certificate");
                for (const auto& p : b.imagePixelProof[i]) Require(p.imageSource == b.resourceSources.at(i) &&
                    std::equal(p.imageDescriptor.begin(), p.imageDescriptor.end(), b.guestDescriptor.begin() + i * 8), "image snapshot certificate mismatch");
                ++images;
            }
        }
        Require(samplers == 1 && images == 1, "fixture must certify exactly one pixel sampler and its image");
    } else throw std::runtime_error("new production pixel certificate API absent");
}
template<class Binding> bool SameProofs(const std::vector<Binding>& a, const std::vector<Binding>& b) {
    if constexpr (requires(Binding x) { x.samplerPixelProof; x.imagePixelProof; x.resourceSources; x.samplerUnnormalized; x.imageUnnormalized; }) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) if (a[i].samplerPixelProof != b[i].samplerPixelProof || a[i].imagePixelProof != b[i].imagePixelProof ||
            a[i].resourceSources != b[i].resourceSources || a[i].samplerUnnormalized != b[i].samplerUnnormalized || a[i].imageUnnormalized != b[i].imageUnnormalized ||
            a[i].guestDescriptor != b[i].guestDescriptor || a[i].count != b[i].count) return false;
        return true;
    } else return false;
}
template<class Binding> void CheckNormalizedProofs(const std::vector<Binding>& bindings) {
    if constexpr (requires(Binding b) { b.samplerPixelProof; b.imagePixelProof; b.samplerUnnormalized; b.imageUnnormalized; }) {
        for (const auto& b : bindings) {
            Require(std::none_of(b.samplerUnnormalized.begin(), b.samplerUnnormalized.end(), [](bool x) { return x; }) &&
                std::none_of(b.imageUnnormalized.begin(), b.imageUnnormalized.end(), [](bool x) { return x; }), "normalized descriptor retained stale pixel flag");
            for (const auto& p : b.samplerPixelProof) Require(p.empty(), "normalized descriptor retained stale sampler certificate");
            for (const auto& p : b.imagePixelProof) Require(p.empty(), "normalized descriptor retained stale image certificate");
        }
    } else throw std::runtime_error("new production pixel certificate API absent");
}
template<class Binding> void MutateProof(std::vector<Binding>& bindings, unsigned fault) {
    if constexpr (requires(Binding b) { b.samplerPixelProof; b.imagePixelProof; b.samplerUnnormalized; }) {
        for (auto& b : bindings) if (b.role == DescriptorRole::GuestSamplers) {
            if (fault == 0) b.samplerPixelProof.clear();
            if (fault == 1) b.samplerPixelProof.at(0).clear();
            if (fault == 2) ++b.samplerPixelProof.at(0).at(0).samplerLiveUseCount;
            if (fault == 3) b.guestDescriptor.at(0) &= ~(1u << 15u);
            if (fault == 4) b.samplerPixelProof.at(0).at(0).samplerDescriptor.at(0) ^= 1u;
            if (fault == 5) ++b.samplerPixelProof.at(0).at(0).samplerSource;
        }
        if (fault == 6) for (auto& b : bindings) if (b.role == DescriptorRole::GuestImages) b.imagePixelProof.clear();
    } else throw std::runtime_error("new production pixel certificate API absent");
}
template<class Binding> void SplitDuplicateProof(std::vector<Binding>& bindings) {
    if constexpr (requires(Binding b) { b.samplerPixelProof; b.imagePixelProof; }) {
        for (auto& b : bindings) {
            auto* list = b.role == DescriptorRole::GuestSamplers ? &b.samplerPixelProof :
                b.role == DescriptorRole::GuestImages ? &b.imagePixelProof : nullptr;
            if (list == nullptr) continue;
            auto& proofs = list->at(0); auto first = proofs.at(0), second = first;
            first.liveUseCount = 1; second.liveUseCount = 2;
            first.samplerLiveUseCount = second.samplerLiveUseCount = 3;
            proofs = {first, second};
        }
    } else throw std::runtime_error("new production pixel certificate API absent");
}
void ProofRefusals(id<MTLDevice> device, id<MTLLibrary> library) {
    Replay fixture(32, 8, 4, true, false, false);
    auto guest = Compile(device, Program(true, false, false), fixture.users); CheckProofs(guest.bindings);
    MetalBackend::TargetOptions options; options.supportsGpuAddresses = options.supportsInt64 = options.supportsSimdGroups = true;
    const auto good = MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
    AgcDriver::Metal::MetalDevice backend(device, library);
    const auto ranges = fixture.Ranges();
    { AgcDriver::Metal::MetalShaderResources resources(backend, ranges); Require(!resources.Bindings(good).empty(), "actual native certified resource construction"); }
    const std::array<const char*, 7> names{"malformed proof count", "missing certificate", "wrong live count", "forged descriptor bit", "snapshot mismatch", "source association mismatch", "missing image proof"};
    for (unsigned fault = 0; fault < names.size(); ++fault) {
        auto wrong = good; MutateProof(wrong.guest.bindings, fault);
        Reject(names[fault], "pixel", [&] { AgcDriver::Metal::MetalShaderResources resources(backend, ranges); static_cast<void>(resources.Bindings(wrong)); });
    }
    for (const auto& name : {"mips", "array", "3D", "wrong width", "wrong format", "base mip", "base layer"}) {
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:8 height:4 mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        if (std::string(name) == "mips") descriptor.mipmapLevelCount = 2;
        if (std::string(name) == "array" || std::string(name) == "base layer") { descriptor.textureType = MTLTextureType2DArray; descriptor.arrayLength = 2; }
        if (std::string(name) == "3D") { descriptor.textureType = MTLTextureType3D; descriptor.depth = 2; }
        if (std::string(name) == "wrong width") descriptor.width = 16;
        if (std::string(name) == "wrong format") descriptor.pixelFormat = MTLPixelFormatRGBA32Uint;
        if (std::string(name) == "base mip") { descriptor.width = 16; descriptor.height = 8; descriptor.mipmapLevelCount = 2; }
        auto root = [device newTextureWithDescriptor:descriptor]; Require(root != nil, std::string("real native texture allocation ") + name);
        id<MTLTexture> selected = root;
        if (std::string(name) == "base mip") selected = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(1,1) slices:NSMakeRange(0,1)];
        if (std::string(name) == "base layer") selected = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0,1) slices:NSMakeRange(1,1)];
        Require(selected != nil, std::string("real native texture view allocation ") + name);
        Reject(std::string("actual depthLookup selected view ") + name, "Metal pixel sampler actual selected texture", [&] {
            AgcDriver::Metal::MetalShaderResources resources(backend, ranges,
                [selected](const AgcDriver::Graphics::GuestTextureResource&, bool, bool) { return selected; });
            static_cast<void>(resources.Bindings(good));
        });
    }
    AgcDriver::Metal::MetalComputePipeline pipeline(device, good);
    AgcDriver::Metal::MetalShaderResources resources(backend, ranges);
    const auto capturedBindings = resources.Bindings(good);
    auto externalDescriptor = [MTLSamplerDescriptor new]; externalDescriptor.normalizedCoordinates = YES;
    externalDescriptor.sAddressMode = externalDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    auto externalSampler = [device newSamplerStateWithDescriptor:externalDescriptor]; Require(externalSampler != nil, "real caller supplied sampler allocation");
    for (const bool assertedAgreement : {false, true}) {
        auto wrong = capturedBindings;
        for (auto& b : wrong) if (!b.samplers.empty()) { b.samplers.at(0) = externalSampler; b.samplersMatchCapturedDescriptors = assertedAgreement; }
        Reject(std::string("external sampler identity asserted=") + std::to_string(assertedAgreement), "Metal pixel sampler native identity", [&] {
            pipeline.Encode(backend.CommandBuffer(), wrong, MTLSizeMake(32,1,1), {}, resources.Residency());
        });
    }
}
std::vector<std::uint32_t> QualifiedAndOffsetProgram(bool deadOffset) {
    auto code = Program(true,false,true); unsigned sampleIndex = 0;
    for (std::size_t i = 0; i < code.size(); ++i) if ((code[i] >> 26u) == 0x3cu) {
        if (++sampleIndex == 2) {
            code[i] = (code[i] & ~(0x7fu<<18u)) | (0x34u<<18u);
            code[i+1] = (code[i+1] & ~(31u<<21u)) | (4u<<21u);
        }
    }
    if (deadOffset) {
        const auto store = std::find(code.begin(),code.end(),0xe0781010u);
        Require(store != code.end(),"dead sample fixture lost output store"); code.erase(store,store+2);
    }
    return code;
}
void DeadSourceLimitations(id<MTLDevice> device) {
    Replay original(32,8,4,true,false,false,true,true); const auto dead = QualifiedAndOffsetProgram(true);
    for (const bool pixel : {true,false}) {
        auto users = original.users; users[16] = (users[16] & ~(1u<<15u)) | (pixel ? 1u<<15u : 0u);
        Reject(std::string("retained dead-image s8 source placement limitation pixel=") + std::to_string(pixel),
            "ResourceMaterializer::Apply cannot remap an image memory reference", [&] { static_cast<void>(Compile(device,dead,users)); });
    }
    std::cout << "dead-image s8 source placement remains unsupported before pixel proof; normalized control has the same stale-metadata refusal\n";
}
void Rejections(id<MTLDevice> device, id<MTLLibrary> library) {
    Replay fixture(32, 8, 4, true, false, false);
    const auto standard = Program(true, false, false);
    for (const auto opcode : {0x22u, 0x28u, 0x34u, 0x47u, 0xa0u, 0x60u}) {
        auto code = Program(true, false, false, opcode);
        auto users = fixture.users;
        if (opcode == 0x47u) for (auto& word : code) if ((word >> 26u) == 0x3cu) word = (word & ~0xf00u) | 0x200u;
        if (opcode == 0x28u) { users[16] |= 1u << 12u; users[9] = (users[9] & ~(0x1ffu << 20u)) | (22u << 20u); }
        Reject("live unsupported opcode=" + Hex(opcode), "unnormalized", [&] { static_cast<void>(Compile(device, code, users)); });
    }
    // Compute image_sample flags0 is intentionally explicitLOD0. Only a real
    // fragment request can exercise the unsupported implicit derivative form.
    const std::array<std::uint32_t, 9> implicitFragment{0x7e0402ff, 0x3f000000, 0x7e0602ff, 0x3f000000,
        0xf0800f08, 0x00820602, 0xf800180f, 0x09080706, 0xbf810000};
    const std::array<MemoryRegion, 1> fragmentRegions{{{reinterpret_cast<std::uintptr_t>(implicitFragment.data()), std::as_bytes(std::span(implicitFragment))}}};
    ShaderPixelStageInfo pixel{}; pixel.wave32 = true; pixel.targetOutputMode[0] = 9; pixel.targetExportMapping.fill(0xe4);
    RecompileRequest fragment{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(implicitFragment.data()), implicitFragment, 0, {}},
        {32, 0, fixture.users, {}, pixel, {}, fragmentRegions}, Target(device), {0,0,0,128}};
    fragment.useCache = false;
    Reject("implicit fragment sample", "unnormalized", [&] { static_cast<void>(Recompile(fragment)); });
    struct DescriptorCase { const char* name; unsigned word; std::uint32_t clear, set; };
    const std::array<DescriptorCase, 12> samplerCases{{
        {"repeat",16,7u,0}, {"mirror",16,7u,1u}, {"opaque border",19,3u<<30u,1u<<30u},
        {"different filters",18,3u<<20u,1u<<20u}, {"anisotropic filter",18,3u<<20u,2u<<20u},
        {"anisotropy ratio with equal point filters",16,0,1u<<9u},
        {"TRUNC_COORD",16,0,1u<<27u}, {"MC_COORD_TRUNC",18,0,1u<<31u}, {"FORCE_SRGB",16,0,1u<<20u},
        {"mip filtering",18,0,1u<<26u}, {"sampler minLOD",17,0,1u}, {"LOD bias",18,0,1u},
    }};
    AgcDriver::Metal::MetalDevice backend(device, library); const auto ranges = fixture.Ranges();
    MetalBackend::TargetOptions options; options.supportsGpuAddresses = options.supportsInt64 = options.supportsSimdGroups = true;
    for (const auto& c : samplerCases) {
        auto users = fixture.users; users[c.word] = (users[c.word] & ~c.clear) | c.set;
        const auto guest = Compile(device, standard, users);
        const auto native = MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
        Reject(c.name, std::string(c.name) == "FORCE_SRGB" ? "forces sRGB" : "unnormalized", [&] {
            AgcDriver::Metal::MetalShaderResources resources(backend, ranges); static_cast<void>(resources.Bindings(native));
        });
    }
    const std::array<DescriptorCase, 5> imageCases{{
        {"view minLOD",9,0,1u<<8u}, {"extra mip",11,0,1u<<16u}, {"base level",11,0,(1u<<12u)|(1u<<16u)},
        {"extra layer",12,0,1u}, {"base array",12,0,1u<<16u}}};
    for (const auto& c : imageCases) {
        auto users = fixture.users; users[c.word] = (users[c.word] & ~c.clear) | c.set;
        Reject(c.name, "unnormalized", [&] { static_cast<void>(Compile(device, standard, users)); });
    }
    for (const auto type : {10u, 11u, 12u, 13u}) {
        auto users = fixture.users; users[11] = (users[11] & ~(15u<<28u)) | (type<<28u);
        Reject("shape descriptor type=" + std::to_string(type), "unnormalized", [&] { static_cast<void>(Compile(device, standard, users)); });
    }
    auto msaaDescriptor = fixture.users; msaaDescriptor[11] = (msaaDescriptor[11] & ~(15u<<28u)) | (14u<<28u) | (1u<<16u); msaaDescriptor[13] = 1u<<4u;
    Reject("multisampled descriptor binding", "unnormalized", [&] { static_cast<void>(Compile(device, standard, msaaDescriptor)); });
    auto packedCode = standard;
    for (auto& word : packedCode) if ((word >> 26u) == 0x3cu) word |= 0x8000u;
    Reject("reduced-width R128 descriptor", "unnormalized", [&] { static_cast<void>(Compile(device, packedCode, fixture.users)); });
    auto conversion = fixture.users; conversion[9] = (conversion[9] & ~(0x1ffu<<20u)) | (34u<<20u);
    Reject("conversion sampled image", "unnormalized", [&] { static_cast<void>(Compile(device, standard, conversion)); });
    auto depth = fixture.users; depth[9] = (depth[9] & ~(0x1ffu<<20u)) | (20u<<20u); depth[11] |= 4u<<20u;
    Reject("depth-bits sampled image", "unnormalized", [&] { static_cast<void>(Compile(device, standard, depth)); });
    auto msaaCode = standard;
    for (auto& word : msaaCode) if ((word >> 26u) == 0x3cu) word = (word & ~(7u<<3u)) | (6u<<3u);
    Reject("multisampled sample instruction decoder", "unsupported multisampled MIMG operation", [&] { static_cast<void>(Compile(device, msaaCode, fixture.users)); });
    // Retained public table-indexed image instructions, with a qualified
    // sampler and direct mip0 color entries. The indirection itself is the
    // refusal contract; no unrelated unsupported mip/layer fields are present.
    const std::array<std::uint32_t, 27> indirectCode{
        0xf4080100,0xfa000000,0xf4080200,0xfa000010,0xf4080300,0xfa000020,0xf4080700,0xfa000030,
        0x7e200500,0x93109010,0xf4200406,0x20000004,0x8f108510,0xf42c0502,0x20000000,
        0x34140082,0x7e0802ff,0x3f000000,0x7e0a02ff,0x3f000000,0x7e0c0280,
        0xf09c0f08,0x00450004,0xbf8c3f70,0xe0701000,0x8007000a,0xbf810000};
    std::array<std::uint32_t, 16> imageTable{};
    std::copy_n(fixture.users.begin()+8,8,imageTable.begin()); std::copy_n(fixture.users.begin()+8,8,imageTable.begin()+8);
    imageTable[8] += 16; // distinct color surface address, same supported shape
    const std::array<std::uint32_t, 12> materials{0,0,0,0, 0,0,0,0, 0,1,0,0};
    const std::array<std::uint32_t, 16> srt{0x3000000,32u<<16u,2,0xfac,
        0x8092,0,0,0, 0x3010000,16u<<16u,3,0xfac, 0x3020000,0,128,0xfac};
    const std::array<std::uint32_t, 2> indirectUsers{0x3030000,0};
    const std::array<MemoryRegion, 4> indirectRegions{{
        {reinterpret_cast<std::uintptr_t>(indirectCode.data()),std::as_bytes(std::span(indirectCode))},
        {0x3000000,std::as_bytes(std::span(imageTable))}, {0x3010000,std::as_bytes(std::span(materials))}, {0x3030000,std::as_bytes(std::span(srt))}}};
    auto indirect = Request(device,indirectCode,indirectUsers,indirectRegions,false);
    Reject("indirect sampled image binding", "unnormalized", [&] { static_cast<void>(Recompile(indirect)); });
    Replay pair(32,8,4,true,false,false,true,true);
    auto liveMixed = QualifiedAndOffsetProgram(false);
    Reject("same sampler live qualified plus offset sample", "unnormalized", [&] { static_cast<void>(Compile(device,liveMixed,pair.users)); });
    DeadSourceLimitations(device);
    auto deadMixed = QualifiedAndOffsetProgram(true);
    // Preserve the s8 failure above. The supported placement uses T#s0, so a
    // dead untranslated memory.resource0 still addresses tracked image0. Only
    // descriptor register placement changes; the dead offset remains present.
    for (std::size_t i = 0; i < deadMixed.size(); ++i) {
        if (deadMixed[i] == 0xe0381000u || deadMixed[i] == 0xe0381010u)
            deadMixed[i+1] = (deadMixed[i+1] & ~(31u<<16u)) | (2u<<16u);
        if (deadMixed[i] == 0xe0781000u) deadMixed[i+1] = (deadMixed[i+1] & ~(31u<<16u)) | (3u<<16u);
        if ((deadMixed[i] >> 26u) == 0x3cu) deadMixed[i+1] &= ~(31u<<16u);
    }
    const auto originalUsers = pair.users;
    std::copy_n(originalUsers.begin()+8,8,pair.users.begin());
    std::copy_n(originalUsers.begin(),4,pair.users.begin()+8);
    std::copy_n(originalUsers.begin()+4,4,pair.users.begin()+12);
    CheckProofs(Compile(device,deadMixed,pair.users).bindings,1);
    pair.SetProgram(deadMixed); pair.RebuildCommands(); pair.secondOutput = false; pair.label += " T#s0 dead-offset-DCE"; pair.Run(device,library);
    std::cout << "dead unsupported offset sample T#s0 PASS: exactly one live qualified use certified; first exact output and unwritten second output guards; s8 placement limitation retained\n";
    ProofRefusals(device, library);
    std::cout << "rejections PASS: actual public compiler binding and native proof owners; later emitter guards not claimed\n";
}
void CacheNativePhase(id<MTLDevice> device, id<MTLLibrary> library, bool hit) {
    Require(ShaderDiskCache::Enabled(), "separate native cache phase requires enabled cache");
    const auto before = ShaderDiskCache::Totals();
    Replay(32, 8, 4, true, false, false).Run(device, library); ShaderDiskCache::Flush();
    const auto after = ShaderDiskCache::Totals();
    Require(hit ? after.hits > before.hits : after.misses > before.misses && after.writes > before.writes,
        hit ? "genuine separate-process native disk hit not observed" : "genuine separate-process native disk miss/write not observed");
    std::cout << "native disk phase=" << (hit ? "hit" : "cold") << " hits=" << after.hits-before.hits << " misses=" << after.misses-before.misses << " writes=" << after.writes-before.writes << " exact native output PASS\n";
}
void SpawnCachePhase(const char* executable, const char* utility, const char* mode) {
    std::array<char*, 4> args{const_cast<char*>(executable), const_cast<char*>(utility), const_cast<char*>(mode), nullptr};
    pid_t child = 0; Require(posix_spawn(&child, executable, nullptr, nullptr, args.data(), environ) == 0, "native cache child spawn failed");
    int status = 0; Require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, std::string("native cache child failed phase=") + mode);
}
// Authoring gate: retained zero-use FORCE_UNNORMALIZED sampler metadata after
// post-tracking DCE must permit exact native output from the surviving sampler.
// PixelProofsFor currently refuses that dead slot before SPIR-V emission. The
// public unused-sample control below passes on PR93 because early DCE removes
// its sampler before tracking; it does not reproduce this compiler-owner bug.
// The regression intentionally removes one output consumer from real translated,
// tracked and materialized IR, leaving resource metadata untouched. All subsequent
// DCE/collection/allocation/population/emission/native/cache steps use existing
// production APIs. This is fixture-induced late DCE, not untouched AGC Recompile.
Replay DeadSamplerReplay() {
    Replay replay(32,8,4,true,false,false,false,true);
    auto code = Program(true,false,true); unsigned sample = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] == 0xe0381000u || code[i] == 0xe0381010u) code[i+1] = (code[i+1] & ~(31u<<16u)) | (2u<<16u);
        if (code[i] == 0xe0781000u) code[i+1] = (code[i+1] & ~(31u<<16u)) | (3u<<16u);
        if ((code[i] >> 26u) == 0x3cu) {
            code[i+1] &= ~(31u<<16u); // actual T#s0, avoiding retained s8 metadata limitation
            if (++sample == 2) code[i] = (code[i] & ~(0x7fu<<18u)) | (0x34u<<18u); // s20 offset use, unused result
        }
    }
    const auto store = std::find(code.begin(),code.end(),0xe0781010u);
    Require(store != code.end(),"dead s20 fixture lost unused output store"); code.erase(store,store+2);
    const auto users = replay.users;
    std::copy_n(users.begin()+8,8,replay.users.begin()); std::copy_n(users.begin(),4,replay.users.begin()+8);
    std::copy_n(users.begin()+4,4,replay.users.begin()+12);
    Require((replay.users[20] & (1u<<15u)) != 0,"dead s20 must retain FORCE_UNNORMALIZED descriptor");
    replay.SetProgram(code); replay.RebuildCommands(); replay.secondOutput = false;
    replay.label = "dead s20 FORCE_UNNORMALIZED offset-only T#s0 live s16 normalized";
    return replay;
}
void EarlyDeadSamplerControl(id<MTLDevice> device, id<MTLLibrary> library) {
    auto replay = DeadSamplerReplay(); AgcDriver::Metal::MetalDriver driver;
    const auto ranges = replay.Ranges(); driver.Configure((__bridge void*)device,(__bridge void*)library,ranges);
    driver.RegisterShader(reinterpret_cast<const Shader*>(Base+0x50100));
    try {
        // Same registered code and native driver, with live coordinate policy
        // and dead descriptor contents replaced between real submissions.
        for (const bool pixel : {false,true,true,false}) {
            replay.users[16] = 0x92u | (pixel ? 1u<<15u : 0u);
            replay.users[20] = (pixel ? 0xb6u : 0x92u) | (1u<<15u); // dead edge/zero descriptors
            replay.SetInputs(); replay.RebuildCommands();
            replay.label = std::string("dead s20 FORCE_UNNORMALIZED offset-only T#s0 live s16 ")+(pixel ? "pixel" : "normalized");
            replay.RunConfigured(driver,false);
        }
        driver.Shutdown();
    } catch (...) { driver.Shutdown(); throw; }
    std::cout << "public early-DCE control PASS (already passes pre-fix): normalized/pixel live s16, distinct s20 only unused unsupported sample; same registered program descriptor replacements; exact native outputs and whole canaries\n";
}
void CheckDeadSamplerInfo(const ShaderInfo& info, unsigned dead) {
    const auto live = 1u-dead;
    Require(info.samplers.size() == 2 && info.samplers[dead].liveUseCount == 0 && info.samplers[dead].liveUseMask == 0 &&
        info.samplers[live].liveUseCount == 1 && info.samplers[live].liveUseMask == 1,
        "late-DCE requires retained two-slot sampler metadata with counts 0/1 and masks 0/qualified");
    Require(info.samplers[0].source != info.samplers[1].source && info.sampledPairs.size() == 1 &&
        info.sampledPairs[0].sampler == live && info.sampledPairs[0].liveUseCount == 1,
        "late-DCE retained a dead sampled pair or changed the surviving sampler association");
}
template<class Binding> void CheckDeadSamplerBindings(const std::vector<Binding>& bindings, const ShaderInfo& info, unsigned dead, bool pixel) {
    if constexpr (requires(Binding b) { b.samplerPixelProof; b.samplerUnnormalized; b.resourceSources; }) {
        unsigned samplerBindings = 0;
        for (const auto& b : bindings) if (b.role == DescriptorRole::GuestSamplers) {
            ++samplerBindings;
            Require(b.count == 2 && b.guestDescriptor.size() == 8 && b.resourceSources.size() == 2 &&
                b.samplerUnnormalized.size() == 2 && b.samplerPixelProof.size() == 2,
                "late-DCE sampler binding lost stable two-slot allocation");
            Require(b.resourceSources[0] == info.samplers[0].source && b.resourceSources[1] == info.samplers[1].source,
                "late-DCE sampler descriptor binding order changed");
            Require(!b.samplerUnnormalized[dead] && b.samplerPixelProof[dead].empty() &&
                (b.guestDescriptor[dead*4] & (1u<<15u)) == 0,
                "late-DCE dead sampler was not neutralized for native resource construction");
            Require(b.samplerUnnormalized[1u-dead] == pixel,
                "late-DCE surviving sampler coordinate policy changed");
        }
        Require(samplerBindings == 1,"late-DCE sampler binding absent");
        if (pixel) CheckProofs(bindings,1); else CheckNormalizedProofs(bindings);
    } else throw std::runtime_error("new production pixel certificate API absent");
}
Replay LateDeadSamplerReplay(bool liveSecond, bool pixel) {
    Replay replay(32,8,4,true,false,false,pixel,true);
    auto code = Program(true,false,true);
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] == 0xe0381000u || code[i] == 0xe0381010u) code[i+1] = (code[i+1] & ~(31u<<16u)) | (2u<<16u);
        if (code[i] == 0xe0781000u || code[i] == 0xe0781010u) code[i+1] = (code[i+1] & ~(31u<<16u)) | (3u<<16u);
        if ((code[i] >> 26u) == 0x3cu) code[i+1] &= ~(31u<<16u);
    }
    const auto users = replay.users;
    std::copy_n(users.begin()+8,8,replay.users.begin()); std::copy_n(users.begin(),4,replay.users.begin()+8);
    std::copy_n(users.begin()+4,4,replay.users.begin()+12);
    const auto live = liveSecond ? 1u : 0u;
    replay.users[16+live*4] = 0x92u | (pixel ? 1u<<15u : 0u);
    replay.users[16+(1u-live)*4] = 0x92u | (1u<<15u);
    replay.SetInputs(); replay.SetProgram(code); replay.RebuildCommands();
    replay.firstOutput = !liveSecond; replay.secondOutput = liveSecond;
    replay.label = std::string("fixture-induced posttracking DCE T#s0 ") + (liveSecond ? "dead/live " : "live/dead ") + (pixel ? "pixel" : "normalized");
    return replay;
}
CompiledVariant CompileLateDeadSampler(const RecompileRequest& request, const ResourceCapture& capture, unsigned dead) {
    auto program = PrepareResourceProgram(request);
    Require(program.Info().samplers.size() == 2 && program.Info().sampledPairs.size() == 2,
        "late-DCE source must track TWO LIVE sampler uses before fixture removes a consumer");
    const auto trackedSamplers = program.Info().samplers;
    ResourceMaterializer{}.Apply(program,capture.specialization);
    std::vector<IrValue*> stores;
    for (auto& block : program.Blocks()) for (IrValue* inst : block->Instructions())
        if (BufferAccessOf(inst->Opcode()) == BufferAccess::Write) stores.push_back(inst);
    Require(stores.size() == 2,"late-DCE source requires exactly two translated output consumers");
    // Sole fixture transformation: remove one real buffer-store consumer after
    // resource tracking/materialization. Do not remove samples or edit metadata.
    auto* removed = stores.at(dead); removed->Invalidate(); removed->Parent()->RemoveInstruction(removed);
    DeadCodeEliminator{}.RemoveIdentities(program); DeadCodeEliminator{}.Eliminate(program);
    const auto inputs = BuildShaderStageInputInfo(ShaderStageKind::Compute,request.context,32);
    ShaderInfoCollector{}.Collect(program,inputs); CheckDeadSamplerInfo(program.Info(),dead);
    Require(program.Info().samplers[0].source == trackedSamplers[0].source && program.Info().samplers[1].source == trackedSamplers[1].source,
        "late-DCE changed tracked sampler source order");
    std::cout << "compiler-owner late-DCE retainedSamplers=2 deadSlot=" << dead << " liveUseCounts="
        << program.Info().samplers[0].liveUseCount << ',' << program.Info().samplers[1].liveUseCount << " livePairs=1 before Populate\n";
    auto allocation = BindingAllocator{}.Allocate(program,request.layout);
    DescriptorBindingBuilder{}.Populate(allocation,program,capture.snapshot,{});
    const SpirvTargetOptions target{request.target.vulkanVersion,request.target.spirvVersion,request.target.subgroupSize,
        request.target.bdaAbiVersion,request.target.supportedCapabilities,request.target.supportedExtensions,request.target.nonConstantImageOffsets};
    RecompileResult result; result.spirv = SpirvEmitter{}.Emit(program,inputs,allocation,target);
    result.bdaAbiVersion = program.Info().usesDma ? request.target.bdaAbiVersion : 0u;
    result.memoryOffsetDword = allocation.layout.memoryOffsetDword; result.hostSubgroupSize = 32;
    // As in compileVariant, store descriptor-free allocation/result; reload
    // must call the real Populate owner with the captured snapshot again.
    allocation.bindings.clear(); allocation.pushConstants.clear();
    return {capture.specialization,request.layout,std::move(program).TakeCompiledInfo(),std::move(allocation),std::move(result)};
}
void DeadSamplerOwnerRefusals(const CompiledVariant& variant, const ResourceSnapshot& snapshot, unsigned dead, std::span<const std::byte> key) {
    // Distinct dead-slot safety guard: zero-count metadata is admissible only
    // with zero mask and no sampled pair. Existing live-proof corruption cases
    // do not exercise the new unused-slot exception.
    for (const bool pair : {false,true}) {
        auto malformed = variant.info.info;
        if (pair) malformed.sampledPairs.push_back({0,dead,0,0,0});
        else malformed.samplers[dead].liveUseMask = 1;
        Reject(pair ? "unused sampler stale zero-use pair" : "unused sampler inconsistent live mask",
            "unused sampler has inconsistent live-use metadata",[&] {
                auto allocation = variant.bindings;
                DescriptorBindingBuilder{}.Populate(allocation,malformed,variant.info.stage,variant.info.userDataBase,snapshot,{});
            });
    }
    auto malformed = variant;
    // Keep count0/mask0 sums consistent, so rejection must cover the serialized
    // zero-use pair itself rather than an incidental total-count mismatch.
    malformed.info.info.sampledPairs.push_back({0,dead,0,0,0});
    const auto file = ShaderDiskCache::EncodeEntry(key,malformed); CompiledVariant rejected;
    Require(ShaderDiskCache::DecodeEntry(file,key,rejected) == ShaderDiskCache::LoadStatus::Rejected,
        "CACHE DEAD SAMPLER serialized zero-use sampled pair accepted with consistent mask/count sums");
    std::cout << "unused sampler owner refusals PASS inconsistent mask/stale pair population and serialized zero-use pair with consistent totals\n";
}
void LateDeadSamplerNative(id<MTLDevice> device, id<MTLLibrary> library, int diskPhase) {
    for (const bool liveSecond : {false,true}) for (const bool pixel : {false,true}) {
        auto replay = LateDeadSamplerReplay(liveSecond,pixel); const unsigned dead = liveSecond ? 0u : 1u;
        const auto code = std::span(replay.code).subspan(GuardWords,replay.codeWords);
        const std::array<MemoryRegion,1> regions{{{reinterpret_cast<std::uintptr_t>(code.data()),std::as_bytes(code)}}};
        auto request = Request(device,code,replay.users,regions,false);
        const auto capture = CaptureResources(request,SrtRuntime{replay.users});
        Require(capture->snapshot.samplers.size() == 2 && (capture->snapshot.samplers[dead].dwords[0] & (1u<<15u)) != 0,
            "late-DCE snapshot lost original dead FORCE_UNNORMALIZED descriptor");
        std::vector<std::byte> key;
        // Never masquerade transformed IR as the untouched AGC Recompile key.
        const std::string identity = "UnnormalizedSampler/compiler-owner-fixture-induced-posttracking-DCE/v1";
        const auto identityBytes = std::as_bytes(std::span(identity.data(),identity.size())); key.assign(identityBytes.begin(),identityBytes.end());
        std::vector<std::byte> productionKey; ShaderDiskCache::BuildKey(request,32,capture->specialization,productionKey);
        key.insert(key.end(),productionKey.begin(),productionKey.end()); key.push_back(std::byte(dead)); key.push_back(std::byte(pixel));
        const auto before = ShaderDiskCache::Totals(); CompiledVariant variant;
        if (diskPhase != 0) {
            const bool loaded = ShaderDiskCache::Load(key,variant);
            Require(loaded == (diskPhase == 2),diskPhase == 2 ? "late-DCE separate-process compiler-owner cache hit absent" : "late-DCE cold isolated key unexpectedly hit");
        }
        if (diskPhase != 2) {
            variant = CompileLateDeadSampler(request,*capture,dead);
            if (diskPhase == 1) { ShaderDiskCache::Store(key,std::make_shared<const CompiledVariant>(variant)); ShaderDiskCache::Flush(); }
        } else { variant.specialization = capture->specialization; variant.layout = request.layout; }
        CheckDeadSamplerInfo(variant.info.info,dead);
        BindingAllocationResult bindings; bindings.layout = variant.bindings.layout;
        bindings.pushConstantOffsetBytes = variant.bindings.pushConstantOffsetBytes; bindings.pushConstantSizeBytes = variant.bindings.pushConstantSizeBytes;
        DescriptorBindingBuilder{}.Populate(bindings,variant.info.info,variant.info.stage,variant.info.userDataBase,capture->snapshot,{});
        if (diskPhase == 0 && !liveSecond && !pixel) DeadSamplerOwnerRefusals(variant,capture->snapshot,dead,key);
        auto guest = variant.result; guest.bindings = std::move(bindings.bindings); guest.pushConstants = std::move(bindings.pushConstants);
        MetalBackend::TargetOptions options; options.supportsGpuAddresses = options.supportsInt64 = options.supportsSimdGroups = true;
        const auto metal = MetalBackend::ConvertToMetal(guest,ShaderStage::Compute,options);
        AgcDriver::Metal::MetalDevice backend(device,library); const auto ranges = replay.Ranges();
        replay.RunAction([&] {
            AgcDriver::Metal::MetalShaderResources resources(backend,ranges);
            const auto nativeBindings = resources.Bindings(metal);
            CheckDeadSamplerBindings(guest.bindings,variant.info.info,dead,pixel);
            AgcDriver::Metal::MetalComputePipeline pipeline(device,metal);
            const auto commands = backend.CommandBuffer(); pipeline.Encode(commands,nativeBindings,MTLSizeMake(32,1,1),{},resources.Residency());
            backend.Wait(commands); Require(resources.Complete(commands).state == BdaAbi::FaultState::Empty,"late-DCE native guest access fault");
        });
        const auto after = ShaderDiskCache::Totals();
        if (diskPhase != 0) Require(diskPhase == 2 ? after.hits > before.hits : after.misses > before.misses && after.writes > before.writes,
            "late-DCE actual compiler-owner disk phase counters absent");
        std::cout << "compiler-owner late-DCE phase=" << (diskPhase == 0 ? "uncached" : diskPhase == 1 ? "cold" : "hit")
            << " deadSlot=" << dead << " livePixel=" << pixel << " hits=" << after.hits-before.hits << " misses=" << after.misses-before.misses
            << " writes=" << after.writes-before.writes << " spirvWords=" << guest.spirv.size() << " exact native golden/whole canaries PASS\n";
    }
}
void DeadSamplerPhase(id<MTLDevice> device, id<MTLLibrary> library, bool hit) {
    Require(ShaderDiskCache::Enabled(),"dead sampler compiler-owner disk phase requires enabled cache");
    LateDeadSamplerNative(device,library,hit ? 2 : 1);
}
void DeadSampler(id<MTLDevice> device, id<MTLLibrary> library, const char* executable, const char* utility) {
    EarlyDeadSamplerControl(device,library);
    LateDeadSamplerNative(device,library,0);
    const auto* configured = std::getenv("ANYPS5_SHADER_CACHE_DIR");
    const auto parent = configured != nullptr ? std::filesystem::path(configured) : std::filesystem::temp_directory_path();
    const auto childDirectory = parent/("dead-sampler-process-"+std::to_string(getpid())); std::filesystem::create_directories(childDirectory);
    Require(setenv("ANYPS5_NO_SHADER_CACHE","0",1) == 0 && setenv("ANYPS5_SHADER_CACHE_DIR",childDirectory.c_str(),1) == 0,"dead sampler child cache environment failed");
    SpawnCachePhase(executable,utility,"dead-sampler-cold"); SpawnCachePhase(executable,utility,"dead-sampler-hit");
}
void Cache(id<MTLDevice> device, id<MTLLibrary> library, const char* executable, const char* utility) {
    Require(ShaderDiskCache::Enabled(), "cache mode requires separately enabled shader disk cache");
    Replay fixture(32, 8, 4, true, false, false);
    const auto code = Program(true, false, false);
    const std::array<MemoryRegion, 1> regions{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    auto request = Request(device, code, fixture.users, regions, true);
    const auto capture = CaptureResources(request, SrtRuntime{fixture.users});
    bool memo = false; const auto first = Recompile(request, *capture, &memo); CheckProofs(first->bindings);
    const auto again = Recompile(request, *capture, &memo); Require(memo && again == first, "identical captured pixel snapshot must hit memo");
    std::vector<std::byte> bytes; ShaderDiskCache::EncodeResult(*first, bytes); RecompileResult decoded;
    Require(ShaderDiskCache::DecodeResult(bytes, decoded) && decoded.spirv == first->spirv && SameProofs(first->bindings, decoded.bindings), "CACHE PROOF result certificate roundtrip mismatch");
    auto duplicate = *first; SplitDuplicateProof(duplicate.bindings); std::vector<std::byte> duplicateBytes;
    ShaderDiskCache::EncodeResult(duplicate, duplicateBytes); RecompileResult invalidDuplicate;
    Require(!ShaderDiskCache::DecodeResult(duplicateBytes, invalidDuplicate), "CACHE PROOF duplicate source split-count certificates accepted");
    for (unsigned fault = 0; fault < 7; ++fault) {
        auto malformed = *first; MutateProof(malformed.bindings,fault); std::vector<std::byte> malformedBytes;
        ShaderDiskCache::EncodeResult(malformed,malformedBytes); RecompileResult invalid;
        Require(!ShaderDiskCache::DecodeResult(malformedBytes,invalid), "CACHE PROOF malformed certificate accepted fault="+std::to_string(fault));
    }
    for (const auto size : {std::size_t(0), bytes.size() / 2, bytes.size() - 1}) {
        RecompileResult rejected; Require(!ShaderDiskCache::DecodeResult(std::span(bytes).first(size), rejected), "truncated result metadata accepted");
    }
    auto program = PrepareResourceProgram(request); ResourceMaterializer{}.Apply(program, capture->specialization);
    DeadCodeEliminator{}.RemoveIdentities(program); DeadCodeEliminator{}.Eliminate(program);
    const auto inputs = BuildShaderStageInputInfo(ShaderStageKind::Compute, request.context, 32);
    ShaderInfoCollector{}.Collect(program, inputs);
    auto allocation = BindingAllocator{}.Allocate(program, request.layout);
    DescriptorBindingBuilder{}.Populate(allocation, program, capture->snapshot, {});
    CompiledVariant variant{capture->specialization, request.layout, std::move(program).TakeCompiledInfo(), allocation, *first};
    std::vector<std::byte> key; ShaderDiskCache::BuildKey(request, 32, capture->specialization, key);
    auto file = ShaderDiskCache::EncodeEntry(key, variant); CompiledVariant restored;
    // The specialization/layout are supplied by the validated disk key at the
    // real cache owner. DecodeEntry intentionally decodes only the payload.
    restored.specialization = capture->specialization; restored.layout = request.layout;
    Require(ShaderDiskCache::DecodeEntry(file, key, restored) == ShaderDiskCache::LoadStatus::Loaded &&
        restored.info.info == variant.info.info && SameProofs(restored.result.bindings, variant.result.bindings) && SameProofs(restored.bindings.bindings, variant.bindings.bindings),
        "CACHE PROOF full info/allocation/result roundtrip mismatch");
    Require(restored.info.stage == variant.info.stage && restored.info.shaderHash == variant.info.shaderHash && restored.info.waveSize == variant.info.waveSize &&
        restored.info.userDataBase == variant.info.userDataBase && restored.info.userDataCount == variant.info.userDataCount &&
        restored.bindings.layout == variant.bindings.layout && restored.bindings.pushConstantOffsetBytes == variant.bindings.pushConstantOffsetBytes &&
        restored.bindings.pushConstantSizeBytes == variant.bindings.pushConstantSizeBytes && restored.bindings.pushConstants == variant.bindings.pushConstants &&
        restored.result.pushConstants == variant.result.pushConstants && restored.result.spirv == variant.result.spirv,
        "CACHE PROOF full layout/allocation/content roundtrip mismatch");
    Require(restored.specialization == variant.specialization && restored.layout.descriptorSet == variant.layout.descriptorSet &&
        restored.layout.firstBinding == variant.layout.firstBinding && restored.layout.pushConstantOffsetBytes == variant.layout.pushConstantOffsetBytes &&
        restored.layout.pushConstantSizeBytes == variant.layout.pushConstantSizeBytes,"cache validated-key specialization/layout changed during payload decode");
    auto old = file; const std::uint32_t oldFormat = 4; std::memcpy(old.data() + 4, &oldFormat, sizeof(oldFormat));
    Require(ShaderDiskCache::DecodeEntry(old, key, restored) == ShaderDiskCache::LoadStatus::Rejected, "old cache format accepted");
    Require(ShaderDiskCache::DecodeEntry(std::span(file).first(file.size()-1), key, restored) == ShaderDiskCache::LoadStatus::Rejected, "truncated cache entry accepted");
    const auto before = ShaderDiskCache::Totals(); ShaderDiskCache::Store(key, std::make_shared<const CompiledVariant>(variant)); ShaderDiskCache::Flush();
    CompiledVariant loaded; Require(ShaderDiskCache::Load(key, loaded) && SameProofs(loaded.result.bindings, first->bindings), "CACHE PROOF disk load failed certificate match");
    auto absentKey = key; absentKey.push_back(std::byte{0x42}); CompiledVariant absent;
    Require(!ShaderDiskCache::Load(absentKey, absent), "disk absent key unexpectedly hit");
    const auto after = ShaderDiskCache::Totals(); Require(after.hits > before.hits && after.misses > before.misses && after.writes > before.writes, "actual disk counters did not record miss/write/hit");
    // Same source/request and same native driver, replacement captured descriptor
    // contents alternate policies; no source reset can hide a stale memo proof.
    AgcDriver::Metal::MetalDriver driver; const auto ranges = fixture.Ranges(); driver.Configure((__bridge void*)device, (__bridge void*)library, ranges);
    driver.RegisterShader(reinterpret_cast<const Shader*>(Base + 0x50100));
    try {
        for (const bool pixel : {false, true, true, false, true}) {
            fixture.users[16] = 0x92u | (pixel ? 1u<<15u : 0u); fixture.SetInputs(); fixture.RebuildCommands();
            const auto current = CaptureResources(request, SrtRuntime{fixture.users}); bool hit = false;
            const auto value = Recompile(request, *current, &hit);
            if (pixel) { CheckProofs(value->bindings); Require(hit && value == first, "pixel descriptor replacement failed exact memo identity"); }
            else CheckNormalizedProofs(value->bindings);
            fixture.RunConfigured(driver, false);
            std::cout << "cache descriptor replacement pixel=" << pixel << " memoHit=" << hit << " resultBytes=" << bytes.size() << " entryBytes=" << file.size() << '\n';
        }
        driver.Shutdown();
    } catch (...) { driver.Shutdown(); throw; }
    std::cout << "cache PASS format=" << ShaderDiskCache::FormatVersion << " actual misses=" << after.misses-before.misses << " writes=" << after.writes-before.writes
        << " hits=" << after.hits-before.hits << " result/full variant certificate roundtrip and native cold/hit outputs exact\n";
    const auto childDirectory = ShaderDiskCache::EntryDirectory() / ("native-process-" + std::to_string(getpid()));
    std::filesystem::create_directories(childDirectory);
    Require(setenv("ANYPS5_SHADER_CACHE_DIR", childDirectory.c_str(), 1) == 0, "cache child directory environment failed");
    SpawnCachePhase(executable, utility, "cache-cold"); SpawnCachePhase(executable, utility, "cache-hit");
}
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            Require(argc == 3, "Pixel sampler replay requires matching utility metallib and native|mixed|rejections|cache mode");
            const std::string mode = argv[2]; Require(mode == "native" || mode == "mixed" || mode == "rejections" || mode == "cache" || mode == "cache-cold" || mode == "cache-hit" || mode == "dce-limitation" || mode == "dead-sampler" || mode == "dead-sampler-cold" || mode == "dead-sampler-hit", "Unknown pixel sampler mode");
            auto device = MTLCreateSystemDefaultDevice(); Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Pixel sampler replay requires genuine Metal3");
            NSError* error = nil; auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, error ? error.localizedDescription.UTF8String : "Cannot load genuine utility metallib");
            if (mode == "native") Native(device, library); else if (mode == "mixed") Mixed(device, library);
            else if (mode == "rejections") Rejections(device, library); else if (mode == "cache") Cache(device, library, argv[0], argv[1]);
            else if (mode == "dce-limitation") DeadSourceLimitations(device);
            else if (mode == "dead-sampler") DeadSampler(device,library,argv[0],argv[1]);
            else if (mode == "dead-sampler-cold" || mode == "dead-sampler-hit") DeadSamplerPhase(device,library,mode == "dead-sampler-hit");
            else CacheNativePhase(device, library, mode == "cache-hit");
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
