#ifndef CODEGEN_X86_ROSETTACPUID_HPP
#define CODEGEN_X86_ROSETTACPUID_HPP

// CPUID for guest code under Rosetta 2 (relinker --to-rosetta).
//
// Rosetta reports a family-6 Intel CPU without AVX, AVX2, FMA, BMI1/2, F16C,
// MOVBE, XSAVE/OSXSAVE, RDRAND or LZCNT, yet executes all of them; XGETBV
// already returns XCR0 = x87|SSE|AVX and leaf 0xD already describes the AVX
// save area. Guest code written for the console's Zen 2 CPU that checks
// CPUID would therefore take its SSE fallbacks. Each CPUID site jumps to this
// stub instead: it runs the real CPUID and adds the Zen 2 features Rosetta
// executes. Features Rosetta faults on stay clear: ADX (ADCX/ADOX), RDSEED
// (lowered to RDRAND anyway), SHA, XSAVEOPT/XSAVEC and AVX-512.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace Codegen::RosettaCpuid {

// Leaf 1 ECX: FMA, MOVBE, XSAVE, OSXSAVE, AVX, F16C, RDRAND.
inline constexpr std::uint32_t kLeaf1Ecx = (1u << 12) | (1u << 22) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 30);
// Leaf 7 subleaf 0 EBX: BMI1, AVX2, BMI2.
inline constexpr std::uint32_t kLeaf7Ebx = (1u << 3) | (1u << 5) | (1u << 8);
// Leaf 0x80000001 ECX: ABM (LZCNT).
inline constexpr std::uint32_t kExtendedLeaf1Ecx = 1u << 5;

// The stub body (without the return jump). It preserves RFLAGS and stays
// below the red zone, like the other Rosetta stubs.
inline std::vector<std::uint8_t> StubBody() {
    std::vector<std::uint8_t> out;
    const auto bytes = [&](std::initializer_list<std::uint8_t> list) { out.insert(out.end(), list); };
    const auto imm32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    // Short forward branches: remember the displacement byte, patch it at the label.
    std::vector<std::size_t> toDone;
    const auto branch = [&](std::uint8_t opcode) {
        out.push_back(opcode);
        out.push_back(0);
        return out.size() - 1;
    };
    const auto land = [&](std::size_t displacement) { out[displacement] = static_cast<std::uint8_t>(out.size() - displacement - 1); };

    bytes({0x48, 0x8D, 0xA4, 0x24, 0x70, 0xFF, 0xFF, 0xFF}); // lea rsp, [rsp - 0x90]
    bytes({0x9C});                                           // pushfq
    bytes({0x3D}); imm32(1);                                 // cmp eax, 1
    const auto toLeaf1 = branch(0x74);                       // je leaf1
    bytes({0x3D}); imm32(7);                                 // cmp eax, 7
    const auto toLeaf7 = branch(0x74);                       // je leaf7
    bytes({0x3D}); imm32(0x80000001u);                       // cmp eax, 0x80000001
    const auto toExtended = branch(0x74);                    // je extended
    bytes({0x0F, 0xA2});                                     // cpuid
    toDone.push_back(branch(0xEB));                          // jmp done

    land(toLeaf1);
    bytes({0x0F, 0xA2});                                     // cpuid
    bytes({0x81, 0xC9}); imm32(kLeaf1Ecx);                   // or ecx, kLeaf1Ecx
    toDone.push_back(branch(0xEB));

    land(toLeaf7);
    bytes({0x85, 0xC9});                                     // test ecx, ecx (CPUID leaves flags alone)
    bytes({0x0F, 0xA2});                                     // cpuid
    toDone.push_back(branch(0x75));                          // jnz done: subleaf != 0
    bytes({0x81, 0xCB}); imm32(kLeaf7Ebx);                   // or ebx, kLeaf7Ebx
    toDone.push_back(branch(0xEB));

    land(toExtended);
    bytes({0x0F, 0xA2});                                     // cpuid
    bytes({0x81, 0xC9}); imm32(kExtendedLeaf1Ecx);           // or ecx, kExtendedLeaf1Ecx

    for (const auto displacement : toDone) land(displacement);
    bytes({0x9D});                                           // popfq
    bytes({0x48, 0x8D, 0xA4, 0x24, 0x90, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 0x90]
    return out;
}

}

#endif
