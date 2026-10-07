#import <Foundation/Foundation.h>
#include "MetalShaderPipeline.hpp"
#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;
using namespace AgcDriver::Metal;
constexpr std::uint32_t Wave = 32, GuardWords = 16, InputWords = 4, OutputWords = 16;
constexpr std::uint32_t Endpgm = 0xbf810000, Nop = 0xbf800000, Setkill = 0xbf8b0000;
enum class Flow { Direct, ReachableBranch, BypassBranch };

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string Hex(std::uint32_t value) {
    char result[16]; std::snprintf(result, sizeof(result), "0x%08x", value); return result;
}
const char* Name(Flow flow) {
    switch (flow) {
        case Flow::Direct: return "direct";
        case Flow::ReachableBranch: return "branch-to-termination";
        case Flow::BypassBranch: return "branch-bypasses-termination";
    }
    throw std::runtime_error("invalid fixture flow");
}
template<class Operation> void Reject(Operation operation, const std::string& diagnostic, const char* name) {
    try { operation(); }
    catch (const std::exception& error) {
        Require(std::string(error.what()).find(diagnostic) != std::string::npos,
            std::string(name) + " rejected for the wrong reason: " + error.what());
        return;
    }
    throw std::runtime_error(std::string(name) + " was silently accepted");
}

std::uint32_t Input(std::uint32_t lane, std::uint32_t component, std::uint32_t salt) {
    return 0x10203040u + lane * 0x01010101u + component * 0x17395171u + salt;
}
std::uint32_t Untouched(std::uint32_t lane, std::uint32_t component) {
    return 0xdeadbeefu ^ (lane * 0x01001001u + component * 0x00010307u);
}
std::vector<std::uint32_t> Program(std::uint32_t termination, Flow flow) {
    std::vector<std::uint32_t> code{
        0x7e020208, 0x34020285, 0x4a000101,
        0x34020084, 0x34060086,
        0xe0301000, 0x80000401, 0xbf8c3f70,
        0x4a0a0881, 0x4a0c0882,
        0xe0701000, 0x80010503};
    if (flow != Flow::Direct) {
        code.push_back(0xbf068000);
        code.push_back(flow == Flow::BypassBranch ? 0xbf840001 : 0xbf840002);
        if (flow == Flow::ReachableBranch) {
            code.push_back(0xe0701008);
            code.push_back(0x80010603);
        }
    }
    code.push_back(termination);
    code.push_back(0xe0701004);
    code.push_back(0x80010603);
    code.push_back(Endpgm);
    return code;
}

void DecodeContract() {
    for (std::uint32_t immediate = 0; immediate <= 0xffffu; ++immediate) {
        const std::array<std::uint32_t, 1> code{Setkill | immediate};
        const auto instruction = DecodeRdnaInstruction(0, code, 0);
        Require(instruction.op == RdnaOpcode::SEndpgm && instruction.wordCount == 1 &&
            instruction.rawWords[0] == code[0], "S_SETKILL immediate " + Hex(immediate) + " lost scalar termination");
    }
    for (const auto opcode : {0x0du, 0x11u, 0x1fu}) {
        const std::array<std::uint32_t, 2> code{0xbf800000u | (opcode << 16), Endpgm};
        Reject([&] { static_cast<void>(RdnaInstructionDecoder{}.Decode(code)); },
            "unsupported SOPP opcode " + std::to_string(opcode), "unsupported halt/code-end opcode");
    }
    const std::array<std::uint32_t, 1> trap{0xbf920000};
    Require(DecodeRdnaInstruction(0, trap, 0).op == RdnaOpcode::STrap, "existing S_TRAP decode changed");
    const std::array<std::uint32_t, 1> nop{Nop};
    Require(DecodeRdnaInstruction(0, nop, 0).op == RdnaOpcode::SNop, "existing S_NOP decode changed");
    for (const auto termination : {Endpgm, Setkill, Setkill | 1u, Setkill | 0xffffu}) {
        const std::array<std::uint32_t, 2> ends{termination, 0xffffffff};
        const auto program = RdnaInstructionDecoder{}.Decode(ends);
        Require(program.instructions.size() == 1, "unreachable invalid words after scalar termination were decoded");
        Reject([&] { static_cast<void>(DecodeRdnaFrontProgram(ends)); },
            "front program reached s_endpgm before s_setpc_b64", "front termination before transfer");
        const std::array<std::uint32_t, 3> outOfBounds{0xbf820002, termination, Endpgm};
        Reject([&] { static_cast<void>(RdnaInstructionDecoder{}.Decode(outOfBounds)); },
            "branch target is out of the code span bounds", "one-past-span branch");
        const std::array<std::uint32_t, 2> missingTargetEnd{0xbf820001, termination};
        Reject([&] { static_cast<void>(RdnaInstructionDecoder{}.Decode(missingTargetEnd)); },
            "branch target is out of the code span bounds", "branch beyond termination span");
        const std::array<std::uint32_t, 3> missingEnd{0xbf820001, termination, Nop};
        Reject([&] { static_cast<void>(RdnaInstructionDecoder{}.Decode(missingEnd)); },
            "code boundary before s_endpgm", "reachable successor without termination");
    }
    const std::array<std::uint32_t, 1> literal{0x7e0002ff};
    Reject([&] { static_cast<void>(RdnaInstructionDecoder{}.Decode(literal)); }, "literal", "truncated literal");
    Reject([&] { static_cast<void>(DecodeRdnaInstruction(0, {}, 0)); },
        "word index is out of the code span bounds", "empty instruction span");
    std::cout << "decode PASS: 65536 immediates; unsupported SOPP 13/17/31; preserved S_NOP/S_TRAP; front/branch/literal bounds\n";
}

std::array<std::uint32_t, 4> Descriptor(std::uint32_t address, std::uint32_t bytes) {
    return {address, 0, bytes, 0x01016fac};
}
MetalBackend::Result Compile(id<MTLDevice> device, std::span<const std::uint32_t> code,
    std::span<const std::uint32_t> userData) {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto limit = device.maxThreadsPerThreadgroup;
    const SpirvTarget target{0x00401000, 0x00010300, Wave, BdaAbi::Version, capabilities, extensions, false,
        {static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(limit.height), static_cast<std::uint32_t>(limit.depth)},
        static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<MemoryRegion, 1> regions{{{address, std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{Wave, 1, 1}, 0, {true, false, false}, false, 1};
    RecompileRequest request{{ShaderStage::Compute, address, code, 0, {}},
        {Wave, 0, userData, compute, {}, {}, regions}, target, {0, 0, 0, 128}};
    request.useCache = false;
    auto guest = Recompile(request);
    Require(!guest.spirv.empty(), "scalar termination frontend produced no SPIR-V");
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true; options.supportsGpuAddresses = true; options.supportsSimdGroups = true;
    return MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
}

void NativeCase(id<MTLDevice> device, id<MTLCommandQueue> queue, std::uint32_t lanes,
    std::uint32_t termination, Flow flow) {
    const std::string label = Hex(termination) + " " + Name(flow) + " lanes=" + std::to_string(lanes);
    const auto salt = (termination & 0xffffu) * 97u + static_cast<std::uint32_t>(flow) * 7919u;
    std::vector<std::uint32_t> input(lanes * InputWords + GuardWords * 2);
    std::vector<std::uint32_t> output(lanes * OutputWords + GuardWords * 2);
    for (std::size_t word = 0; word < input.size(); ++word) input[word] = 0xa6b5c4d3u ^ static_cast<std::uint32_t>(word * 31);
    for (std::size_t word = 0; word < output.size(); ++word) output[word] = 0x76543210u ^ static_cast<std::uint32_t>(word * 37);
    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
        for (std::uint32_t component = 0; component < InputWords; ++component)
            input[GuardWords + lane * InputWords + component] = Input(lane, component, salt);
        for (std::uint32_t component = 0; component < OutputWords; ++component)
            output[GuardWords + lane * OutputWords + component] = Untouched(lane, component);
    }
    const auto initialOutput = output;
    auto inBuffer = [device newBufferWithBytes:input.data() length:input.size() * 4 options:MTLResourceStorageModeShared];
    auto outBuffer = [device newBufferWithBytes:output.data() length:output.size() * 4 options:MTLResourceStorageModeShared];
    Require(inBuffer && outBuffer, label + " buffer allocation failed");
    const auto inDescriptor = Descriptor(0x100000, lanes * InputWords * 4);
    const auto outDescriptor = Descriptor(0x200000, lanes * OutputWords * 4);
    std::array<std::uint32_t, 8> userData{};
    std::copy(inDescriptor.begin(), inDescriptor.end(), userData.begin());
    std::copy(outDescriptor.begin(), outDescriptor.end(), userData.begin() + 4);
    const auto program = Program(termination, flow);
    std::vector<std::uint32_t> guardedCode(GuardWords * 2 + program.size(), 0xffffffff);
    std::copy(program.begin(), program.end(), guardedCode.begin() + GuardWords);
    const auto originalCode = guardedCode;
    const auto code = std::span<const std::uint32_t>(guardedCode).subspan(GuardWords, program.size());
    auto shader = Compile(device, code, userData);
    MetalComputePipeline pipeline(device, shader);
    std::vector<MetalShaderResourceBinding> bindings;
    bool boundInput = false, boundOutput = false;
    for (const auto& binding : shader.guest.bindings) {
        Require(binding.role == DescriptorRole::GuestBuffers && binding.guestDescriptor.size() == binding.count * 4u,
            label + " reflected an unexpected descriptor");
        MetalShaderResourceBinding native{binding.descriptorSet, binding.binding};
        for (std::uint32_t index = 0; index < binding.count; ++index) {
            const auto descriptor = binding.guestDescriptor.begin() + index * 4;
            const bool isInput = std::equal(inDescriptor.begin(), inDescriptor.end(), descriptor);
            const bool isOutput = std::equal(outDescriptor.begin(), outDescriptor.end(), descriptor);
            Require(isInput != isOutput, label + " reflected an unknown descriptor");
            native.buffers.push_back({isInput ? inBuffer : outBuffer, GuardWords * 4,
                lanes * (isInput ? InputWords : OutputWords) * 4});
            boundInput |= isInput; boundOutput |= isOutput;
        }
        bindings.push_back(std::move(native));
    }
    Require(boundInput && boundOutput, label + " missing native input/output bindings");
    auto command = [queue commandBuffer];
    Require(command != nil, label + " command allocation failed");
    pipeline.Encode(command, bindings, MTLSizeMake(lanes, 1, 1));
    [command commit]; [command waitUntilCompleted];
    Require(command.status == MTLCommandBufferStatusCompleted,
        label + " Metal command failed: " + (command.error.localizedDescription.UTF8String ?: "unknown"));
    Require(std::memcmp(inBuffer.contents, input.data(), input.size() * 4) == 0, label + " input/guard modified");
    Require(guardedCode == originalCode, label + " code/guard modified");
    std::memcpy(output.data(), outBuffer.contents, output.size() * 4);
    for (std::uint32_t word = 0; word < GuardWords; ++word)
        Require(output[word] == initialOutput[word] && output[output.size() - GuardWords + word] == initialOutput[output.size() - GuardWords + word],
            label + " output guard modified");
    const bool writesSuccessor = termination == Nop || flow == Flow::BypassBranch;
    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
        for (std::uint32_t component = 0; component < OutputWords; ++component) {
            const auto expected = component == 0 ? Input(lane, 0, salt) + 1u :
                component == 1 && writesSuccessor ? Input(lane, 0, salt) + 2u : Untouched(lane, component);
            const auto actual = output[GuardWords + lane * OutputWords + component];
            Require(actual == expected, label + " " + (component == 1 && !writesSuccessor ? "POST-TERMINATION SENTINEL" : "output") +
                " lane=" + std::to_string(lane) + " word=" + std::to_string(component) + " actual=" + Hex(actual) + " expected=" + Hex(expected));
        }
    }
    std::cout << label << " PASS: Completed; pre-store golden, successor " << (writesSuccessor ? "golden" : "sentinel")
        << ", skipped poison/unused words and input/output/code guards\n";
}

void NativeContract() {
    auto device = MTLCreateSystemDefaultDevice(); Require(device != nil, "No Metal device available");
    auto queue = [device newCommandQueue]; Require(queue != nil, "No Metal command queue available");
    std::uint32_t completed = 0;
    for (const auto lanes : {32u, 64u, 256u}) {
        for (const auto termination : {Setkill, Setkill | 1u, Setkill | 0xffffu, Endpgm, Nop}) {
            for (const auto flow : {Flow::Direct, Flow::ReachableBranch, Flow::BypassBranch}) {
                NativeCase(device, queue, lanes, termination, flow); ++completed;
            }
        }
    }
    std::cout << "native PASS: " << completed << " frontend->SPIRV->MSL->MTL commands Completed on "
        << device.name.UTF8String << "; wave32, 32/64/256 lanes. KILL-status and PS5/gfx1035 parity unqualified.\n";
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const std::string mode = argc > 1 ? argv[1] : "all";
            Require(mode == "all" || mode == "native" || mode == "decode", "Unknown scalar termination mode");
            if (mode == "all" || mode == "native") NativeContract();
            if (mode == "all" || mode == "decode") DecodeContract();
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
