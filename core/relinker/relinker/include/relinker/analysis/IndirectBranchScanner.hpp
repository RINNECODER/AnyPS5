#ifndef RELINKER_ANALYSIS_INDIRECTBRANCHSCANNER_HPP
#define RELINKER_ANALYSIS_INDIRECTBRANCHSCANNER_HPP

#include <relinker/domain/IIndirectBranchScanner.hpp>
#include <memory>

namespace Relinker {

// Opt-in (ANYPS5_TRACE_INDIRECT) scanner for the Phase 1 dynamic check. It finds
// the computed indirect branches the runtime traps with `int3` and reports their
// opcode offsets so the pipeline can rewrite them.
std::unique_ptr<IIndirectBranchScanner> MakeIndirectBranchScanner();

std::unique_ptr<IIndirectBranchScanner> MakeNullIndirectBranchScanner();

}

#endif
