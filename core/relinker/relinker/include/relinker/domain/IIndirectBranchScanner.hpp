#ifndef RELINKER_DOMAIN_IINDIRECTBRANCHSCANNER_HPP
#define RELINKER_DOMAIN_IINDIRECTBRANCHSCANNER_HPP

#include <relinker/domain/Types.hpp>
#include <cstddef>
#include <vector>

namespace Relinker {

// One computed (non-RIP-relative) indirect branch site: `call r/m64` or
// `jmp r/m64`. Address is the guest virtual address of the opcode byte, the
// byte the tracer rewrites to `int3` (the rest of the instruction becomes NOPs
// so the relinker's own linear decode stays in sync).
struct IndirectBranchSite {
    std::size_t InstructionOffset;
    std::size_t OpcodeOffset;
    std::size_t Length;
    VirtualAddress Address;
    bool IsCall;
};

class IIndirectBranchScanner {
public:
    virtual ~IIndirectBranchScanner() = default;

    virtual std::vector<IndirectBranchSite> ScanCodeSectionForIndirectBranches(
        const std::vector<std::uint8_t>& codeSection,
        FileByteOffset codeSectionOffset,
        FileByteOffset codeSectionSize
    ) = 0;
};

}

#endif
