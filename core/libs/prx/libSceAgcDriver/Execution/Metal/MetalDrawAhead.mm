#include "MetalDrawAhead.hpp"
#include "MetalWorkerPool.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalDraw.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalRenderState.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPipelineCache.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderPipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <spirv/unified1/spirv.hpp>

namespace AgcDriver::Metal {
using DriverDetail::Submission;
namespace {
using namespace ShaderRecompiler;

std::uint32_t PositionExportLayout(std::span<const std::uint32_t> code, std::uint32_t paClVsOutCntl) {
    const auto bits = paClVsOutCntl & Graphics::PositionExportLayoutBits;
    if (bits == 0) return 0;
    const auto decoded = RdnaInstructionDecoder{}.Decode(code);
    const bool used = std::any_of(decoded.instructions.begin(), decoded.instructions.end(), [](const auto& instruction) {
        return instruction.family == RdnaInstructionFamily::EXP && instruction.exportTarget >= 0x0du && instruction.exportTarget <= 0x0fu && instruction.exportEnableMask != 0;
    });
    return used ? bits : 0;
}

SpirvTarget nativeTarget(id<MTLDevice> device) {
    static constexpr std::array<std::uint32_t, 8> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityMinLod, spv::CapabilitySampledImageArrayDynamicIndexing,
        spv::CapabilityShaderNonUniform, spv::CapabilitySampledImageArrayNonUniformIndexing, spv::CapabilityFragmentBarycentricKHR};
    const auto supported = [&](std::span<const std::uint32_t> all) {
        return device.supportsShaderBarycentricCoordinates ? all : all.first(all.size() - 1u);
    };
    static constexpr std::array<std::string_view, 3> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage", "SPV_EXT_descriptor_indexing"};
    const auto maximum = device.maxThreadsPerThreadgroup;
    SpirvTarget target{0x00401000, 0x00010300, 32, BdaAbi::Version, supported(capabilities), extensions, false,
        {static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height), static_cast<std::uint32_t>(maximum.depth)},
        static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
    return target;
}

void configureSamplerArguments(MetalBackend::TargetOptions& options, const RecompileResult& guest, id<MTLDevice> device) {
    options.supportsArgumentBuffersTier2 = device.argumentBuffersSupport == MTLArgumentBuffersTier2;
    options.maxArgumentBufferSamplers = 32;
    options.samplerArgumentBuffer = false;
    std::uint32_t count = 0;
    for (const auto& binding : guest.bindings) {
        if (binding.kind != DescriptorKind::Sampler) continue;
        if (binding.count > options.maxSamplers - count) {
            options.samplerArgumentBuffer = true;
            break;
        }
        count += binding.count;
    }
}

void PrewarmDraw(
    const DrawDispatchState& decoded,
    const Pm4::DrawParameters& parameters,
    MetalDraw* /*draw*/,
    id<MTLDevice> device) {
    if (decoded.programs.size() < 2) return;
    try {
        auto target = nativeTarget(device);
        std::vector<MemoryRegion> memory;
        std::vector<LinkedProgram> linked;
        for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
            const auto& program = decoded.programs[i];
            memory.insert(memory.end(), program.memory.begin(), program.memory.end());
            linked.push_back({decoded.roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
        }
        ShaderMemory capture(memory);
        std::vector<RecompileResult> programs;
        programs.reserve(decoded.programs.size());
        std::vector<Graphics::CompiledShader> stages;
        stages.reserve(decoded.programs.size());
        std::uint32_t pushOffset = 0;

        for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
            if (decoded.roles[i] == ProgramRole::GeometryBack) continue;
            const auto& program = decoded.programs[i];
            std::optional<ShaderVertexStageInfo> vertex;
            if (program.binary.stage != ShaderStage::Fragment) {
                vertex = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData);
                if (vertex) {
                    vertex->paClVsOutCntl = PositionExportLayout(program.binary.code, decoded.state.paClVsOutCntl);
                    if ((vertex->paClVsOutCntl & 0xff00u) != 0) return;
                }
            }
            auto request = BuildDrawRecompileRequest(program.binary, program.firstUserSgpr, program.userData,
                decoded.state, decoded.pixel, vertex, target, pushOffset, parameters, memory, linked);
            request.context.floatMode = DriverDetail::HeaderFloatMode(program.snapshot->type,
                program.snapshot->headerAddress, program.snapshot->header);
            const auto resources = capture.Capture(request);
            memory = capture.Regions();
            request.context.memory = memory;
            programs.push_back(*Recompile(request, *resources));
            const auto& result = programs.back();
            stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushOffset});
            pushOffset += static_cast<std::uint32_t>(result.pushConstants.size());
        }

        const auto* vertexStage = &stages[0];
        const auto* fragmentStage = &stages[1];
        if (stages[0].stage != ShaderStage::Vertex && stages[1].stage == ShaderStage::Vertex) {
            std::swap(vertexStage, fragmentStage);
        }

        MetalBackend::TargetOptions vTarget;
        configureSamplerArguments(vTarget, *vertexStage->program, device);
        auto nativeVertex = MetalBackend::ConvertToMetal(*vertexStage->program, ShaderStage::Vertex, vTarget);

        MetalBackend::TargetOptions fTarget;
        configureSamplerArguments(fTarget, *fragmentStage->program, device);
        auto nativeFragment = MetalBackend::ConvertToMetal(*fragmentStage->program, ShaderStage::Fragment, fTarget);

        auto pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        ConfigureRenderPipelineDescriptor(pipelineDescriptor, decoded.state);

        auto vertexPipeline = std::make_shared<MetalRenderPipeline>(
            device, std::move(nativeVertex), std::move(nativeFragment), pipelineDescriptor);

        if (decoded.state.depthTest || decoded.state.stencilTest) {
            static_cast<void>(CreateDepthStencilState(device, decoded.state));
        }
        static_cast<void>(vertexPipeline);
    } catch (...) {
    }
}

}

bool MetalDrawAhead::Enabled() {
    static const bool enabled = [] {
        const char* v1 = std::getenv("ANYPS5_METAL_SCAN_AHEAD");
        if (v1 != nullptr && (*v1 == '0' || std::strcmp(v1, "false") == 0 || std::strcmp(v1, "off") == 0 || std::strcmp(v1, "no") == 0)) return false;
        const char* v2 = std::getenv("APS5_NO_DRAW_AHEAD");
        if (v2 != nullptr && std::strcmp(v2, "0") != 0) return false;
        return true;
    }();
    return enabled;
}

void MetalDrawAhead::Post(const Submission& submission, const QueueState& state, MetalDraw* draw, id<MTLDevice> device) {
    if (!Enabled() || draw == nullptr || device == nil || submission.shaders == nullptr || submission.commands.empty()) {
        return;
    }
    const auto serial = submission.serial;
    activeSerial.store(serial, std::memory_order_release);

    MetalWorkerPool::Shared().Enqueue([this, serial, commands = submission.commands, state, shaders = submission.shaders, draw, device] {
        if (retiredSerial.load(std::memory_order_acquire) >= serial) return;
        QueueState scanState = state;
        const auto totalWords = commands.size();

        for (std::size_t cursor = 0; cursor < totalWords;) {
            if (retiredSerial.load(std::memory_order_acquire) >= serial) break;
            const auto header = commands[cursor];
            const auto count = Pm4::PacketWords(header);
            if (count == 0 || count > totalWords - cursor) break;

            const auto opcode = (header >> 8u) & 0xffu;
            const auto packet = std::span(commands).subspan(cursor, count);
            const auto next = cursor + count;

            if (Pm4::FillerPacket(header) || Pm4::PureNopPacket(header)) {
                cursor = next;
                continue;
            }

            if (opcode == 0x69 || opcode == 0x76 || opcode == 0x79 || opcode == 0x7a ||
                opcode == 0x10 || opcode == 0x12 || opcode == 0x11 || opcode == 0x13 ||
                opcode == 0x20 || opcode == 0x26 || opcode == 0x28 || opcode == 0x2a ||
                opcode == 0x2f || opcode == 0x59 || opcode == 0x81) {
                try {
                    Pm4::Execute(packet, scanState);
                } catch (...) {
                }
            } else if (Pm4::DrawOpcode(opcode) && !Pm4::IndirectDrawOpcode(opcode)) {
                publishedCount.fetch_add(1, std::memory_order_relaxed);
                try {
                    const auto drawParams = Pm4::ResolveDraw(packet, scanState);
                    if (drawParams.indexCount > 0 && drawParams.instanceCount > 0) {
                        auto decoded = DecodeDrawDispatch(scanState, *shaders, DriverDetail::NullPixelProgramAddress());
                        if (decoded.state.stages.path == Graphics::ShaderPath::Vertex &&
                            !decoded.state.rectList && !decoded.state.stages.mesh) {
                            MetalWorkerPool::Shared().Enqueue([this, serial, decoded = std::move(decoded), drawParams, draw, device] {
                                if (retiredSerial.load(std::memory_order_acquire) >= serial) {
                                    droppedCount.fetch_add(1, std::memory_order_relaxed);
                                    return;
                                }
                                PrewarmDraw(decoded, drawParams, draw, device);
                                compiledCount.fetch_add(1, std::memory_order_relaxed);
                            });
                        }
                    }
                } catch (...) {
                    droppedCount.fetch_add(1, std::memory_order_relaxed);
                }
            }

            cursor = next;
        }
    });
}

void MetalDrawAhead::Progress(std::uint64_t serial, std::size_t cursor) {
    passedCursor.store(cursor, std::memory_order_release);
    usedCount.fetch_add(1, std::memory_order_relaxed);
}

void MetalDrawAhead::Retire(std::uint64_t serial) {
    retiredSerial.store(serial, std::memory_order_release);
}

DrawAheadCounters MetalDrawAhead::TakeStatistics(bool reset) {
    DrawAheadCounters result{
        .published = publishedCount.load(std::memory_order_relaxed),
        .used = usedCount.load(std::memory_order_relaxed),
        .compiled = compiledCount.load(std::memory_order_relaxed),
        .dropped = droppedCount.load(std::memory_order_relaxed),
    };
    if (reset) {
        publishedCount.store(0, std::memory_order_relaxed);
        usedCount.store(0, std::memory_order_relaxed);
        compiledCount.store(0, std::memory_order_relaxed);
        droppedCount.store(0, std::memory_order_relaxed);
    }
    return result;
}

}
