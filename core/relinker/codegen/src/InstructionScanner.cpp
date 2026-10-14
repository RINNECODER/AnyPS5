#include <codegen/IInstructionScanner.hpp>
#include <codegen/CodegenException.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <string>

namespace Codegen {

class InstructionScanner : public IInstructionScanner {
public:
    [[nodiscard]] std::vector<InstructionMatch> ScanCodeSection(
        const std::vector<std::uint8_t>& codeSection,
        Domain::FileByteOffset codeSectionOffset,
        Domain::FileByteOffset codeSectionSize
    ) const override;
    void VisitCodeSection(
        const std::vector<std::uint8_t>& codeSection,
        Domain::FileByteOffset codeSectionOffset,
        Domain::FileByteOffset codeSectionSize,
        const std::function<void(const InstructionMatch&)>& visit
    ) const override;
};

void InstructionScanner::VisitCodeSection(
    const std::vector<std::uint8_t>& codeSection,
    const Domain::FileByteOffset codeSectionOffset,
    const Domain::FileByteOffset codeSectionSize,
    const std::function<void(const InstructionMatch&)>& visit
) const {
    if (codeSectionSize > std::numeric_limits<Domain::FileByteOffset>::max() - codeSectionOffset) throw CodegenException("Code section offset overflow", codeSectionOffset);
    const std::size_t limit = std::min(codeSection.size(), static_cast<std::size_t>(codeSectionSize));

    const X64InstructionDecoder decoder;
    std::size_t i = 0;

    while (i < limit) {
        const std::uint8_t* cursor = codeSection.data() + i;
        const std::size_t available = limit - i;

        std::size_t length = 0;
        try {
            length = decoder.Decode(cursor, available);
        } catch (const CodegenException& e) {
            if (std::all_of(cursor, cursor + available, [](const std::uint8_t b) { return b == 0; })) break;
            throw CodegenException(std::string("Cannot decode instruction: ") + e.what(), codeSectionOffset + i);
        }

        visit({codeSectionOffset + i, length});

        i += length;
    }
}

std::vector<InstructionMatch> InstructionScanner::ScanCodeSection(
    const std::vector<std::uint8_t>& codeSection,
    const Domain::FileByteOffset codeSectionOffset,
    const Domain::FileByteOffset codeSectionSize
) const {
    std::vector<InstructionMatch> matches;
    VisitCodeSection(codeSection, codeSectionOffset, codeSectionSize, [&](const InstructionMatch& match) { matches.push_back(match); });
    return matches;
}

std::unique_ptr<IInstructionScanner> MakeInstructionScanner() {
    return std::make_unique<InstructionScanner>();
}

}
