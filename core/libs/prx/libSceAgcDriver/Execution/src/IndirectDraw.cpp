#include "prx/libSceAgcDriver/Execution/include/IndirectDraw.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace AgcDriver {
namespace {

std::optional<std::pair<std::size_t, std::size_t>> Locate(std::uint32_t location, std::span<const IndirectDrawProgram> programs) {
    if (location == 0x280u) return std::nullopt;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        if (program.role == ShaderRecompiler::ProgramRole::Fragment || program.role == ShaderRecompiler::ProgramRole::GeometryBack || location < program.userDataBase) continue;
        const auto word = location - program.userDataBase + (8u - program.firstUserSgpr);
        if (word < program.userData.size()) return std::make_pair(i, static_cast<std::size_t>(word));
    }
    return std::nullopt;
}

}

void MarkIndirectDrawSgprs(Pm4::DrawParameters::IndirectDraw& indirect, std::span<const IndirectDrawProgram> programs) {
    const auto sgprOf = [&](std::uint32_t location) -> std::int32_t {
        const auto word = Locate(location, programs);
        if (!word || word->first != 0) return -1;
        return static_cast<std::int32_t>(programs.front().firstUserSgpr + word->second);
    };
    indirect.baseVertexSgpr = sgprOf(indirect.baseVertexLocation);
    indirect.startInstanceSgpr = sgprOf(indirect.startInstanceLocation);
    indirect.drawIndexSgpr = sgprOf(indirect.drawIndexLocation);
}

std::vector<Pm4::DrawArguments> ReadIndirectDrawRecords(const Pm4::DrawParameters::IndirectDraw& indirect) {
    const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
    std::vector<Pm4::DrawArguments> records;
    for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(indirect, record));
    return records;
}

std::optional<ExpandedIndirectDraw> ExpandIndirectDrawRecord(
    const Pm4::DrawParameters& draw, const Pm4::DrawArguments& arguments,
    std::uint32_t record, std::span<const IndirectDrawProgram> programs) {
    if (!draw.indirect) throw std::invalid_argument("indirect draw record expansion requires indirect parameters");
    const auto& indirect = *draw.indirect;
    if (record >= indirect.count) throw std::invalid_argument("indirect draw record index exceeds the packet's count");
    if (arguments.count == 0 || arguments.instances == 0) return std::nullopt;
    ExpandedIndirectDraw expanded{{0, arguments.count, 0, arguments.instances, draw.flags, draw.indexed, 0, 0}, {}};
    const auto patch = [&](std::uint32_t location, std::uint32_t value) {
        const auto word = Locate(location, programs);
        if (!word) return;
        programs[word->first].userData[word->second] = value;
        expanded.patchedPrograms.insert(word->first);
    };
    patch(indirect.baseVertexLocation, indirect.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex);
    patch(indirect.startInstanceLocation, arguments.firstInstance);
    if (indirect.drawIndexEnabled) patch(indirect.drawIndexLocation, record);
    if (draw.indexed) {
        if (arguments.firstVertexOrIndex >= draw.indexCount) return std::nullopt;
        expanded.parameters.indexAddress = draw.indexAddress + static_cast<std::uint64_t>(arguments.firstVertexOrIndex) * draw.indexSize;
        expanded.parameters.indexCount = std::min(arguments.count, draw.indexCount - arguments.firstVertexOrIndex);
        expanded.parameters.indexSize = draw.indexSize;
    } else {
        expanded.parameters.firstVertex = indirect.indxOffset;
    }
    return expanded;
}

}
