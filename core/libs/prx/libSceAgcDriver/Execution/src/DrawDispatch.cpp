#include "prx/libSceAgcDriver/Execution/include/DrawDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace AgcDriver {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AGC driver: ") + reason);
}

// Program address, resource and user-data registers that were never written read as zero.
std::uint32_t ReadGraphicsRegister(const Registers& registers, std::uint32_t offset) {
    const auto found = registers.find(offset);
    return found == registers.end() ? 0u : found->second;
}


}

void DecodeDrawPrograms(const Graphics::State& state, const QueueState& queue, const DriverDetail::ShaderRegistry& shaders,
                        std::uint64_t nullPixelProgramAddress, bool staticAbi, bool includeFragment,
                        std::vector<DrawDispatchProgram>& programs, std::vector<ShaderRecompiler::ProgramRole>& roles) {
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;
    const auto programAddress = [&](std::uint32_t base) {
        Graphics::NoteRegisterRead(Graphics::RegisterBank::Shader, base);
        Graphics::NoteRegisterRead(Graphics::RegisterBank::Shader, base + 1);
        const auto high = ReadGraphicsRegister(queue.shader, base + 1);
        require((high & ~0xffu) == 0, "reserved graphics program address bits are set");
        return (static_cast<std::uint64_t>(ReadGraphicsRegister(queue.shader, base)) << 8u) | (static_cast<std::uint64_t>(high) << 40u);
    };
    const bool pixelSkipped = Graphics::PixelProgramSkipped(queue);
    const auto prepare = [&](std::uint64_t address, std::uint8_t type, Stage stage, std::uint32_t rsrc2, std::uint32_t userDataBase) {
        const bool nullPixel = stage == Stage::Fragment && (address == 0 || pixelSkipped);
        if (nullPixel) address = nullPixelProgramAddress;
        auto it = shaders.upper_bound(address);
        require(it != shaders.begin(), "graphics program does not belong to a registered shader");
        --it;
        const auto& snapshot = *it->second;
        require(address - snapshot.codeAddress < snapshot.code.size() * sizeof(std::uint32_t), "graphics program is outside registered shader code");
        require((address - snapshot.codeAddress) % sizeof(std::uint32_t) == 0, "graphics entry point is not dword aligned");
        require(snapshot.type == type, "graphics program refers to an incompatible shader binary type");
        if (!nullPixel) Graphics::NoteRegisterRead(Graphics::RegisterBank::Shader, rsrc2);
        const auto resources = nullPixel ? 0u : ReadGraphicsRegister(queue.shader, rsrc2);
        const auto userCount = ((resources >> 1u) & 0x1fu) | (((resources >> 27u) & 1u) << 5u);
        require(userCount <= 32, "graphics user SGPR count exceeds the register bank");
        const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
        DrawDispatchProgram result{
            {stage, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
            userDataBase,
            8,
            {},
            {{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}, {snapshot.headerAddress, snapshot.header}}},
            it->second,
            codeOffset
        };
        for (std::uint32_t i = 0; i < userCount; ++i) {
            Graphics::NoteRegisterRead(Graphics::RegisterBank::Shader, userDataBase + i);
            result.userData.push_back(ReadGraphicsRegister(queue.shader, userDataBase + i));
        }
        return result;
    };
    programs.reserve(programs.size() + 5);
    roles.reserve(roles.size() + 5);
    const auto append = [&](std::uint32_t base, std::uint8_t type, Stage stage, std::uint32_t resources, std::uint32_t users, Role role) {
        programs.push_back(prepare(programAddress(base), type, stage, resources, users));
        roles.push_back(role);
    };
    const auto initializeMerged = [&](DrawDispatchProgram& program, std::uint32_t pointerBase, bool pointerRequired) {
        program.firstUserSgpr = 0;
        program.userData.insert(program.userData.begin(), 8, 0);
        Graphics::NoteRegisterRead(Graphics::RegisterBank::Shader, pointerBase);
        Graphics::NoteRegisterRead(Graphics::RegisterBank::Shader, pointerBase + 1);
        if (staticAbi) return;
        if (!pointerRequired && !queue.shader.contains(pointerBase) && !queue.shader.contains(pointerBase + 1)) return;
        const auto low = ReadGraphicsRegister(queue.shader, pointerBase);
        const auto high = ReadGraphicsRegister(queue.shader, pointerBase + 1);
        const auto address = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
        require(address != 0 || !pointerRequired, "merged shader user-data address is null");
        if (address == 0) return;
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), 8, 4);
        program.userData[0] = low;
        program.userData[1] = high;
    };
    if (state.stages.path == Graphics::ShaderPath::Tessellation) {
        append(0x148, 5, Stage::Local, 0x10b, 0x10c, Role::Local);
        append(0x108, 7, Stage::TessellationControl, 0x10b, 0x10c, Role::Hull);
        initializeMerged(programs.back(), 0x102, true);
        append(0x0c8, 2, Stage::TessellationEvaluation, 0x08b, 0x08c, Role::Domain);
    } else if (state.stages.path == Graphics::ShaderPath::Geometry) {
        const auto frontAddress = programAddress(0xc8);
        auto snapshot = shaders.upper_bound(frontAddress);
        require(snapshot != shaders.begin(), "geometry front program is not registered");
        --snapshot;
        const auto type = snapshot->second->type;
        require(type == 2 || type == 4, "invalid geometry front binary type");
        append(0xc8, type, Stage::Mesh, 0x8b, 0x8c, Role::Main);
        initializeMerged(programs.back(), 0x82, type == 4);
        if (type == 4) append(0x88, 6, Stage::Mesh, 0x8b, 0x8c, Role::GeometryBack);
    } else {
        append(0xc8, 2, Stage::Vertex, 0x8b, 0x8c, Role::Main);
    }
    if (includeFragment) {
        if (pixelSkipped) {
            const auto rejection = Graphics::NullPixelProgramRejection(queue);
            require(rejection.empty(), rejection.c_str());
        }
        append(0x008, 1, Stage::Fragment, 0x00b, 0x00c, Role::Fragment);
        programs.back().firstUserSgpr = 0;
    }
}

DrawDispatchState DecodeDrawDispatch(const QueueState& queue, const DriverDetail::ShaderRegistry& shaders, std::uint64_t nullPixelProgramAddress) {
    DrawDispatchState product;
    product.state = Graphics::DecodeState(queue);
    DecodeDrawPrograms(product.state, queue, shaders, nullPixelProgramAddress, false, true, product.programs, product.roles);
    product.pixel = Graphics::DecodePixelStageInfo(queue.context, Graphics::ExportMappings(product.state), Graphics::PixelProgramSkipped(queue));
    return product;
}

ShaderRecompiler::RecompileRequest BuildDrawRecompileRequest(
    const ShaderRecompiler::ShaderBinary& binary, std::uint32_t firstUserSgpr,
    std::span<const std::uint32_t> userData, const Graphics::State& graphics,
    const ShaderRecompiler::ShaderPixelStageInfo& pixel,
    const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo,
    const ShaderRecompiler::SpirvTarget& target, std::uint32_t pushOffset,
    const Pm4::DrawParameters& drawParameters, std::span<const ShaderRecompiler::MemoryRegion> memory,
    std::span<const ShaderRecompiler::LinkedProgram> linked) {
    const auto pushLimit = graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes;
    require(pushOffset <= pushLimit, "stage push constants exceed the pipeline push constant block");
    const bool fragment = binary.stage == ShaderRecompiler::ShaderStage::Fragment;
    const auto waveSize = fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    return {
        binary,
        {waveSize, firstUserSgpr, userData, std::nullopt, fragment ? std::optional(pixel) : std::nullopt, vertexInfo, memory},
        target,
        // Each stage's push range stays inside one PipelinePushSlotBytes slot; the binding allocator
        // rejects a range that crosses a slot boundary.
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushSlotBytes) - pushOffset % Graphics::PipelinePushSlotBytes},
        ShaderRecompiler::GraphicsCompileContext{firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation,
            {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
    };
}

void FoldDrawOffsets(const ShaderRecompiler::RecompileResult& main, std::uint32_t firstUserSgpr,
                     std::span<const std::uint32_t> userData, Pm4::DrawParameters& parameters) {
    const auto userWord = [&](std::int32_t sgpr) {
        require(sgpr >= 0 && static_cast<std::uint32_t>(sgpr) >= firstUserSgpr, "invalid draw offset SGPR");
        const auto index = static_cast<std::uint32_t>(sgpr) - firstUserSgpr;
        require(index < userData.size(), "draw offset SGPR exceeds user data");
        return userData[index];
    };
    if (main.vertexOffsetSgpr >= 0) {
        const auto offset = userWord(main.vertexOffsetSgpr);
        require(offset <= std::numeric_limits<std::uint32_t>::max() - parameters.firstVertex, "draw vertex offset overflow");
        parameters.firstVertex += offset;
    }
    if (main.instanceOffsetSgpr >= 0) parameters.firstInstance = userWord(main.instanceOffsetSgpr);
}


}
