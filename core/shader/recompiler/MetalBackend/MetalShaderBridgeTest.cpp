#include "MetalShaderBridge.hpp"
#include "SubgroupVertexFixture.hpp"
#include <spirv.hpp>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace ShaderRecompiler;
using namespace ShaderRecompiler::MetalBackend;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static RecompileResult Sample(std::initializer_list<spv::Capability> extra = {}, bool specialized = false, bool push = false) {
    std::vector<std::uint32_t> words{spv::MagicNumber, 0x00010300u, 0u, 24u, 0u};
    const auto emit = [&](spv::Op op, std::initializer_list<std::uint32_t> args) {
        words.push_back((static_cast<std::uint32_t>(args.size() + 1) << 16u) | op);
        words.insert(words.end(), args);
    };
    emit(spv::OpCapability, {spv::CapabilityShader});
    for (auto capability : extra) emit(spv::OpCapability, {capability});
    emit(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, 10, 0x6e69616du, 0});
    if (specialized) emit(spv::OpExecutionModeId, {10, spv::ExecutionModeLocalSizeId, 16, 16, 16});
    else emit(spv::OpExecutionMode, {10, spv::ExecutionModeLocalSize, 1, 1, 1});
    if (specialized) emit(spv::OpDecorate, {16, spv::DecorationSpecId, 1});
    emit(spv::OpDecorate, {4, spv::DecorationArrayStride, 4});
    emit(spv::OpDecorate, {5, spv::DecorationBlock});
    emit(spv::OpMemberDecorate, {5, 0, spv::DecorationOffset, 0});
    emit(spv::OpDecorate, {7, spv::DecorationDescriptorSet, 2});
    emit(spv::OpDecorate, {7, spv::DecorationBinding, 7});
    if (push) {
        emit(spv::OpDecorate, {17, spv::DecorationBlock});
        emit(spv::OpMemberDecorate, {17, 0, spv::DecorationOffset, 16});
    }
    emit(spv::OpTypeVoid, {1});
    emit(spv::OpTypeFunction, {2, 1});
    emit(spv::OpTypeInt, {3, 32, 0});
    emit(spv::OpTypeRuntimeArray, {4, 3});
    emit(spv::OpTypeStruct, {5, 4});
    emit(spv::OpTypePointer, {6, spv::StorageClassStorageBuffer, 5});
    emit(spv::OpTypePointer, {8, spv::StorageClassStorageBuffer, 3});
    emit(spv::OpConstant, {3, 9, 0});
    emit(spv::OpConstant, {3, 14, 42});
    if (specialized) emit(spv::OpSpecConstant, {3, 16, 4});
    if (push) {
        emit(spv::OpTypeStruct, {17, 3});
        emit(spv::OpTypePointer, {18, spv::StorageClassPushConstant, 17});
        emit(spv::OpTypePointer, {20, spv::StorageClassPushConstant, 3});
        emit(spv::OpVariable, {18, 19, spv::StorageClassPushConstant});
    }
    emit(spv::OpVariable, {6, 7, spv::StorageClassStorageBuffer});
    emit(spv::OpFunction, {1, 10, spv::FunctionControlMaskNone, 2});
    emit(spv::OpLabel, {11});
    emit(spv::OpArrayLength, {3, 12, 7, 0});
    emit(spv::OpIAdd, {3, 13, 12, 14});
    emit(spv::OpAccessChain, {8, 15, 7, 9, 9});
    if (push) {
        emit(spv::OpAccessChain, {20, 21, 19, 9});
        emit(spv::OpLoad, {3, 22, 21});
        emit(spv::OpIAdd, {3, 23, 13, 22});
    }
    emit(spv::OpStore, {15, push ? 23u : 13u});
    emit(spv::OpReturn, {});
    emit(spv::OpFunctionEnd, {});
    RecompileResult guest;
    guest.spirv = std::move(words);
    DescriptorBinding binding{};
    binding.kind = DescriptorKind::StorageBuffer;
    binding.role = DescriptorRole::GuestBuffers;
    binding.descriptorSet = 2;
    binding.binding = 7;
    binding.count = 1;
    binding.bufferWritten = {true};
    guest.bindings.push_back(std::move(binding));
    guest.variantId = 191;
    guest.hostSubgroupSize = 32;
    if (push) {
        const std::uint32_t bias = 11;
        guest.pushConstants.resize(sizeof(bias));
        std::memcpy(guest.pushConstants.data(), &bias, sizeof(bias));
    }
    return guest;
}

template<class Callable>
static void Reject(Callable callable, const char* reason) {
    try { callable(); }
    catch (const std::exception& error) {
        Require(std::strstr(error.what(), reason) != nullptr, error.what());
        return;
    }
    throw std::runtime_error(std::string("accepted invalid contract: ") + reason);
}

// The conversion cache must return what an uncached conversion returns: a hit needs the same SPIR-V, guest
// metadata the bridge reads, stage, and every target option; the returned guest is always the caller's.
static void VerifyConversionCache(const RecompileResult& sample, const RecompileResult& pushGuest, TargetOptions target) {
    ClearConversionCache();
    target.pushConstantOffsetBytes = 0;
    const auto cold = ConvertToMetal(sample, ShaderStage::Compute, target);
    auto stats = GetConversionCacheStatistics();
    Require(stats.misses == 1 && stats.hits == 0 && stats.entries == 1, "first conversion was not a counted cache miss");
    auto relabeled = sample;
    relabeled.variantId = 777;
    const auto warm = ConvertToMetal(relabeled, ShaderStage::Compute, target);
    stats = GetConversionCacheStatistics();
    Require(stats.hits == 1 && stats.misses == 1, "identical conversion inputs missed the cache");
    Require(warm.source == cold.source && warm.entryPoint == cold.entryPoint && warm.resources.size() == cold.resources.size(),
            "cached conversion differs from the uncached result");
    Require(warm.guest.variantId == 777 && cold.guest.variantId == 191, "cached conversion returned another caller's guest metadata");

    const auto pushCold = ConvertToMetal(pushGuest, ShaderStage::Compute, target);
    auto shifted = target;
    shifted.pushConstantOffsetBytes = 16;
    const auto pushShifted = ConvertToMetal(pushGuest, ShaderStage::Compute, shifted);
    stats = GetConversionCacheStatistics();
    Require(stats.misses == 3 && stats.hits == 1, "different target options shared one cache entry");
    Require(pushShifted.pushConstantBytes == 20 && pushCold.pushConstantData != pushShifted.pushConstantData, "push offset variants returned the wrong layout");
    auto flipped = target;
    flipped.flipVertexY = !flipped.flipVertexY;
    static_cast<void>(ConvertToMetal(sample, ShaderStage::Compute, flipped));
    Require(GetConversionCacheStatistics().misses == 4, "a target option outside the compute contract still must key the cache");

    auto described = sample;
    described.bindings[0].guestDescriptor.assign(4 * described.bindings[0].count, 0x1234u);
    static_cast<void>(ConvertToMetal(described, ShaderStage::Compute, target));
    stats = GetConversionCacheStatistics();
    Require(stats.misses == 5 && stats.hits == 1, "guest descriptor words did not key the cache");
    // Push-constant values do not change the converted shader: a draw whose push constants differ
    // hits the conversion of the same module and gets its own values in pushConstantData, exactly
    // as an uncached conversion materializes them.
    auto pushed = pushGuest;
    pushed.pushConstants[0] = std::byte{0x5a};
    const auto pushedShifted = ConvertToMetal(pushed, ShaderStage::Compute, shifted);
    stats = GetConversionCacheStatistics();
    Require(stats.misses == 5 && stats.hits == 2, "a push-constant variant of a converted module missed the cache");
    Require(pushedShifted.pushConstantData != pushShifted.pushConstantData && pushedShifted.pushConstantData.size() == 20 &&
            pushedShifted.pushConstantData[16] == std::byte{0x5a}, "a cached conversion returned another caller's push constants");
    Require(ConvertToMetal(pushShifted.guest, ShaderStage::Compute, shifted).pushConstantData == pushShifted.pushConstantData,
            "a cached conversion kept the push constants of the previous caller");
    ClearConversionCache();
    const auto pushedFresh = ConvertToMetal(pushed, ShaderStage::Compute, shifted);
    Require(pushedFresh.pushConstantData == pushedShifted.pushConstantData && pushedFresh.source == pushedShifted.source &&
            pushedFresh.resources.size() == pushedShifted.resources.size(), "a push-constant variant served from the cache differs from its uncached conversion");
    Require(ConvertToMetal(pushed, ShaderStage::Compute, shifted).pushConstantData == pushedFresh.pushConstantData &&
            GetConversionCacheStatistics().hits == 1, "repeated push conversion missed the cache");
    ClearConversionCache();
    stats = GetConversionCacheStatistics();
    Require(stats.entries == 0 && stats.bytes == 0, "clearing the conversion cache kept entries");
}

int main() {
    try {
        const auto sample = Sample();
        TargetOptions target;
        target.pushConstantBuffer = 0;
        target.bufferSizesBuffer = 1;
        const auto converted = ConvertToMetal(sample, ShaderStage::Compute, target);
        Require(!converted.source.empty() && !converted.entryPoint.empty(), "conversion omitted executable MSL");
        Require(converted.stage == ShaderStage::Compute && converted.threadsPerThreadgroup == std::array<std::uint32_t, 3>{1, 1, 1}, "lost stage/workgroup contract");
        Require(converted.resources.size() == 1 && converted.resources[0].active && converted.resources[0].buffer == 2,
                "descriptor remapping overlaps auxiliary buffers");
        Require(converted.resources[0].descriptorSet == 2 && converted.resources[0].binding == 7 &&
                converted.resources[0].requiresByteLengths && converted.bufferSizesBuffer == 1,
                "array-length resource reflection omitted byte-length binding");
        Require(converted.guest.variantId == 191 && converted.guest.hostSubgroupSize == 32 &&
                converted.guest.bindings[0].bufferWritten == std::vector<bool>{true}, "lost guest variant/write contract");
        const auto pushGuest = Sample({}, false, true);
        target.pushConstantOffsetBytes = 16;
        const auto withPush = ConvertToMetal(pushGuest, ShaderStage::Compute, target);
        Require(withPush.pushConstantBuffer == 0 && withPush.pushConstantBytes == 20 && withPush.pushConstantData.size() == 20, "lost declared push block size");
        Require(std::all_of(withPush.pushConstantData.begin(), withPush.pushConstantData.begin() + 16, [](auto byte) { return byte == std::byte{}; }) &&
                std::equal(pushGuest.pushConstants.begin(), pushGuest.pushConstants.end(), withPush.pushConstantData.begin() + 16), "stage push region was not padded at its original offset");
        Require(withPush.guest.pushConstants == pushGuest.pushConstants, "changed original guest push region");
        target.pushConstantOffsetBytes = 17;
        Reject([&] { static_cast<void>(ConvertToMetal(pushGuest, ShaderStage::Compute, target)); }, "region exceeds the shader block");
        auto invalid = sample;
        invalid.bindings.clear();
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "missing from guest binding metadata");
        invalid = sample;
        invalid.bindings[0].count = 2;
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "kind/count disagrees");
        invalid = sample;
        invalid.bindings[0].kind = DescriptorKind::UniformBuffer;
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "kind/count disagrees");
        invalid = sample;
        invalid.bindings.push_back(invalid.bindings[0]);
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "duplicate descriptor");
        Reject([&] { static_cast<void>(ConvertToMetal(sample, ShaderStage::Vertex)); }, "matching the requested stage");
        Reject([&] { static_cast<void>(ConvertToMetal(sample, ShaderStage::TessellationControl)); }, "unimplemented native Metal scheduling");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityFloat64}), ShaderStage::Compute)); }, "Float64");
        // A rejected conversion is remembered: the same input fails again with the same error.
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityFloat64}), ShaderStage::Compute)); }, "Float64");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityInt64Atomics}), ShaderStage::Compute)); }, "64-bit atomics");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityInt64}), ShaderStage::Compute)); }, "lacks 64-bit integer");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityPhysicalStorageBufferAddresses}), ShaderStage::Compute)); }, "lacks GPU-address");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityGroupNonUniform}), ShaderStage::Compute)); }, "lacks SIMD-group");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({}, true), ShaderStage::Compute)); }, "specialized compute workgroup");
        invalid = sample;
        invalid.bdaAbiVersion = 1;
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "incompatible BDA ABI");
        VerifyConversionCache(sample, pushGuest, target);
        {
            // Guest lane work in a vertex function: Metal has no [[thread_index_in_simdgroup]] or
            // [[threads_per_simdgroup]] vertex input, so the lane comes from an entry-time prefix count.
            TargetOptions simd;
            simd.supportsSimdGroups = true;
            const auto lane = ConvertToMetal(Fixtures::SubgroupLaneVertex(), ShaderStage::Vertex, simd);
            Require(lane.requiresSimdGroups && lane.nativeExecutionKind == NativeExecutionKind::Vertex, "lost vertex SIMD-group contract");
            Require(lane.source.find("= simd_prefix_exclusive_sum(1u);") != std::string::npos &&
                    lane.source.find("simd_shuffle(") != std::string::npos && lane.source.find("simd_ballot(") != std::string::npos,
                    "vertex lane, shuffle or ballot was not lowered to SIMD-group functions");
            Require(lane.source.find("thread_index_in_simdgroup") == std::string::npos &&
                    lane.source.find("threads_per_simdgroup") == std::string::npos && lane.source.find("thread_execution_width") == std::string::npos,
                    "vertex function declares a SIMD-group input Metal does not provide");
            Reject([&] { static_cast<void>(ConvertToMetal(Fixtures::SubgroupLaneVertex(64), ShaderStage::Vertex, simd)); }, "32-wide host subgroup");
            Reject([&] { static_cast<void>(ConvertToMetal(Fixtures::SubgroupLaneVertex(), ShaderStage::Vertex)); }, "lacks SIMD-group");
        }
        {
            std::string snippet =
                "static __attribute__((noinline))\n"
                "ulong get_bda_pointer(constant ulong* bda_pagetable, ulong address, uint bytes, uint instruction);\n"
                "[[clang::noinline]] ulong probe_bda_pointer(constant ulong* bda_pagetable, ulong address, uint bytes, uint instruction);\n"
                "[[clang::optnone]] uint4 read_bda_span(ulong address, uint offset, uint used, uint instruction);\n"
                "static __attribute__((noinline))\n"
                "void write_bda_bytes(ulong address, uint value, uint bytes, uint instruction);\n"
                "static __attribute__((noinline))\n"
                "void record_bda_fault(constant ulong* bda_pagetable, ulong address, uint bytes, uint instruction, uint reason);\n";
            InlineBdaHelpers(snippet);
            Require(snippet.find("static inline __attribute__((always_inline))\nulong get_bda_pointer") != std::string::npos, "get_bda_pointer was not made inline");
            Require(snippet.find("[[clang::noinline]]") == std::string::npos && snippet.find("ulong probe_bda_pointer") != std::string::npos, "probe_bda_pointer retained noinline");
            Require(snippet.find("[[clang::optnone]]") == std::string::npos && snippet.find("uint4 read_bda_span") != std::string::npos, "read_bda_span retained optnone");
            Require(snippet.find("static inline __attribute__((always_inline))\nvoid write_bda_bytes") != std::string::npos, "write_bda_bytes was not made inline");
            Require(snippet.find("static __attribute__((noinline))\nvoid record_bda_fault") != std::string::npos, "record_bda_fault should retain noinline");
        }
        {
            std::vector<std::uint32_t> bdaWords{spv::MagicNumber, 0x00010300u, 0u, 30u, 0u};
            const auto emitBda = [&](spv::Op op, std::initializer_list<std::uint32_t> args) {
                bdaWords.push_back((static_cast<std::uint32_t>(args.size() + 1) << 16u) | op);
                bdaWords.insert(bdaWords.end(), args);
            };
            emitBda(spv::OpCapability, {spv::CapabilityShader});
            emitBda(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
            emitBda(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, 10, 0x6e69616du, 0});
            emitBda(spv::OpExecutionMode, {10, spv::ExecutionModeLocalSize, 1, 1, 1});
            emitBda(spv::OpName, {20, 0x5f746567u, 0x5f616462u, 0x6e696f70u, 0x00726574u});
            emitBda(spv::OpName, {21, 0x6f636572u, 0x625f6472u, 0x665f6164u, 0x746c7561u, 0});
            emitBda(spv::OpTypeVoid, {1});
            emitBda(spv::OpTypeFunction, {2, 1});
            emitBda(spv::OpFunction, {1, 10, spv::FunctionControlMaskNone, 2});
            emitBda(spv::OpLabel, {11});
            emitBda(spv::OpFunctionCall, {1, 12, 20});
            emitBda(spv::OpFunctionCall, {1, 13, 21});
            emitBda(spv::OpReturn, {});
            emitBda(spv::OpFunctionEnd, {});
            emitBda(spv::OpFunction, {1, 20, spv::FunctionControlDontInlineMask, 2});
            emitBda(spv::OpLabel, {22});
            emitBda(spv::OpReturn, {});
            emitBda(spv::OpFunctionEnd, {});
            emitBda(spv::OpFunction, {1, 21, spv::FunctionControlDontInlineMask, 2});
            emitBda(spv::OpLabel, {23});
            emitBda(spv::OpReturn, {});
            emitBda(spv::OpFunctionEnd, {});
            RecompileResult bdaGuest;
            bdaGuest.spirv = std::move(bdaWords);
            bdaGuest.variantId = 192;
            bdaGuest.hostSubgroupSize = 32;
            TargetOptions target;
            const auto bdaConverted = ConvertToMetal(bdaGuest, ShaderStage::Compute, target);
            Require(bdaConverted.source.find("get_bda_pointer") != std::string::npos, "missing get_bda_pointer in converted MSL");
            Require(bdaConverted.source.find("static inline __attribute__((always_inline))\nvoid get_bda_pointer") != std::string::npos, "expected always_inline on get_bda_pointer");
            Require(bdaConverted.source.find("record_bda_fault") != std::string::npos, "missing record_bda_fault in converted MSL");
            Require(bdaConverted.source.find("static __attribute__((noinline))\nvoid record_bda_fault") != std::string::npos, "record_bda_fault should stay noinline");
        }
        std::cout << "PASS: descriptor reflection, implicit buffer lengths, preserved guest metadata, unsupported-contract rejection, conversion cache keys, and vertex subgroup lanes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
