#include <relinker/analysis/IndirectBranchScanner.hpp>
#include <codegen/IInstructionScanner.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>

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
    IndirectBranchScanner() {
        // The load runs untraced by leaving the sites it hammers alone
        // (ANYPS5_TRACE_INDIRECT_EXCLUDE, one hex opcode address per line).
        const char* path = std::getenv("ANYPS5_TRACE_INDIRECT_EXCLUDE");
        if (path != nullptr && *path != '\0') {
            std::ifstream stream(path);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty()) continue;
                _exclude.insert(static_cast<VirtualAddress>(std::strtoull(line.c_str(), nullptr, 16)));
            }
        }
        // Function entries to record (ANYPS5_TRACE_ENTRY, one hex address per line).
        const char* entries = std::getenv("ANYPS5_TRACE_ENTRY");
        if (entries != nullptr && *entries != '\0') {
            std::ifstream stream(entries);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty()) continue;
                _entries.insert(static_cast<VirtualAddress>(std::strtoull(line.c_str(), nullptr, 16)));
            }
        }
    }

    std::vector<IndirectBranchSite> ScanCodeSectionForIndirectBranches(
        const std::vector<std::uint8_t>& codeSection,
        FileByteOffset codeSectionOffset,
        FileByteOffset codeSectionSize) override;

private:
    std::set<VirtualAddress> _exclude;
    std::set<VirtualAddress> _entries;
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
        const VirtualAddress address = codeSectionOffset + opcode;
        if (!_exclude.empty() && _exclude.count(address) != 0) continue;
        sites.push_back({start, opcode, match.Length, address, info.FlowKind == Codegen::ControlFlowKind::IndirectCall});
    }
    // Function entries: only a `push r64` prologue is trapped and emulated by the
    // runtime (the register state at the entry is what the recorder wants).
    for (const VirtualAddress entry : _entries) {
        if (entry < codeSectionOffset || entry >= codeSectionOffset + codeSectionSize) continue;
        const std::size_t start = static_cast<std::size_t>(entry - codeSectionOffset);
        if (start >= codeSection.size()) continue;
        const std::size_t available = std::min<std::size_t>(15, codeSection.size() - start);
        const auto info = decoder.DecodeInstruction(codeSection.data() + start, available);
        if (info.IsTwoByteOpcode || info.Opcode < 0x50 || info.Opcode > 0x57) continue;
        const std::size_t opcode = start + info.OpcodeOffset;
        sites.push_back({start, opcode, info.Length, entry, false, true});
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
