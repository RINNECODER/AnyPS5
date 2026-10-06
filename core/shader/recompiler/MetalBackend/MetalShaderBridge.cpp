#include "MetalShaderBridge.hpp"
#include "../BdaAbi.hpp"
#include <spirv_msl.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ShaderRecompiler::MetalBackend {
namespace {
[[noreturn]] void Fail(const std::string& reason) {
    throw std::runtime_error("Metal shader bridge: " + reason);
}

class MetalCompiler final : public spirv_cross::CompilerMSL {
public:
    using CompilerMSL::CompilerMSL;

    void ConfigureMeshPosition(std::uint32_t position, std::uint32_t vertices, bool flipY, bool fixupDepth) {
        meshPosition = position;
        meshVertices = vertices;
        meshFlipY = flipY;
        meshFixupDepth = fixupDepth;
    }

    InterfaceLayout InputLayout() const { return Layout(stage_in_var_id); }
    InterfaceLayout OutputLayout() const { return Layout(stage_out_var_id); }

protected:
    std::string to_function_args(const TextureFunctionArguments& args, bool* forward) override {
        if (args.base.imgtype->image.dim != spv::Dim1D || (!args.grad_x && !args.grad_y))
            return CompilerMSL::to_function_args(args, forward);
        auto expanded = args;
        const auto expandGradient = [&](std::uint32_t gradient) {
            if (gradient == 0) return 0u;
            const auto& scalar = expression_type(gradient);
            if ((scalar.basetype != spirv_cross::SPIRType::Float && scalar.basetype != spirv_cross::SPIRType::Half) ||
                scalar.vecsize != 1 || scalar.columns != 1 || !scalar.array.empty())
                Fail("logical 1D texture derivatives require scalar floating-point gradients");
            const auto ids = ir.increase_bound_by(2);
            auto& vector = set<spirv_cross::SPIRType>(ids, spirv_cross::SPIRType{spv::OpTypeVector});
            vector.basetype = spirv_cross::SPIRType::Float;
            vector.width = 32;
            vector.vecsize = 2;
            vector.columns = 1;
            emit_op(ids, ids + 1, "float2(" + to_unpacked_expression(gradient) + ", 0.0)", should_forward(gradient));
            inherit_expression_dependencies(ids + 1, gradient);
            return ids + 1;
        };
        expanded.grad_x = expandGradient(args.grad_x);
        expanded.grad_y = expandGradient(args.grad_y);
        return CompilerMSL::to_function_args(expanded, forward);
    }

    void emit_function_prototype(spirv_cross::SPIRFunction& function,
                                 const spirv_cross::Bitset& flags) override {
        CompilerMSL::emit_function_prototype(function, flags);
        if (function.self != ir.default_entry_point || meshPosition == 0 || positionHookInstalled ||
            (!meshFlipY && !meshFixupDepth)) return;
        positionHookInstalled = true;
        function.fixup_hooks_out.insert(function.fixup_hooks_out.begin(), [this]() {
            statement("threadgroup_barrier(mem_flags::mem_threadgroup);");
            const auto& group = get_entry_point().workgroup_size;
            const auto threads = group.x * group.y * group.z;
            statement("if (", to_name(builtin_mesh_sizes_id), ".y != 0u)");
            begin_scope();
            statement("for (uint spvMetalPositionIndex = ", to_name(builtin_local_invocation_index_id),
                      "; spvMetalPositionIndex < min(", to_name(builtin_mesh_sizes_id),
                      ".x, ", meshVertices, "u); spvMetalPositionIndex += ", threads, "u)");
            begin_scope();
            const auto position = to_name(meshPosition) + "[spvMetalPositionIndex]";
            if (meshFixupDepth)
                statement(position, ".z = (", position, ".z + ", position, ".w) * 0.5;");
            if (meshFlipY) statement(position, ".y = -(", position, ".y);");
            end_scope();
            end_scope();
        });
    }

private:
    InterfaceLayout Layout(std::uint32_t variable) const {
        if (variable == 0) Fail("rectangle stage has no raw interface");
        const auto& type = get_variable_data_type(get<spirv_cross::SPIRVariable>(variable));
        if (type.basetype != spirv_cross::SPIRType::Struct || type.member_types.empty())
            Fail("rectangle raw interface is not a structure");
        InterfaceLayout result;
        std::uint64_t offset = 0;
        std::uint32_t positionCount = 0;
        std::vector<std::uint32_t> locations;
        for (std::uint32_t i = 0; i < type.member_types.size(); ++i) {
            const auto& memberType = get_type(type.member_types[i]);
            if ((memberType.basetype != spirv_cross::SPIRType::Float &&
                 memberType.basetype != spirv_cross::SPIRType::UInt &&
                 memberType.basetype != spirv_cross::SPIRType::Int) ||
                memberType.width != 32 || memberType.columns != 1 || memberType.vecsize != 4 ||
                !memberType.array.empty())
                Fail("rectangle interface requires the original 32-bit four-component fields");
            const auto alignment = get_declared_struct_member_alignment_msl(type, i);
            const auto bytes = get_declared_struct_member_size_msl(type, i);
            if (alignment == 0 || alignment > std::numeric_limits<std::uint32_t>::max() ||
                bytes == 0 || bytes > std::numeric_limits<std::uint32_t>::max())
                Fail("invalid rectangle interface field packing");
            offset = (offset + alignment - 1) / alignment * alignment;
            if (offset > std::numeric_limits<std::uint32_t>::max() - bytes)
                Fail("rectangle interface size overflow");
            InterfaceMember member;
            if (has_member_decoration(type.self, i, spv::DecorationBuiltIn))
                member.builtin = get_member_decoration(type.self, i, spv::DecorationBuiltIn);
            else if (has_member_decoration(type.self, i, spv::DecorationLocation))
                member.location = get_member_decoration(type.self, i, spv::DecorationLocation);
            if (member.location.has_value() == member.builtin.has_value() ||
                (member.builtin && *member.builtin != spv::BuiltInPosition))
                Fail("rectangle interface field lacks an original location or position semantic");
            if (member.builtin && (++positionCount != 1 || memberType.basetype != spirv_cross::SPIRType::Float))
                Fail("rectangle interface requires one float4 position field");
            if (member.location) {
                if (*member.location >= 32 || std::find(locations.begin(), locations.end(), *member.location) != locations.end())
                    Fail("invalid or duplicate rectangle interface location");
                locations.push_back(*member.location);
            }
            member.components = memberType.vecsize;
            member.width = memberType.width;
            member.offset = static_cast<std::uint32_t>(offset);
            member.bytes = static_cast<std::uint32_t>(bytes);
            result.members.push_back(member);
            result.alignment = std::max(result.alignment, static_cast<std::uint32_t>(alignment));
            offset += bytes;
        }
        offset = (offset + result.alignment - 1) / result.alignment * result.alignment;
        if (offset > std::numeric_limits<std::uint32_t>::max()) Fail("rectangle interface stride overflow");
        result.stride = static_cast<std::uint32_t>(offset);
        if (positionCount != 1) Fail("rectangle interface has no position field");
        return result;
    }

    std::uint32_t meshPosition = 0;
    std::uint32_t meshVertices = 0;
    bool meshFlipY = false;
    bool meshFixupDepth = false;
    bool positionHookInstalled = false;
};

std::uint32_t DescriptorCount(const spirv_cross::CompilerMSL& compiler,
                              const spirv_cross::Resource& resource) {
    const auto& type = compiler.get_type(resource.type_id);
    std::uint64_t count = 1;
    for (std::size_t i = 0; i < type.array.size(); ++i) {
        if (!type.array_size_literal[i] || type.array[i] == 0)
            Fail("runtime or specialization-sized descriptor arrays require argument-buffer support");
        count *= type.array[i];
        if (count > std::numeric_limits<std::uint32_t>::max()) Fail("descriptor array count overflow");
    }
    return static_cast<std::uint32_t>(count);
}
}

Result ConvertToMetal(const RecompileResult& guest, ShaderStage stage, const TargetOptions& target) {
    switch (target.rectListMode) {
    case RectListMode::None:
    case RectListMode::VertexCapture:
    case RectListMode::Control:
    case RectListMode::Evaluation: break;
    default: Fail("invalid rectangle execution mode");
    }
    const bool capture = target.rectListMode == RectListMode::VertexCapture;
    const bool control = target.rectListMode == RectListMode::Control;
    const bool evaluation = target.rectListMode == RectListMode::Evaluation;
    const bool rectangle = target.rectListMode != RectListMode::None;
    if ((capture && stage != ShaderStage::Vertex) ||
        (control && stage != ShaderStage::TessellationControl) ||
        (evaluation && stage != ShaderStage::TessellationEvaluation))
        Fail("rectangle execution mode does not match the guest stage");
    if ((control || evaluation) != target.rectListInputLayout.has_value())
        Fail("rectangle control and evaluation require an upstream raw interface layout");
    if ((target.rectListIndexSize != 0 && !capture) ||
        (capture && target.rectListIndexSize != 0 && target.rectListIndexSize != 2 && target.rectListIndexSize != 4))
        Fail("rectangle vertex capture requires an explicit 16-bit, 32-bit, or absent index contract");
    spv::ExecutionModel execution;
    switch (stage) {
    case ShaderStage::Compute: execution = spv::ExecutionModelGLCompute; break;
    case ShaderStage::Vertex: execution = spv::ExecutionModelVertex; break;
    case ShaderStage::Fragment: execution = spv::ExecutionModelFragment; break;
    case ShaderStage::Mesh: execution = spv::ExecutionModelMeshEXT; break;
    case ShaderStage::TessellationControl:
        if (!control) Fail("guest tessellation control requires unimplemented native Metal scheduling");
        execution = spv::ExecutionModelTessellationControl; break;
    case ShaderStage::TessellationEvaluation:
        if (!evaluation) Fail("guest tessellation evaluation requires unimplemented native Metal scheduling");
        execution = spv::ExecutionModelTessellationEvaluation; break;
    default: Fail("stage requires unimplemented native Metal scheduling");
    }
    if (guest.spirv.empty()) Fail("empty SPIR-V module");
    if (target.mslVersion < 20200) Fail("MSL 2.2 or newer is required");
    if (stage == ShaderStage::Mesh && target.mslVersion < 30000) Fail("mesh shaders require MSL 3.0 or newer");
    if (target.maxBuffers > 31 || target.maxBuffers < 3 || target.maxTextures > 128 || target.maxSamplers > 16)
        Fail("invalid Metal resource limits");
    if (target.samplerArgumentBuffer && (!target.supportsArgumentBuffersTier2 || target.maxArgumentBufferSamplers == 0))
        Fail("sampler argument buffers require an explicit Tier2 capacity contract");
    if (target.samplerArgumentBuffer && std::any_of(guest.bindings.begin(), guest.bindings.end(), [](const auto& binding) { return binding.descriptorSet != 0; }))
        Fail("sampler argument buffers require original descriptor set zero");
    if (target.pushConstantBuffer >= target.maxBuffers || target.bufferSizesBuffer >= target.maxBuffers ||
        target.pushConstantBuffer == target.bufferSizesBuffer)
        Fail("invalid or overlapping auxiliary buffer indices");
    const auto vertexBufferCount = stage == ShaderStage::Vertex ? target.vertexBufferCount : 0u;
    if (vertexBufferCount > target.maxBuffers || vertexBufferCount > target.pushConstantBuffer ||
        vertexBufferCount > target.bufferSizesBuffer)
        Fail("vertex buffer slots overlap auxiliary Metal slots or exceed the buffer limit");
    if (stage == ShaderStage::Vertex && guest.vertexAttributes.size() > vertexBufferCount)
        Fail("vertex attribute buffers require reserved native Metal slots");

    MetalCompiler compiler(guest.spirv.Words());
    const auto entries = compiler.get_entry_points_and_stages();
    if (entries.size() != 1 || entries[0].execution_model != execution)
        Fail("expected exactly one entry point matching the requested stage");
    compiler.set_entry_point(entries[0].name, execution);
    if (stage == ShaderStage::Compute || stage == ShaderStage::Mesh) {
        spirv_cross::SpecializationConstant x{}, y{}, z{};
        compiler.get_work_group_size_specialization_constants(x, y, z);
        if (x.id || y.id || z.id)
            Fail(stage == ShaderStage::Mesh ? "specialized mesh workgroup dimensions require an explicit dispatch specialization contract" :
                 "specialized compute workgroup dimensions require an explicit dispatch specialization contract");
    }

    Result result;
    result.stage = stage;
    result.nativeExecutionKind = stage == ShaderStage::Mesh ? NativeExecutionKind::Mesh :
        stage == ShaderStage::Fragment ? NativeExecutionKind::Fragment :
        evaluation || (stage == ShaderStage::Vertex && !capture) ? NativeExecutionKind::Vertex : NativeExecutionKind::Compute;
    result.vertexBufferCount = vertexBufferCount;
    result.guest = guest;
    if (rectangle) {
        result.rectList = RectListInfo{target.rectListMode};
        result.rectList->indexSize = target.rectListIndexSize;
        if (control && compiler.get_execution_mode_argument(spv::ExecutionModeOutputVertices) != 4)
            Fail("generated rectangle control requires exactly four output control points");
        if (evaluation) {
            const auto& modes = compiler.get_execution_mode_bitset();
            if (!modes.get(spv::ExecutionModeQuads) || !modes.get(spv::ExecutionModeSpacingEqual) ||
                !modes.get(spv::ExecutionModeVertexOrderCw) || modes.get(spv::ExecutionModeTriangles) ||
                modes.get(spv::ExecutionModeIsolines) || modes.get(spv::ExecutionModePointMode))
                Fail("generated rectangle evaluation requires equal-spaced clockwise quad patches");
            compiler.set_execution_mode(spv::ExecutionModeOutputVertices, 4);
        }
    }
    if (stage == ShaderStage::Mesh) {
        for (std::uint32_t i = 0; i < 3; ++i)
            result.threadsPerThreadgroup[i] = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, i);
        if (std::any_of(result.threadsPerThreadgroup.begin(), result.threadsPerThreadgroup.end(), [](auto dimension) { return dimension == 0; }))
            Fail("specialized or missing mesh workgroup dimensions require an explicit dispatch specialization contract");
    }
    if (stage == ShaderStage::Mesh) {
        const auto& modes = compiler.get_execution_mode_bitset();
        if (!modes.get(spv::ExecutionModeOutputTrianglesEXT) ||
            modes.get(spv::ExecutionModeOutputLinesEXT) || modes.get(spv::ExecutionModeOutputPoints))
            Fail("mesh output requires the original triangle topology contract");
        const auto vertices = compiler.get_execution_mode_argument(spv::ExecutionModeOutputVertices);
        const auto primitives = compiler.get_execution_mode_argument(spv::ExecutionModeOutputPrimitivesEXT);
        if (vertices == 0 || vertices > 256 || primitives == 0 || primitives > 512)
            Fail("mesh output counts exceed native Metal limits");
        std::uint32_t threads = 1;
        for (const auto dimension : result.threadsPerThreadgroup) {
            if (dimension > 1024u / threads) Fail("mesh workgroup exceeds native Metal thread limits");
            threads *= dimension;
        }
        result.mesh = MeshOutputInfo{vertices, primitives};
    }
    for (const auto capability : compiler.get_declared_capabilities()) {
        result.capabilities.push_back(static_cast<std::uint32_t>(capability));
        if (capability == spv::CapabilityFloat64) Fail("Float64 requires explicit software emulation");
        if (capability == spv::CapabilityInt64Atomics) Fail("64-bit atomics are unsupported by the pinned SPIRV-Cross MSL backend");
        result.requiresInt64 |= capability == spv::CapabilityInt64;
        result.requiresGpuAddresses |= capability == spv::CapabilityPhysicalStorageBufferAddresses;
        result.requiresSimdGroups |= capability == spv::CapabilityGroupNonUniform ||
            capability == spv::CapabilityGroupNonUniformBallot || capability == spv::CapabilityGroupNonUniformShuffle ||
            capability == spv::CapabilityGroupNonUniformShuffleRelative || capability == spv::CapabilityGroupNonUniformArithmetic ||
            capability == spv::CapabilityGroupNonUniformVote;
        if (capability == spv::CapabilityComputeDerivativeGroupQuadsKHR ||
            capability == spv::CapabilityComputeDerivativeGroupLinearKHR)
            Fail("compute derivative-group execution has no validated Metal scheduling contract");
    }
    for (const auto& extension : compiler.get_declared_extensions()) result.extensions.push_back(extension);
    if (result.requiresInt64 && !target.supportsInt64) Fail("target lacks 64-bit integer support");
    if (result.requiresGpuAddresses && !target.supportsGpuAddresses) Fail("target lacks GPU-address support");
    if (result.requiresSimdGroups && !target.supportsSimdGroups) Fail("target lacks SIMD-group support");
    if (guest.bdaAbiVersion != 0) {
        const bool generatedFault = control && guest.bdaAbiVersion == BdaAbi::Version &&
            !result.requiresGpuAddresses && guest.bindings.size() == 1 &&
            guest.bindings[0].kind == DescriptorKind::StorageBuffer &&
            guest.bindings[0].role == DescriptorRole::FaultBuffer && guest.bindings[0].count == 1 &&
            guest.bindings[0].descriptorSet == 0;
        if (!generatedFault && (guest.bdaAbiVersion != BdaAbi::Version || !result.requiresGpuAddresses))
            Fail("incompatible BDA ABI or missing physical-address capability");
        for (const auto role : {DescriptorRole::BdaPagetable, DescriptorRole::FaultBuffer})
            if (!generatedFault && std::none_of(guest.bindings.begin(), guest.bindings.end(), [role](const auto& binding) { return binding.role == role; }))
                Fail("BDA ABI requires page-table and fault-buffer descriptor metadata");
    }
    if (control && (guest.bdaAbiVersion != BdaAbi::Version || result.requiresGpuAddresses ||
        guest.bindings.size() != 1 || guest.bindings[0].kind != DescriptorKind::StorageBuffer ||
        guest.bindings[0].role != DescriptorRole::FaultBuffer || guest.bindings[0].count != 1 ||
        guest.bindings[0].descriptorSet != 0))
        Fail("generated rectangle control requires the original fault-only descriptor ABI");
    if (evaluation && (guest.bdaAbiVersion != 0 || !guest.bindings.empty()))
        Fail("generated rectangle evaluation has no guest descriptors");

    auto options = compiler.get_msl_options();
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.msl_version = target.mslVersion;
    options.buffer_size_buffer_index = target.bufferSizesBuffer;
    options.texture_buffer_native = true;
    options.texture_1D_as_2D = true;
    options.argument_buffers = target.samplerArgumentBuffer;
    if (target.samplerArgumentBuffer) {
        options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
        options.force_active_argument_buffer_resources = true;
        compiler.add_discrete_descriptor_set(0);
    }
    if (rectangle) {
        options.vertex_for_tessellation = capture;
        options.multi_patch_workgroup = control;
        options.raw_buffer_tese_input = evaluation;
        options.vertex_index_type = target.rectListIndexSize == 2 ? spirv_cross::CompilerMSL::Options::IndexType::UInt16 :
            target.rectListIndexSize == 4 ? spirv_cross::CompilerMSL::Options::IndexType::UInt32 : spirv_cross::CompilerMSL::Options::IndexType::None;
        options.shader_output_buffer_index = target.rectListBuffers.output;
        options.shader_input_buffer_index = target.rectListBuffers.input;
        options.shader_tess_factor_buffer_index = target.rectListBuffers.factors;
        options.indirect_params_buffer_index = target.rectListBuffers.indirect;
        options.shader_index_buffer_index = target.rectListBuffers.indices;
    }
    compiler.set_msl_options(options);
    auto common = compiler.get_common_options();
    common.vertex.flip_vert_y = (evaluation || (stage == ShaderStage::Vertex && !capture)) && target.flipVertexY;
    common.vertex.fixup_clipspace = (evaluation || (stage == ShaderStage::Vertex && !capture)) && target.fixupClipSpace;
    compiler.set_common_options(common);
    const auto resources = compiler.get_shader_resources();
    const bool useSamplerBank = target.samplerArgumentBuffer && !resources.separate_samplers.empty();
    const auto activeResources = compiler.get_shader_resources(compiler.get_active_interface_variables());
    if (control || evaluation) {
        const auto checkPosition = [&](const auto& reflected, std::uint32_t count) {
            std::uint32_t positions = 0;
            for (const auto& builtin : reflected) {
                if (builtin.builtin != spv::BuiltInPosition) continue;
                ++positions;
                const auto& type = compiler.get_type(builtin.resource.type_id);
                if (type.basetype != spirv_cross::SPIRType::Struct || type.member_types.size() != 1 ||
                    (count ? type.array.size() != 1 || !type.array_size_literal[0] || type.array[0] != count : !type.array.empty()))
                    Fail("generated rectangle position array differs from the original control-point contract");
                const auto& position = compiler.get_type(type.member_types[0]);
                if (!compiler.has_member_decoration(type.self, 0, spv::DecorationBuiltIn) ||
                    compiler.get_member_decoration(type.self, 0, spv::DecorationBuiltIn) != spv::BuiltInPosition ||
                    position.basetype != spirv_cross::SPIRType::Float || position.width != 32 ||
                    position.vecsize != 4 || position.columns != 1 || !position.array.empty())
                    Fail("generated rectangle position requires the original float4 interface block");
            }
            if (positions != 1) Fail("generated rectangle requires one position input and output");
        };
        const auto checkParameters = [&](const auto& reflected, std::uint32_t count) {
            for (const auto& resource : reflected) {
                const auto& type = compiler.get_type(resource.type_id);
                if (!compiler.has_decoration(resource.id, spv::DecorationLocation) ||
                    compiler.get_decoration(resource.id, spv::DecorationLocation) >= 32 ||
                    type.basetype != spirv_cross::SPIRType::Float || type.width != 32 || type.vecsize != 4 || type.columns != 1 ||
                    (count ? type.array.size() != 1 || !type.array_size_literal[0] || type.array[0] != count : !type.array.empty()))
                    Fail("generated rectangle parameters differ from the original float4 control-point contract");
            }
        };
        checkPosition(resources.builtin_inputs, control ? 3 : 4);
        checkPosition(resources.builtin_outputs, control ? 4 : 0);
        checkParameters(resources.stage_inputs, control ? 3 : 4);
        checkParameters(resources.stage_outputs, control ? 4 : 0);
    }
    if (target.rectListInputLayout) {
        const auto& layout = *target.rectListInputLayout;
        if (layout.stride == 0 || layout.alignment != 16 || layout.stride % layout.alignment != 0 || layout.members.empty())
            Fail("invalid upstream rectangle interface layout");
        std::vector<std::uint32_t> locations;
        std::uint32_t positionCount = 0;
        std::uint32_t end = 0;
        for (const auto& member : layout.members) {
            if (member.components != 4 || member.width != 32 || member.bytes != 16 ||
                member.offset != end || member.location.has_value() == member.builtin.has_value())
                Fail("upstream rectangle interface differs from the original float4 layout");
            if (member.location) {
                if (*member.location >= 32 || std::find(locations.begin(), locations.end(), *member.location) != locations.end())
                    Fail("invalid or duplicate rectangle interface location");
                locations.push_back(*member.location);
                spirv_cross::MSLShaderInterfaceVariable input{};
                input.location = *member.location;
                input.vecsize = member.components;
                input.format = spirv_cross::MSL_SHADER_VARIABLE_FORMAT_ANY32;
                compiler.add_msl_shader_input(input);
            } else if (*member.builtin != spv::BuiltInPosition || ++positionCount != 1)
                Fail("upstream rectangle interface requires one position field");
            if (end > std::numeric_limits<std::uint32_t>::max() - member.bytes)
                Fail("upstream rectangle interface size overflow");
            end += member.bytes;
        }
        if (positionCount != 1 || end != layout.stride) Fail("upstream rectangle interface stride does not match its fields");
    }
    if (stage == ShaderStage::Mesh) {
        std::uint32_t position = 0;
        for (const auto& output : resources.builtin_outputs) {
            if (output.builtin != spv::BuiltInPosition) continue;
            const auto& type = compiler.get_type(output.resource.type_id);
            if (position != 0 || !compiler.has_decoration(output.resource.id, spv::DecorationBuiltIn) ||
                compiler.get_decoration(output.resource.id, spv::DecorationBuiltIn) != spv::BuiltInPosition ||
                type.basetype != spirv_cross::SPIRType::Float || type.width != 32 || type.vecsize != 4 ||
                type.columns != 1 || type.array.size() != 1 || !type.array_size_literal[0] ||
                type.array[0] != result.mesh->maxVertices)
                Fail("mesh position output requires the original standalone float4 array contract");
            position = output.resource.id;
        }
        if (position == 0) Fail("mesh shader has no position output");
        compiler.ConfigureMeshPosition(position, result.mesh->maxVertices, target.flipVertexY, target.fixupClipSpace);
    }
    if (stage == ShaderStage::Vertex) {
        std::vector<std::uint32_t> locations;
        for (const auto& attribute : guest.vertexAttributes) {
            if (attribute.components == 0 || attribute.components > 4 ||
                std::find(locations.begin(), locations.end(), attribute.location) != locations.end())
                Fail("invalid or duplicate vertex attribute metadata");
            locations.push_back(attribute.location);
            const auto reflected = std::find_if(resources.stage_inputs.begin(), resources.stage_inputs.end(), [&](const auto& input) {
                return compiler.has_decoration(input.id, spv::DecorationLocation) &&
                    compiler.get_decoration(input.id, spv::DecorationLocation) == attribute.location;
            });
            if (reflected == resources.stage_inputs.end()) Fail("vertex attribute metadata has no matching SPIR-V input");
            const auto& type = compiler.get_type(reflected->type_id);
            if (type.columns != 1 || !type.array.empty() || type.width != 32 || type.vecsize > attribute.components ||
                (type.basetype != spirv_cross::SPIRType::Float && type.basetype != spirv_cross::SPIRType::Int &&
                 type.basetype != spirv_cross::SPIRType::UInt))
                Fail("vertex attribute component/scalar contract does not match the SPIR-V input");
            spirv_cross::MSLShaderInterfaceVariable input{};
            input.location = attribute.location;
            input.vecsize = attribute.components;
            compiler.add_msl_shader_input(input);
        }
        for (const auto& input : resources.stage_inputs)
            if (compiler.has_decoration(input.id, spv::DecorationLocation) &&
                std::find(locations.begin(), locations.end(), compiler.get_decoration(input.id, spv::DecorationLocation)) == locations.end())
                Fail("SPIR-V vertex input is missing original vertex attribute metadata");
    }
    if (!resources.sampled_images.empty() || !resources.subpass_inputs.empty() ||
        !resources.atomic_counters.empty() || !resources.acceleration_structures.empty())
        Fail("combined images, subpass inputs, atomic counters, and acceleration structures are outside this compute slice");
    if (resources.push_constant_buffers.size() > 1) Fail("multiple push-constant blocks");

    std::vector<std::uint32_t> reservedBuffers{target.pushConstantBuffer, target.bufferSizesBuffer};
    const auto reserveImplicit = [&](ImplicitBufferRole role, std::uint32_t index, std::uint32_t bytes) {
        if (index >= target.maxBuffers || index < vertexBufferCount ||
            std::find(reservedBuffers.begin(), reservedBuffers.end(), index) != reservedBuffers.end())
            Fail("implicit rectangle buffer slot overlaps a reserved Metal slot");
        reservedBuffers.push_back(index);
        result.rectList->buffers.push_back({role, index, bytes});
    };
    if (capture || control) reserveImplicit(ImplicitBufferRole::StageOutput, target.rectListBuffers.output, 0);
    if (control || evaluation) reserveImplicit(ImplicitBufferRole::StageInput, target.rectListBuffers.input, target.rectListInputLayout->stride);
    if (control) {
        reserveImplicit(ImplicitBufferRole::TessellationFactors, target.rectListBuffers.factors, 12);
        reserveImplicit(ImplicitBufferRole::IndirectParameters, target.rectListBuffers.indirect, 8);
    }
    if (capture && target.rectListIndexSize) reserveImplicit(ImplicitBufferRole::Indices, target.rectListBuffers.indices, target.rectListIndexSize);
    std::uint32_t buffer = vertexBufferCount, texture = 0, sampler = 0;
    const auto allocateBuffer = [&](std::uint32_t count) {
        while (std::find(reservedBuffers.begin(), reservedBuffers.end(), buffer) != reservedBuffers.end()) ++buffer;
        const auto first = buffer;
        if (count > target.maxBuffers || first > target.maxBuffers - count) Fail("buffer descriptors exceed native Metal slots");
        for (std::uint32_t i = first; i < first + count; ++i)
            if (std::find(reservedBuffers.begin(), reservedBuffers.end(), i) != reservedBuffers.end()) Fail("descriptor array overlaps reserved Metal slots");
        buffer += count;
        return first;
    };
    for (const auto& binding : guest.bindings) {
        if (binding.count == 0) Fail("zero descriptor count");
        if (std::any_of(result.resources.begin(), result.resources.end(), [&](const auto& mapping) {
            return mapping.descriptorSet == binding.descriptorSet && mapping.binding == binding.binding;
        })) Fail("duplicate descriptor set/binding metadata");
        ResourceMapping mapping{binding.descriptorSet, binding.binding, binding.count, binding.kind, binding.role};
        spirv_cross::MSLResourceBinding native{};
        native.stage = execution;
        native.desc_set = binding.descriptorSet;
        native.binding = binding.binding;
        native.count = binding.count;
        switch (binding.kind) {
        case DescriptorKind::UniformBuffer:
        case DescriptorKind::StorageBuffer:
            mapping.buffer = allocateBuffer(binding.count);
            native.msl_buffer = *mapping.buffer;
            break;
        case DescriptorKind::UniformTexelBuffer:
        case DescriptorKind::StorageTexelBuffer:
        case DescriptorKind::SampledImage:
        case DescriptorKind::StorageImage:
            if (binding.count > target.maxTextures || texture > target.maxTextures - binding.count)
                Fail("image descriptors exceed native Metal slots");
            mapping.texture = texture;
            native.msl_texture = texture;
            texture += binding.count;
            break;
        case DescriptorKind::Sampler:
            if (binding.count > (useSamplerBank ? target.maxArgumentBufferSamplers : target.maxSamplers) ||
                sampler > (useSamplerBank ? target.maxArgumentBufferSamplers : target.maxSamplers) - binding.count)
                Fail(useSamplerBank ? "sampler descriptors exceed explicit argument-buffer capacity" : "sampler descriptors exceed native Metal slots");
            if (useSamplerBank) {
                native.desc_set = 1;
                native.basetype = spirv_cross::SPIRType::Sampler;
            }
            mapping.sampler = sampler;
            native.msl_sampler = sampler;
            sampler += binding.count;
            break;
        }
        compiler.add_msl_resource_binding(native);
        result.resources.push_back(mapping);
    }

    const auto reflect = [&](const auto& reflected, DescriptorKind kind, std::optional<DescriptorKind> texelKind = {}) {
        for (const auto& resource : reflected) {
            const auto set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const auto binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            auto mapping = std::find_if(result.resources.begin(), result.resources.end(), [&](const auto& candidate) {
                return candidate.descriptorSet == set && candidate.binding == binding;
            });
            if (mapping == result.resources.end()) Fail("SPIR-V descriptor missing from guest binding metadata");
            auto expected = kind;
            if (texelKind && compiler.get_type(resource.type_id).image.dim == spv::DimBuffer) expected = *texelKind;
            if (mapping->kind != expected || mapping->count != DescriptorCount(compiler, resource))
                Fail("SPIR-V descriptor kind/count disagrees with guest binding metadata");
            mapping->requiresByteLengths = compiler.buffer_requires_array_length(resource.id);
        }
    };
    reflect(resources.uniform_buffers, DescriptorKind::UniformBuffer);
    reflect(resources.storage_buffers, DescriptorKind::StorageBuffer);
    reflect(resources.separate_images, DescriptorKind::SampledImage, DescriptorKind::UniformTexelBuffer);
    reflect(resources.storage_images, DescriptorKind::StorageImage, DescriptorKind::StorageTexelBuffer);
    reflect(resources.separate_samplers, DescriptorKind::Sampler);
    if (useSamplerBank) {
        std::vector<std::uint32_t> occupied;
        for (std::uint32_t i = 0; i < vertexBufferCount; ++i) occupied.push_back(i);
        if (!resources.push_constant_buffers.empty()) occupied.push_back(target.pushConstantBuffer);
        const auto words = guest.spirv.Words();
        for (std::size_t i = 5; i < words.size();) {
            const auto count = words[i] >> 16u;
            if (count == 0 || count > words.size() - i) Fail("invalid SPIR-V instruction while reserving auxiliary buffers");
            if ((words[i] & 0xffffu) == spv::OpArrayLength) occupied.push_back(target.bufferSizesBuffer);
            i += count;
        }
        if (result.rectList)
            for (const auto& implicit : result.rectList->buffers) occupied.push_back(implicit.index);
        const auto activeBuffer = [&](const ResourceMapping& mapping) {
            const auto present = [&](const auto& reflected) {
                return std::any_of(reflected.begin(), reflected.end(), [&](const auto& resource) {
                    return compiler.get_decoration(resource.id, spv::DecorationDescriptorSet) == mapping.descriptorSet &&
                        compiler.get_decoration(resource.id, spv::DecorationBinding) == mapping.binding;
                });
            };
            return present(activeResources.uniform_buffers) || present(activeResources.storage_buffers);
        };
        for (const auto& mapping : result.resources)
            if (mapping.buffer && activeBuffer(mapping))
                for (std::uint32_t i = 0; i < mapping.count; ++i) occupied.push_back(*mapping.buffer + i);
        auto slot = target.maxBuffers;
        while (slot != 0) {
            --slot;
            if (std::find(occupied.begin(), occupied.end(), slot) == occupied.end()) break;
        }
        if (std::find(occupied.begin(), occupied.end(), slot) != occupied.end())
            Fail("sampler argument buffer has no available native buffer slot");
        result.samplerArgumentBuffer = slot;
        spirv_cross::MSLResourceBinding bank{};
        bank.stage = execution;
        bank.desc_set = 1;
        bank.binding = spirv_cross::kArgumentBufferBinding;
        bank.msl_buffer = slot;
        compiler.add_msl_resource_binding(bank);
        result.samplerArgumentCount = sampler;
        for (const auto& resource : resources.separate_samplers)
            compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 1);
    }
    if (!resources.push_constant_buffers.empty()) {
        spirv_cross::MSLResourceBinding push{};
        push.stage = execution;
        push.desc_set = spirv_cross::kPushConstDescSet;
        push.binding = spirv_cross::kPushConstBinding;
        push.msl_buffer = target.pushConstantBuffer;
        compiler.add_msl_resource_binding(push);
        result.pushConstantBuffer = target.pushConstantBuffer;
        const auto size = compiler.get_declared_struct_size(compiler.get_type(resources.push_constant_buffers[0].base_type_id));
        if (size > std::numeric_limits<std::uint32_t>::max()) Fail("push-constant block size overflow");
        result.pushConstantBytes = static_cast<std::uint32_t>(size);
        if (target.pushConstantOffsetBytes > size || guest.pushConstants.size() > size - target.pushConstantOffsetBytes)
            Fail("materialized push-constant region exceeds the shader block");
        result.pushConstantData.resize(size);
        std::copy(guest.pushConstants.begin(), guest.pushConstants.end(), result.pushConstantData.begin() + target.pushConstantOffsetBytes);
    }
    try { result.source = compiler.compile(); }
    catch (const std::exception& error) { Fail(error.what()); }
    for (const auto& image : resources.storage_images)
        if (compiler.get_automatic_msl_resource_binding_secondary(image.id) != std::numeric_limits<std::uint32_t>::max())
            Fail("image atomic emulation requires an unimplemented secondary Metal buffer contract");
    result.entryPoint = compiler.get_cleansed_entry_point_name(entries[0].name, execution);
    for (auto& mapping : result.resources)
        mapping.active = compiler.is_msl_resource_binding_used(execution,
            result.samplerArgumentBuffer && mapping.kind == DescriptorKind::Sampler ? 1u : mapping.descriptorSet, mapping.binding);
    for (const auto& resource : resources.storage_buffers) {
        for (auto& mapping : result.resources)
            if (mapping.descriptorSet == compiler.get_decoration(resource.id, spv::DecorationDescriptorSet) &&
                mapping.binding == compiler.get_decoration(resource.id, spv::DecorationBinding))
                mapping.requiresByteLengths = compiler.buffer_requires_array_length(resource.id);
    }
    if (compiler.needs_buffer_size_buffer()) result.bufferSizesBuffer = target.bufferSizesBuffer;
    if (result.samplerArgumentBuffer && (result.samplerArgumentBuffer == result.pushConstantBuffer ||
        result.samplerArgumentBuffer == result.bufferSizesBuffer))
        Fail("sampler argument buffer overlaps an active auxiliary buffer");
    if (compiler.needs_swizzle_buffer() || compiler.needs_view_mask_buffer() || compiler.needs_depth_clip_state_buffer() ||
        compiler.needs_dispatch_base_buffer() || compiler.needs_patch_output_buffer() ||
        (compiler.needs_output_buffer() != (capture || control)) ||
        (compiler.needs_input_threadgroup_mem() && !control))
        Fail("unimplemented implicit Metal resource contract");
    if (rectangle) {
        if (control || evaluation) {
            result.rectList->inputLayout = compiler.InputLayout();
            const auto& actual = result.rectList->inputLayout;
            const auto& expected = *target.rectListInputLayout;
            if (actual.stride != expected.stride || actual.alignment != expected.alignment ||
                actual.members.size() != expected.members.size())
                Fail("rectangle raw input packing does not match the upstream output");
            for (std::size_t i = 0; i < actual.members.size(); ++i) {
                const auto& a = actual.members[i];
                const auto& e = expected.members[i];
                if (a.location != e.location || a.builtin != e.builtin || a.components != e.components ||
                    a.width != e.width || a.offset != e.offset || a.bytes != e.bytes)
                    Fail("rectangle raw input semantic packing does not match the upstream output");
            }
        }
        if (capture || control) {
            result.rectList->outputLayout = compiler.OutputLayout();
            for (auto& implicit : result.rectList->buffers)
                if (implicit.role == ImplicitBufferRole::StageOutput)
                    implicit.elementBytes = result.rectList->outputLayout.stride;
        }
    }
    if (stage == ShaderStage::Compute) for (std::uint32_t i = 0; i < 3; ++i)
        result.threadsPerThreadgroup[i] = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, i);
    if (stage == ShaderStage::Compute && std::any_of(result.threadsPerThreadgroup.begin(), result.threadsPerThreadgroup.end(), [](auto dimension) { return dimension == 0; }))
        Fail("specialized or missing compute workgroup dimensions require an explicit dispatch specialization contract");
    return result;
}
}
