// Decode check for the high-half result select of v_interp_p2_f16 (op_sel[3]), ported from the
// block that upstream PR #5471 adds to its VOP3 interpolation decode test.
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

using namespace ShaderRecompiler;

int main() {
    constexpr std::uint32_t Destination = 5u;
    int failures = 0;
    for (const std::uint32_t selector : {0x0u, 0x4000u}) {
        const std::vector<std::uint32_t> code{0xd75a0005u | selector, 0x041e0201u, 0xbf810000u};
        try {
            const auto decoded = RdnaInstructionDecoder{}.Decode(code);
            const auto& instruction = decoded.instructions.at(0);
            const std::uint32_t half = selector != 0u ? 5u : 6u;
            if (instruction.op != RdnaOpcode::VInterpP2F16 || instruction.destination.reg != Destination || instruction.destination.sdwaSel != half ||
                instruction.sourceCount != 4u || instruction.source0.reg != 1u || instruction.source1.value != 1u || instruction.source3.reg != 7u) {
                std::fprintf(stderr, "v_interp_p2_f16 v5, v1, attr1.x, v7%s decoded to the wrong opcode or operands\n", selector != 0u ? " op_sel:[0,0,0,1]" : "");
                ++failures;
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "v_interp_p2_f16 v5, v1, attr1.x, v7%s was refused: %s\n", selector != 0u ? " op_sel:[0,0,0,1]" : "", error.what());
            ++failures;
        }
    }
    // The 32-bit-result forms keep refusing the bit.
    try {
        (void)RdnaInstructionDecoder{}.Decode(std::vector<std::uint32_t>{0xd7424005u, 0x00020201u, 0xbf810000u});
        std::fprintf(stderr, "v_interp_p1ll_f16 with the high-half destination selector was accepted\n");
        ++failures;
    } catch (const std::exception&) {
    }
    if (failures == 0) std::puts("v_interp_p2_f16 high-half decode passed");
    return failures == 0 ? 0 : 1;
}
