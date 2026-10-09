#include "prx/libSceAgcDriver/Execution/include/ComputeDispatch.hpp"
#include "SceShaders.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace AgcDriver::Graphics {

ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader, std::span<const std::byte> header,
                                                               void (*noteRegisterRead)(std::uint32_t)) {
    const auto read = [&](std::uint32_t offset) {
        if (noteRegisterRead != nullptr) noteRegisterRead(offset);
        const auto found = shader.find(offset);
        if (found == shader.end()) {
            char text[64];
            std::snprintf(text, sizeof(text), "AGC graphics: missing register at DWORD 0x%x", offset);
            throw std::runtime_error(text);
        }
        return found->second;
    };
    const auto numThreadX = read(0x207);
    const auto numThreadY = read(0x208);
    const auto numThreadZ = read(0x209);
    if (numThreadX == 0 || numThreadY == 0 || numThreadZ == 0) throw std::runtime_error("AGC graphics: COMPUTE_NUM_THREAD_X/Y/Z must be nonzero");
    const auto rsrc2 = read(0x213);
    std::uint32_t scratchDwords = 0;
    if ((rsrc2 & 0x1u) != 0) {
        if (header.size() < sizeof(Shader)) throw std::runtime_error("AGC graphics: COMPUTE_PGM_RSRC2.SCRATCH_EN without an AGC shader header");
        Shader agcShader;
        std::memcpy(&agcShader, header.data(), sizeof(Shader));
        scratchDwords = agcShader.scratch_size_dw_per_thread;
        if (scratchDwords == 0) throw std::runtime_error("AGC graphics: COMPUTE_PGM_RSRC2.SCRATCH_EN with a zero scratch size");
    }
    static const std::uint32_t ldsSlack = [] { const char* text = std::getenv("APS5_LDS_SLACK"); return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0)) : 0u; }();
    return ShaderRecompiler::ShaderComputeStageInfo{
        {numThreadX, numThreadY, numThreadZ},
        std::min(((rsrc2 >> 15u) & 0x1FFu) * 128u + ldsSlack, 16384u),
        {((rsrc2 >> 7u) & 0x1u) != 0, ((rsrc2 >> 8u) & 0x1u) != 0, ((rsrc2 >> 9u) & 0x1u) != 0},
        ((rsrc2 >> 10u) & 0x1u) != 0,
        ((rsrc2 >> 11u) & 0x3u) + 1u,
        {},
        scratchDwords
    };
}

}
