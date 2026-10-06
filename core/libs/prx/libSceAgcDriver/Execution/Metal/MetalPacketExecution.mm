#include "MetalDriverInternal.hpp"
#include "prx/libSceAgcDriver/Execution/include/ComputeDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/IndirectDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MeshDraw.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderResources.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace AgcDriver::Metal {
namespace {
using namespace ShaderRecompiler;

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

SpirvTarget nativeTarget(id<MTLDevice> device, std::optional<MeshTargetLimits> mesh = {}) {
    static constexpr std::array<std::uint32_t, 4> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityMinLod};
    static constexpr std::array<std::string_view, 2> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto maximum = device.maxThreadsPerThreadgroup;
    SpirvTarget target{0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height), static_cast<std::uint32_t>(maximum.depth)},
        static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
    if (mesh) {
        static constexpr std::array<std::uint32_t, 5> meshCapabilities{
            spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
            spv::CapabilityMeshShadingEXT, spv::CapabilityMinLod};
        static constexpr std::array<std::string_view, 3> meshExtensions{
            "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage", "SPV_EXT_mesh_shader"};
        target.spirvVersion = 0x00010400;
        target.supportedCapabilities = meshCapabilities;
        target.supportedExtensions = meshExtensions;
        target.mesh = mesh;
    }
    return target;
}

void checkGpuFault(const BdaAbi::Fault& fault) {
    if (fault.state == BdaAbi::FaultState::Empty) return;
    std::ostringstream message;
    message << "Native Metal guest GPU access failed: reason=" << static_cast<std::uint32_t>(fault.reason)
        << " address=0x" << std::hex << fault.address << " instruction=0x" << fault.instruction
        << std::dec << " bytes=" << fault.bytes << " stage=" << fault.stage;
    throw std::runtime_error(message.str());
}

}

void MetalDriver::Impl::ExecuteDispatchSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
    if (backend == nullptr || submission.shaders == nullptr) throw std::runtime_error("Native Metal compute executor is not configured");
    const auto decoded = DecodeComputeDispatch(queue, packet);
    if (std::any_of(decoded.groups.begin(), decoded.groups.end(), [](auto value) { return value == 0; })) return;
    std::shared_ptr<const DriverDetail::ShaderSnapshot> captured;
    auto position = submission.shaders->upper_bound(decoded.programAddress);
    if (position != submission.shaders->begin()) {
        --position;
        const auto& candidate = position->second;
        if (decoded.programAddress >= candidate->codeAddress &&
            decoded.programAddress - candidate->codeAddress < candidate->code.size() * sizeof(std::uint32_t)) {
            captured = candidate;
        }
    }
    if (captured == nullptr) {
        CompletePriorGpuWorkAndCopyBack();
        const auto available = NativeGuestMemory::ReadableBorrowedBytes(decoded.programAddress, 1024 * 1024);
        captured = DriverDetail::CaptureRawComputeShader(decoded.programAddress, available);
    }
    const auto& snapshot = *captured;
    if (snapshot.type != 0 || (decoded.programAddress - snapshot.codeAddress) % sizeof(std::uint32_t) != 0) {
        throw std::invalid_argument("Native Metal compute program refers to incompatible shader code");
    }
    const auto offset = static_cast<std::size_t>((decoded.programAddress - snapshot.codeAddress) / sizeof(std::uint32_t));
    std::vector<MemoryRegion> memory{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}};
    if (!snapshot.header.empty()) memory.push_back({snapshot.headerAddress, snapshot.header});
    ShaderMemory capture(memory);
    RecompileRequest request{{ShaderStage::Compute, decoded.programAddress, std::span(snapshot.code).subspan(offset),
        snapshot.headerAddress, snapshot.header},
        {decoded.waveSize, 0, decoded.userData, decoded.compute, {}, {}, memory}, nativeTarget(nativeDevice),
        {0, 0, 0, 128}, {}, true};
    const auto resources = capture.Capture(request);
    memory = capture.Regions();
    request.context.memory = memory;
    const auto guest = Recompile(request, *resources);
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true;
    options.supportsGpuAddresses = true;
    options.supportsSimdGroups = true;
    configureSamplerArguments(options, *guest, nativeDevice);
    const auto converted = MetalBackend::ConvertToMetal(*guest, ShaderStage::Compute, options);
    MetalComputePipeline pipeline(nativeDevice, converted);
    MetalShaderResources bindings(*backend, ranges);
    const auto descriptors = bindings.Bindings(converted);
    std::array<NSUInteger, 3> grid{};
    for (std::uint32_t axis = 0; axis < 3; ++axis) {
        const auto threads = static_cast<std::uint64_t>(decoded.groups[axis]) * converted.threadsPerThreadgroup[axis];
        if (threads > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Native Metal compute grid exceeds shader integer range");
        grid[axis] = static_cast<NSUInteger>(threads);
    }
    id<MTLCommandBuffer> commands = backend->CommandBuffer();
    pipeline.Encode(commands, descriptors, MTLSizeMake(grid[0], grid[1], grid[2]), {}, bindings.Residency());
    [commands commit];
    backend->Wait(commands);
    checkGpuFault(bindings.Complete(commands));
}

void MetalDriver::Impl::ExecuteDrawSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
    if (draw == nullptr || submission.shaders == nullptr) throw std::runtime_error("Native Metal draw executor is not configured");
    auto parameters = Pm4::ResolveDraw(packet, queue);
    if (!parameters.indirect && (parameters.indexCount == 0 || parameters.instanceCount == 0)) return;
    auto decoded = DecodeDrawDispatch(queue, *submission.shaders, DriverDetail::NullPixelProgramAddress());
    if ((decoded.state.stages.path != Graphics::ShaderPath::Vertex && decoded.state.stages.path != Graphics::ShaderPath::Geometry) ||
        (decoded.state.rectList && decoded.state.stages.path != Graphics::ShaderPath::Vertex)) {
        throw std::runtime_error("Native Metal draw execution requires the implemented vertex or mesh path");
    }
    const auto executeDirect = [&](Pm4::DrawParameters direct,
        std::optional<std::array<std::uint32_t, 5>> meshArguments = std::nullopt) {
        CheckFailureAndStopping();
        const auto mesh = decoded.state.stages.mesh.has_value();
        const auto rectangle = decoded.state.rectList;
        if (rectangle && direct.indexCount % 3 != 0) throw std::runtime_error("Native Metal draw contains an incomplete rect-list primitive");
        if (mesh) {
            auto& front = decoded.programs.front();
            if (front.firstUserSgpr != 0 || front.userData.size() < MeshIndexBufferUserWord + 4) {
                throw std::runtime_error("Native Metal mesh program lacks the hidden user words");
            }
            const auto descriptor = Graphics::MeshIndexBufferDescriptor(meshArguments ? parameters : direct, front.binary.codeAddress);
            std::copy(descriptor.begin(), descriptor.end(), front.userData.begin() + MeshIndexBufferUserWord);
        }
        auto target = nativeTarget(nativeDevice, mesh ? std::optional(draw->MeshLimits()) : std::nullopt);
        if (rectangle) {
            static constexpr std::array<std::uint32_t, 5> capabilities{
                spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses,
                spv::CapabilityStorageBuffer8BitAccess, spv::CapabilityTessellation, spv::CapabilityMinLod};
            target.supportedCapabilities = capabilities;
            target.tessellation = draw->TessellationLimits();
        }
        const auto pushLimit = mesh ? MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes;
        std::vector<MemoryRegion> memory;
        std::vector<LinkedProgram> linked;
        for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
            const auto& program = decoded.programs[i];
            memory.insert(memory.end(), program.memory.begin(), program.memory.end());
            linked.push_back({decoded.roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
        }
        ShaderMemory capture(memory);
        std::vector<RecompileResult> programs;
        programs.reserve(decoded.programs.size() + (rectangle ? 2u : 0u));
        std::vector<Graphics::CompiledShader> stages;
        stages.reserve(decoded.programs.size() + (rectangle ? 2u : 0u));
        std::uint32_t pushOffset = 0;
        for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
            if (decoded.roles[i] == ProgramRole::GeometryBack) continue;
            const auto& program = decoded.programs[i];
            std::optional<ShaderVertexStageInfo> vertex;
            if (program.binary.stage != ShaderStage::Fragment) {
                vertex = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData);
            }
            auto request = BuildDrawRecompileRequest(program.binary, program.firstUserSgpr, program.userData,
                decoded.state, decoded.pixel, vertex, target, pushOffset, direct, memory, linked);
            const auto resources = capture.Capture(request);
            memory = capture.Regions();
            request.context.memory = memory;
            programs.push_back(*Recompile(request, *resources));
            const auto& result = programs.back();
            if (result.pushConstants.size() > pushLimit - pushOffset) {
                throw std::runtime_error("Native Metal draw stage push constants exceed the pipeline block");
            }
            stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushOffset});
            pushOffset += static_cast<std::uint32_t>(result.pushConstants.size());
            if (i == 0) FoldDrawOffsets(result, program.firstUserSgpr, program.userData, direct);
        }
        if (rectangle) {
            if (programs.size() != 2 || stages.size() != 2 || stages[0].stage != ShaderStage::Vertex ||
                stages[1].stage != ShaderStage::Fragment) {
                throw std::runtime_error("Native Metal rect-list requires vertex and fragment programs");
            }
            auto generated = BuildRectListShaders(programs[0], programs[1], target);
            programs.push_back(std::move(generated.control));
            programs.push_back(std::move(generated.evaluation));
            stages.insert(stages.begin() + 1, {{ShaderStage::TessellationControl, &programs[2], 0},
                {ShaderStage::TessellationEvaluation, &programs[3], 0}});
        }
        checkGpuFault(draw->DrawSynchronously(decoded.state, direct, stages, ranges, meshArguments));
    };
    if (!parameters.indirect) {
        executeDirect(parameters);
        return;
    }
    std::vector<IndirectDrawProgram> programs;
    programs.reserve(decoded.programs.size());
    for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
        auto& program = decoded.programs[i];
        programs.push_back({decoded.roles[i], program.userDataBase, program.firstUserSgpr, program.userData});
    }
    MarkIndirectDrawSgprs(*parameters.indirect, programs);
    CompletePriorGpuWorkAndCopyBack();
    const auto records = ReadIndirectDrawRecords(*parameters.indirect);
    for (std::uint32_t record = 0; record < records.size(); ++record) {
        CheckFailureAndStopping();
        const auto expanded = ExpandIndirectDrawRecord(parameters, records[record], record, programs);
        if (!expanded) continue;
        std::optional<std::array<std::uint32_t, 5>> meshArguments;
        if (decoded.state.stages.mesh && expanded->parameters.indexed) {
            const auto& mesh = *decoded.state.stages.mesh;
            const auto inputSize = mesh.inputPrimitive == 1 ? 1u : mesh.inputPrimitive == 2 ? 2u : 3u;
            const auto step = mesh.inputPrimitive == 5 || mesh.inputPrimitive == 6 ? 1u : inputSize;
            if (expanded->parameters.indexCount < inputSize || mesh.primitivesPerGroup == 0) {
                throw std::runtime_error("Native Metal mesh draw contains no complete primitive");
            }
            const auto groups = ((expanded->parameters.indexCount - inputSize) / step) / mesh.primitivesPerGroup + 1u;
            meshArguments = {groups, expanded->parameters.instanceCount, 1, expanded->parameters.indexCount,
                records[record].firstVertexOrIndex};
        }
        executeDirect(expanded->parameters, meshArguments);
    }
}

void MetalDriver::Impl::ExecuteSubmission(const Submission& submission, QueueState& queue) {
    NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    if (submission.suspend) {
        std::lock_guard lock(gpuMutex);
        CheckFailureAndStopping();
        CompletePriorGpuWorkAndCopyBack();
        return;
    }
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        CheckFailureAndStopping();
        const auto header = submission.commands[cursor];
        const auto count = Pm4::PacketWords(header);
        if (count > submission.commands.size() - cursor) throw std::invalid_argument("Native Metal captured packet exceeds its command buffer");
        const auto packet = std::span(submission.commands).subspan(cursor, count);
        auto next = cursor + count;
        if (Pm4::FillerPacket(header)) { cursor = next; continue; }
        const auto opcode = (header >> 8u) & 0xffu;
        if (header != FlipPacketHeader && header != RenderingWaitPacketHeader) Pm4::Validate(packet, submission.queue);
        std::unique_lock lock(gpuMutex);
        CheckFailureAndStopping();
        if (Pm4::Predicated(header) && queue.predication.operation != 0 && !Pm4::PredicationPasses(queue)) {
            cursor = opcode == 0x3f ? submission.conditionalEnds.at(cursor) : next;
            if (cursor < next || cursor > submission.commands.size()) throw std::invalid_argument("Native Metal predicated indirect-buffer end is invalid");
            continue;
        }
        if (opcode == 0x3f) { cursor = next; continue; }
        if (header == RenderingWaitPacketHeader) {
            CompletePriorGpuWorkAndCopyBack();
            const auto wait = submission.renderingWaits.at(cursor);
            if (wait == nullptr) throw std::runtime_error("Native Metal rendering wait reservation is null");
            NoteWaitBlocked(submission.queue, 0, true);
            lock.unlock();
            try {
                CheckFailureAndStopping();
                wait->Wait();
                CheckFailureAndStopping();
            } catch (...) {
                NoteWaitBlocked(submission.queue, 0, false);
                throw;
            }
            NoteWaitBlocked(submission.queue, 0, false);
        } else if (header == FlipPacketHeader) {
            CompletePriorGpuWorkAndCopyBack();
            CheckFailureAndStopping();
            const auto flip = submission.flips.at(cursor);
            if (flip == nullptr) throw std::runtime_error("Native Metal flip reservation is null");
            if (frameSerial == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Native Metal frame serial overflow");
            auto frame = std::make_shared<FrameTiming>(++frameSerial);
            const auto now = FrameTiming::Clock::now();
            frame->IncludeSubmission(submission.serial, now, now, now, true);
            frame->SetFlip(submission.serial, cursor, now, now);
            lock.unlock();
            flip->GpuReady(frame);
        } else if (opcode == 0x15) {
            ExecuteDispatchSynchronously(queue, packet, submission);
        } else if (opcode == 0x16) {
            const auto direct = Pm4::ResolveDispatch(packet, queue);
            ExecuteDispatchSynchronously(queue, direct, submission);
        } else if (Pm4::DrawOpcode(opcode)) {
            ExecuteDrawSynchronously(queue, packet, submission);
        } else if (opcode == 0x3c || opcode == 0x93) {
            CompletePriorGpuWorkAndCopyBack();
            const auto address = static_cast<std::uint64_t>(packet[2]) | (static_cast<std::uint64_t>(packet[3]) << 32u);
            bool blocked = false;
            try {
                while (!Pm4::WaitSatisfied(packet)) {
                    CheckFailureAndStopping();
                    if (!blocked) { NoteWaitBlocked(submission.queue, address, true); blocked = true; }
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    CheckFailureAndStopping();
                    lock.lock();
                }
            } catch (...) {
                if (blocked) NoteWaitBlocked(submission.queue, address, false);
                throw;
            }
            if (blocked) NoteWaitBlocked(submission.queue, address, false);
            CheckFailureAndStopping();
        } else if (opcode == 0x22) {
            if (Pm4::ReadCondition(packet) == 0) {
                next = submission.conditionalEnds.at(cursor);
                if (next < cursor + count || next > submission.commands.size()) throw std::invalid_argument("Native Metal conditional execution end is invalid");
            }
        } else if (opcode == 0x42 || opcode == 0x58 || opcode == 0x46) {
            CompletePriorGpuWorkAndCopyBack();
            if (opcode == 0x46 && (packet[1] & 0x3fu) == 0x39u) {
                if (draw == nullptr) throw std::runtime_error("Native Metal sample counter executor is not configured");
                const auto address = static_cast<std::uint64_t>(packet[2]) | (static_cast<std::uint64_t>(packet[3]) << 32u);
                draw->DumpSamplesSynchronously(address);
            }
        } else {
            if (opcode == 0x49 || opcode == 0x37 || opcode == 0x40 || opcode == 0x50 || opcode == 0x83) {
                CompletePriorGpuWorkAndCopyBack();
            }
            Pm4::Execute(packet, queue);
            if (opcode == 0x49 || opcode == 0x37 || opcode == 0x40 || opcode == 0x50 || opcode == 0x83) NotifyLabelStore();
            if (opcode == 0x49 && ((packet[2] >> 24u) & 7u) != 0) {
                CompletePriorGpuWorkAndCopyBack();
                DeliverEopInterrupt(submission.queue);
            }
        }
        cursor = next;
    }
    {
        std::lock_guard lock(gpuMutex);
        CheckFailureAndStopping();
        CompletePriorGpuWorkAndCopyBack();
    }
    if (submission.rewindTail != nullptr) ExecuteRewindTail(submission, queue);
}

}
