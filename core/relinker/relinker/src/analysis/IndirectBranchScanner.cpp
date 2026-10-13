#include <relinker/analysis/IndirectBranchScanner.hpp>
#include <codegen/IInstructionScanner.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <algorithm>
#include <memory>

namespace Relinker {

namespace {

// Only the computed forms are trapped. `call [rip+disp]` (FF /2) and
// `jmp [rip+disp]` (FF /4) decode as the direct Call/UnconditionalJump kinds:
// their target is a GOT/PLT slot that relocations already describe, and the
// relinked PLT is dense, so trapping them would add one signal per import call
// without answering the Phase 1 question.
bool IsTracedIndirect(const Codegen::DecodedInstructionInfo& info) {
    return info.FlowKind == Codegen::ControlFlowKind::IndirectCall
        || info.FlowKind == Codegen::ControlFlowKind::IndirectJump;
}

class IndirectBranchScanner : public IIndirectBranchScanner {
public:
    std::vector<IndirectBranchSite> ScanCodeSectionForIndirectBranches(
        const std::vector<std::uint8_t>& codeSection,
        FileByteOffset codeSectionOffset,
        FileByteOffset codeSectionSize) override;
};

std::vector<IndirectBranchSite> IndirectBranchScanner::ScanCodeSectionForIndirectBranches(
    const std::vector<std::uint8_t>& codeSection,
    const FileByteOffset codeSectionOffset,
    const FileByteOffset codeSectionSize) {
    const auto scanner = Codegen::MakeInstructionScanner();
    const auto matches = scanner->ScanCodeSection(codeSection, codeSectionOffset, codeSectionSize);
    const Codegen::X64InstructionDecoder decoder;

    std::vector<IndirectBranchSite> sites;
    for (const auto& match : matches) {
        const std::size_t start = static_cast<std::size_t>(match.Offset - codeSectionOffset);
        if (start >= codeSection.size() || match.Length > codeSection.size() - start)
            continue;
        const auto info = decoder.DecodeInstruction(codeSection.data() + start, match.Length);
        if (!IsTracedIndirect(info))
            continue;
        const std::size_t opcode = start + info.OpcodeOffset;
        sites.push_back({opcode, codeSectionOffset + opcode, info.FlowKind == Codegen::ControlFlowKind::IndirectCall});
    }
    return sites;
}

class NullIndirectBranchScanner : public IIndirectBranchScanner {
public:
    std::vector<IndirectBranchSite> ScanCodeSectionForIndirectBranches(
        const std::vector<std::uint8_t>&,
        FileByteOffset,
        FileByteOffset) override { return {}; }
};

}

std::unique_ptr<IIndirectBranchScanner> MakeIndirectBranchScanner() {
    return std::make_unique<IndirectBranchScanner>();
}

std::unique_ptr<IIndirectBranchScanner> MakeNullIndirectBranchScanner() {
    return std::make_unique<NullIndirectBranchScanner>();
}

}
