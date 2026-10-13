#ifndef RELINKER_DOMAIN_IINDIRECTBRANCHSCANNER_HPP
#define RELINKER_DOMAIN_IINDIRECTBRANCHSCANNER_HPP

#include <relinker/domain/Types.hpp>
#include <cstddef>
#include <vector>

namespace Relinker {

// One computed (non-RIP-relative) indirect branch site: `call r/m64` or
// `jmp r/m64`. Address is the guest virtual address of the opcode byte, the
// byte the tracer rewrites to `int3`.
struct IndirectBranchSite {
    std::size_t OpcodeOffset;
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
