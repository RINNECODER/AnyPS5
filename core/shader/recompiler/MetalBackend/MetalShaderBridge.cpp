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

protected:
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
    spv::ExecutionModel execution;
    switch (stage) {
    case ShaderStage::Compute: execution = spv::ExecutionModelGLCompute; break;
    case ShaderStage::Vertex: execution = spv::ExecutionModelVertex; break;
    case ShaderStage::Fragment: execution = spv::ExecutionModelFragment; break;
    case ShaderStage::Mesh: execution = spv::ExecutionModelMeshEXT; break;
    default: Fail("stage requires unimplemented native Metal scheduling");
    }
    if (guest.spirv.empty()) Fail("empty SPIR-V module");
    if (target.mslVersion < 20200) Fail("MSL 2.2 or newer is required");
    if (stage == ShaderStage::Mesh && target.mslVersion < 30000) Fail("mesh shaders require MSL 3.0 or newer");
    if (target.maxBuffers > 31 || target.maxBuffers < 3 || target.maxTextures > 128 || target.maxSamplers > 16)
        Fail("invalid Metal resource limits");
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
    result.vertexBufferCount = vertexBufferCount;
    result.guest = guest;
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
        if (guest.bdaAbiVersion != BdaAbi::Version || !result.requiresGpuAddresses)
            Fail("incompatible BDA ABI or missing physical-address capability");
        for (const auto role : {DescriptorRole::BdaPagetable, DescriptorRole::FaultBuffer})
            if (std::none_of(guest.bindings.begin(), guest.bindings.end(), [role](const auto& binding) { return binding.role == role; }))
                Fail("BDA ABI requires page-table and fault-buffer descriptor metadata");
    }

    auto options = compiler.get_msl_options();
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.msl_version = target.mslVersion;
    options.buffer_size_buffer_index = target.bufferSizesBuffer;
    options.texture_buffer_native = true;
    options.argument_buffers = false;
    compiler.set_msl_options(options);
    auto common = compiler.get_common_options();
    common.vertex.flip_vert_y = stage == ShaderStage::Vertex && target.flipVertexY;
    common.vertex.fixup_clipspace = stage == ShaderStage::Vertex && target.fixupClipSpace;
    compiler.set_common_options(common);
    const auto resources = compiler.get_shader_resources();
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

    std::uint32_t buffer = vertexBufferCount, texture = 0, sampler = 0;
    const auto allocateBuffer = [&](std::uint32_t count) {
        while (buffer == target.pushConstantBuffer || buffer == target.bufferSizesBuffer) ++buffer;
        const auto first = buffer;
        if (count > target.maxBuffers || first > target.maxBuffers - count) Fail("buffer descriptors exceed native Metal slots");
        for (std::uint32_t i = first; i < first + count; ++i)
            if (i == target.pushConstantBuffer || i == target.bufferSizesBuffer) Fail("descriptor array overlaps auxiliary Metal slots");
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
            if (binding.count > target.maxSamplers || sampler > target.maxSamplers - binding.count)
                Fail("sampler descriptors exceed native Metal slots");
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
        mapping.active = compiler.is_msl_resource_binding_used(execution, mapping.descriptorSet, mapping.binding);
    for (const auto& resource : resources.storage_buffers) {
        for (auto& mapping : result.resources)
            if (mapping.descriptorSet == compiler.get_decoration(resource.id, spv::DecorationDescriptorSet) &&
                mapping.binding == compiler.get_decoration(resource.id, spv::DecorationBinding))
                mapping.requiresByteLengths = compiler.buffer_requires_array_length(resource.id);
    }
    if (compiler.needs_buffer_size_buffer()) result.bufferSizesBuffer = target.bufferSizesBuffer;
    if (compiler.needs_swizzle_buffer() || compiler.needs_view_mask_buffer() || compiler.needs_depth_clip_state_buffer() ||
        compiler.needs_dispatch_base_buffer() || compiler.needs_output_buffer() || compiler.needs_patch_output_buffer() ||
        compiler.needs_input_threadgroup_mem())
        Fail("unimplemented implicit Metal resource contract");
    if (stage == ShaderStage::Compute) for (std::uint32_t i = 0; i < 3; ++i)
        result.threadsPerThreadgroup[i] = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, i);
    if (stage == ShaderStage::Compute && std::any_of(result.threadsPerThreadgroup.begin(), result.threadsPerThreadgroup.end(), [](auto dimension) { return dimension == 0; }))
        Fail("specialized or missing compute workgroup dimensions require an explicit dispatch specialization contract");
    return result;
}
}
