#pragma once

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace AgcDriver {

struct IndirectDrawProgram {
    ShaderRecompiler::ProgramRole role;
    std::uint32_t userDataBase;
    std::uint32_t firstUserSgpr;
    std::span<std::uint32_t> userData;
};

struct ExpandedIndirectDraw {
    Pm4::DrawParameters parameters;
    std::set<std::size_t> patchedPrograms;
};

void MarkIndirectDrawSgprs(Pm4::DrawParameters::IndirectDraw& indirect, std::span<const IndirectDrawProgram> programs);
std::vector<Pm4::DrawArguments> ReadIndirectDrawRecords(const Pm4::DrawParameters::IndirectDraw& indirect);
std::optional<ExpandedIndirectDraw> ExpandIndirectDrawRecord(
    const Pm4::DrawParameters& draw, const Pm4::DrawArguments& arguments,
    std::uint32_t record, std::span<const IndirectDrawProgram> programs);

}
