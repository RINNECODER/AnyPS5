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
    default: Fail("stage requires unimplemented native Metal scheduling");
    }
    if (guest.spirv.empty()) Fail("empty SPIR-V module");
    if (target.mslVersion < 20200) Fail("MSL 2.2 or newer is required");
    if (target.maxBuffers > 31 || target.maxBuffers < 3 || target.maxTextures > 128 || target.maxSamplers > 16)
        Fail("invalid Metal resource limits");
    if (target.pushConstantBuffer >= target.maxBuffers || target.bufferSizesBuffer >= target.maxBuffers ||
        target.pushConstantBuffer == target.bufferSizesBuffer)
        Fail("invalid or overlapping auxiliary buffer indices");

    spirv_cross::CompilerMSL compiler(guest.spirv.Words());
    const auto entries = compiler.get_entry_points_and_stages();
    if (entries.size() != 1 || entries[0].execution_model != execution)
        Fail("expected exactly one entry point matching the requested stage");
    compiler.set_entry_point(entries[0].name, execution);
    if (stage == ShaderStage::Compute) {
        spirv_cross::SpecializationConstant x{}, y{}, z{};
        compiler.get_work_group_size_specialization_constants(x, y, z);
        if (x.id || y.id || z.id)
            Fail("specialized compute workgroup dimensions require an explicit dispatch specialization contract");
    }

    Result result;
    result.stage = stage;
    result.guest = guest;
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
    const auto resources = compiler.get_shader_resources();
    if (!resources.sampled_images.empty() || !resources.subpass_inputs.empty() ||
        !resources.atomic_counters.empty() || !resources.acceleration_structures.empty())
        Fail("combined images, subpass inputs, atomic counters, and acceleration structures are outside this compute slice");
    if (resources.push_constant_buffers.size() > 1) Fail("multiple push-constant blocks");

    std::uint32_t buffer = 0, texture = 0, sampler = 0;
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
