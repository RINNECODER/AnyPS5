#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HOSTINTERPOLATIONCHECKER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HOSTINTERPOLATIONCHECKER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"

namespace ShaderRecompiler {

class HostInterpolationChecker {
public:
    // barycentricWeights: the target reads BaryCoordKHR weights natively but has no per-vertex
    // inputs (Metal [[barycentric_coord]]), so raw I/J reads may stay next to host interpolation.
    [[nodiscard]] bool Lower(IrProgram& program, const ShaderPixelInputInfo& pixel, bool barycentricWeights = false) const;
};

}

#endif
