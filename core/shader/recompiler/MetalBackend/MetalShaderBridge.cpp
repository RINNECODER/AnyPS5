#include "MetalShaderBridge.hpp"
#include "../BdaAbi.hpp"
#include <spirv_msl.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace ShaderRecompiler::MetalBackend {
namespace {
[[noreturn]] void Fail(const std::string& reason) {
    throw std::runtime_error("Metal shader bridge: " + reason);
}

class MetalCompiler final : public spirv_cross::CompilerMSL {
public:
    using CompilerMSL::CompilerMSL;

    struct ShadowPair {
        std::uint32_t imageSet, imageBinding, imageElement;
        std::uint32_t samplerSet, samplerBinding, samplerElement;
        std::uint32_t magArgument, minArgument;
        float relativeViewMin, samplerMin, samplerMax, samplerBias, samplerAnisotropy;
    };

    struct DescriptorElement {
        std::uint32_t variable;
        std::optional<std::uint32_t> constant;
        std::uint32_t index;
    };

    struct ShadowSelection {
        std::uint32_t firstPair;
        std::optional<std::uint32_t> imageIndex;
    };

    struct QueryPair {
        std::vector<float> relativeViewMinimums;
        std::optional<std::uint32_t> imageIndex;
        float samplerMin, samplerMax;
        bool nearestMip;
        std::uint32_t samplerSet = 0, samplerBinding = 0, samplerElement = 0;
    };

    void PrepareMinimumLodCertificate(const RecompileResult& guest) {
        struct Definition { spv::Op op; std::vector<std::uint32_t> words; };
        struct Use { std::uint32_t operand, result; bool required, blocked; };
        std::unordered_map<std::uint32_t, Definition> definitions;
        std::vector<Use> uses;
        ir.for_each_typed_id<spirv_cross::SPIRBlock>([&](std::uint32_t, const auto& block) {
            for (const auto& instruction : block.ops) {
                const auto op = static_cast<spv::Op>(instruction.op);
                const auto* words = stream(instruction);
                if (op == spv::OpAccessChain || op == spv::OpLoad || op == spv::OpCopyObject ||
                    op == spv::OpSampledImage || op == spv::OpImage || op == spv::OpIAdd)
                    definitions.emplace(words[1], Definition{op, {words, words + instruction.length}});
                bool required = false, blocked = false;
                switch (op) {
                case spv::OpImageSampleImplicitLod:
                case spv::OpImageSampleExplicitLod:
                case spv::OpImageSampleDrefImplicitLod:
                case spv::OpImageSampleDrefExplicitLod:
                case spv::OpImageQueryLod:
                    required = true;
                    break;
                case spv::OpImageFetch:
                case spv::OpImageGather:
                case spv::OpImageDrefGather:
                case spv::OpImageQueryFormat:
                case spv::OpImageQueryOrder:
                case spv::OpImageQuerySizeLod:
                case spv::OpImageQuerySize:
                case spv::OpImageQueryLevels:
                case spv::OpImageQuerySamples:
                    break;
                case spv::OpImageSampleProjImplicitLod:
                case spv::OpImageSampleProjExplicitLod:
                case spv::OpImageSampleProjDrefImplicitLod:
                case spv::OpImageSampleProjDrefExplicitLod:
                case spv::OpImageSparseSampleImplicitLod:
                case spv::OpImageSparseSampleExplicitLod:
                case spv::OpImageSparseSampleDrefImplicitLod:
                case spv::OpImageSparseSampleDrefExplicitLod:
                case spv::OpImageSparseSampleProjImplicitLod:
                case spv::OpImageSparseSampleProjExplicitLod:
                case spv::OpImageSparseSampleProjDrefImplicitLod:
                case spv::OpImageSparseSampleProjDrefExplicitLod:
                case spv::OpImageSparseFetch:
                case spv::OpImageSparseGather:
                case spv::OpImageSparseDrefGather:
                case spv::OpImageSampleFootprintNV:
                case spv::OpImageRead:
                case spv::OpImageWrite:
                case spv::OpImageSparseRead:
                case spv::OpImageTexelPointer:
                    blocked = true;
                    break;
                case spv::OpImageSampleWeightedQCOM:
                case spv::OpImageBoxFilterQCOM:
                case spv::OpImageBlockMatchSSDQCOM:
                case spv::OpImageBlockMatchSADQCOM:
                case spv::OpImageBlockMatchWindowSSDQCOM:
                case spv::OpImageBlockMatchWindowSADQCOM:
                case spv::OpImageBlockMatchGatherSSDQCOM:
                case spv::OpImageBlockMatchGatherSADQCOM:
                    minimumLodCertificateUnresolved = true;
                    continue;
                default:
                    continue;
                }
                uses.push_back({words[op == spv::OpImageWrite ? 0 : 2],
                                op == spv::OpImageWrite ? 0u : words[1], required, blocked});
            }
        });
        const auto resolve = [&](std::uint32_t id) -> std::optional<DescriptorElement> {
            for (std::uint32_t depth = 0; depth < 8; ++depth) {
                const auto found = definitions.find(id);
                if (found == definitions.end()) return {};
                const auto& definition = found->second;
                const auto& words = definition.words;
                if (definition.op == spv::OpLoad || definition.op == spv::OpCopyObject ||
                    definition.op == spv::OpSampledImage || definition.op == spv::OpImage) {
                    id = words[2];
                    continue;
                }
                if (definition.op != spv::OpAccessChain || words.size() != 4) return {};
                const auto* constant = maybe_get<spirv_cross::SPIRConstant>(words[3]);
                if (constant && !constant->specialization)
                    return DescriptorElement{words[2], constant->scalar(), words[3]};
                const auto index = definitions.find(words[3]);
                if (index == definitions.end() || index->second.op != spv::OpIAdd || index->second.words.size() != 4)
                    return {};
                const auto& indexWords = index->second.words;
                const auto* base = maybe_get<spirv_cross::SPIRConstant>(indexWords[2]);
                const auto& type = get<spirv_cross::SPIRType>(indexWords[0]);
                if (!base || base->specialization || type.basetype != spirv_cross::SPIRType::UInt ||
                    type.width != 32 || type.vecsize != 1 || type.columns != 1 || !type.array.empty()) return {};
                return DescriptorElement{words[2], {}, words[3]};
            }
            return {};
        };
        for (const auto& image : guest.bindings) {
            if (image.role != DescriptorRole::GuestImages || image.kind != DescriptorKind::SampledImage) continue;
            if (image.count == 0 || image.count > 64 || image.guestDescriptor.size() != image.count * 8u) {
                minimumLodCertificateUnresolved = true;
                continue;
            }
            for (std::uint32_t i = 0; i < image.count; ++i) {
                const auto* words = image.guestDescriptor.data() + i * 8u;
                const auto base = (words[3] >> 12u) & 15u;
                auto last = (words[3] >> 16u) & 15u;
                const auto allocatedLast = (words[5] >> 4u) & 15u;
                if (base <= allocatedLast) last = std::min(last, allocatedLast);
                const auto minimum = static_cast<float>((words[1] >> 8u) & 4095u) / 256.0f;
                if (std::min(minimum, float(last)) > float(base))
                    minimumLodImageUses.try_emplace(std::array{image.descriptorSet, image.binding, i});
            }
        }
        for (const auto& use : uses) {
            const auto resolved = resolve(use.operand);
            if (!resolved) { minimumLodCertificateUnresolved = true; continue; }
            const auto descriptorSet = get_decoration(resolved->variable, spv::DecorationDescriptorSet);
            const auto binding = get_decoration(resolved->variable, spv::DecorationBinding);
            const auto image = std::find_if(guest.bindings.begin(), guest.bindings.end(), [&](const auto& descriptor) {
                return descriptor.descriptorSet == descriptorSet && descriptor.binding == binding;
            });
            if (image == guest.bindings.end()) { minimumLodCertificateUnresolved = true; continue; }
            if (image->role != DescriptorRole::GuestImages) continue;
            if (image->count == 0 || image->count > 64 || image->guestDescriptor.size() != image->count * 8u ||
                (resolved->constant && *resolved->constant >= image->count)) {
                minimumLodCertificateUnresolved = true;
                continue;
            }
            if (!resolved->constant && get<spirv_cross::SPIRConstant>(
                    definitions.at(resolved->index).words[2]).scalar() >= image->count) {
                minimumLodCertificateUnresolved = true;
                continue;
            }
            const auto first = resolved->constant.value_or(0u);
            const auto end = resolved->constant ? first + 1u : image->count;
            for (auto i = first; i < end; ++i) {
                const auto* words = image->guestDescriptor.data() + i * 8u;
                const auto base = (words[3] >> 12u) & 15u;
                auto last = (words[3] >> 16u) & 15u;
                const auto allocatedLast = (words[5] >> 4u) & 15u;
                if (base <= allocatedLast) last = std::min(last, allocatedLast);
                const auto minimum = static_cast<float>((words[1] >> 8u) & 4095u) / 256.0f;
                if (std::min(minimum, float(last)) <= float(base)) continue;
                auto& state = minimumLodImageUses[{descriptorSet, binding, i}];
                state.blocked |= use.blocked || image->kind != DescriptorKind::SampledImage;
                if (use.required) state.required.insert(use.result);
            }
        }
    }

    std::vector<MinimumLodImage> MinimumLodImages() const {
        std::vector<MinimumLodImage> images;
        if (minimumLodCertificateUnresolved) return images;
        for (const auto& [key, state] : minimumLodImageUses) {
            if (state.blocked || std::any_of(state.required.begin(), state.required.end(), [&](auto id) {
                    return !minimumLodEmittedOperations.contains(id);
                })) continue;
            images.push_back({key[0], key[1], key[2]});
        }
        return images;
    }

    void PrepareMinimumLod(const RecompileResult& guest) {
        struct Definition { spv::Op op; std::vector<std::uint32_t> operands; };
        std::unordered_map<std::uint32_t, Definition> definitions;
        std::vector<std::uint32_t> sampled, queried;
        ir.for_each_typed_id<spirv_cross::SPIRBlock>([&](std::uint32_t, const auto& block) {
            for (const auto& instruction : block.ops) {
                const auto op = static_cast<spv::Op>(instruction.op);
                const auto* words = stream(instruction);
                if (op == spv::OpAccessChain || op == spv::OpLoad || op == spv::OpCopyObject ||
                    op == spv::OpSampledImage || op == spv::OpIAdd)
                    definitions.emplace(words[1], Definition{op, {words, words + instruction.length}});
                if (op == spv::OpImageSampleExplicitLod || op == spv::OpImageSampleImplicitLod ||
                    op == spv::OpImageSampleDrefExplicitLod || op == spv::OpImageSampleDrefImplicitLod)
                    sampled.push_back(words[2]);
                if (op == spv::OpImageQueryLod) queried.push_back(words[2]);
            }
        });
        if (queried.empty() && minimumLodImageUses.empty()) return;
        const auto element = [&](std::uint32_t id, bool allowDynamic) -> DescriptorElement {
            for (std::uint32_t depth = 0; depth < 8; ++depth) {
                const auto found = definitions.find(id);
                if (found == definitions.end()) Fail("minimum LOD resource operand has no descriptor provenance");
                const auto& definition = found->second;
                const auto& words = definition.operands;
                if (definition.op == spv::OpLoad || definition.op == spv::OpCopyObject) { id = words[2]; continue; }
                if (definition.op != spv::OpAccessChain || words.size() != 4)
                    Fail("minimum LOD prototype requires original one-dimensional descriptor access chains");
                const auto* constant = maybe_get<spirv_cross::SPIRConstant>(words[3]);
                if (constant && !constant->specialization)
                    return {words[2], constant->scalar(), words[3]};
                if (!allowDynamic) Fail("minimum LOD prototype requires a constant original sampler element");
                const auto index = definitions.find(words[3]);
                if (index == definitions.end() || index->second.op != spv::OpIAdd || index->second.operands.size() != 4)
                    Fail("minimum LOD dynamic image requires original TableImageIndex provenance");
                const auto& indexWords = index->second.operands;
                const auto* base = maybe_get<spirv_cross::SPIRConstant>(indexWords[2]);
                const auto& indexType = get<spirv_cross::SPIRType>(indexWords[0]);
                if (!base || base->specialization || indexType.basetype != spirv_cross::SPIRType::UInt ||
                    indexType.width != 32 || indexType.vecsize != 1 || indexType.columns != 1 || !indexType.array.empty())
                    Fail("minimum LOD dynamic image requires original unsigned TableImageIndex base and slot");
                return {words[2], {}, words[3]};
            }
            Fail("minimum LOD descriptor provenance is cyclic");
        };
        const auto bindingFor = [&](std::uint32_t variable) -> const DescriptorBinding& {
            const auto descriptorSet = get_decoration(variable, spv::DecorationDescriptorSet);
            const auto binding = get_decoration(variable, spv::DecorationBinding);
            const auto found = std::find_if(guest.bindings.begin(), guest.bindings.end(), [&](const auto& descriptor) {
                return descriptor.descriptorSet == descriptorSet && descriptor.binding == binding;
            });
            if (found == guest.bindings.end()) Fail("minimum LOD descriptor has no original logical binding");
            return *found;
        };
        const auto relativeMin = [](const DescriptorBinding& image, std::uint32_t imageElement) {
            const auto* words = image.guestDescriptor.data() + imageElement * 8u;
            const auto base = (words[3] >> 12u) & 15u;
            auto last = (words[3] >> 16u) & 15u;
            const auto allocatedLast = (words[5] >> 4u) & 15u;
            if (base <= allocatedLast) last = std::min(last, allocatedLast);
            const auto absolute = static_cast<float>((words[1] >> 8u) & 4095u) / 256.0f;
            return std::max(0.0f, std::min(absolute, float(last)) - float(base));
        };
        struct Access {
            std::uint32_t combined;
            DescriptorElement image, sampler;
            std::uint32_t samplerType;
        };
        std::vector<Access> accesses;
        auto combinedOperands = sampled;
        for (const auto combined : queried)
            if (std::find(combinedOperands.begin(), combinedOperands.end(), combined) == combinedOperands.end())
                combinedOperands.push_back(combined);
        for (const auto combined : combinedOperands) {
            const auto definition = definitions.find(combined);
            if (definition == definitions.end() || definition->second.op != spv::OpSampledImage ||
                definition->second.operands.size() != 4)
                Fail("minimum LOD sample has no original sampled-image provenance");
            const auto& words = definition->second.operands;
            const auto imageElement = element(words[2], true);
            const auto samplerElement = element(words[3], false);
            const auto& image = bindingFor(imageElement.variable);
            const auto& sampler = bindingFor(samplerElement.variable);
            if (image.role != DescriptorRole::GuestImages || sampler.role != DescriptorRole::GuestSamplers ||
                image.count == 0 || image.count > 64 || sampler.count == 0 || sampler.count > 32 ||
                (imageElement.constant && *imageElement.constant >= image.count) ||
                *samplerElement.constant >= sampler.count ||
                image.guestDescriptor.size() != image.count * 8u || sampler.guestDescriptor.size() != sampler.count * 4u)
                Fail("minimum LOD final physical descriptor metadata is invalid");
            if (!imageElement.constant) {
                const auto& indexWords = definitions.at(imageElement.index).operands;
                if (get<spirv_cross::SPIRConstant>(indexWords[2]).scalar() >= image.count)
                    Fail("minimum LOD TableImageIndex root exceeds the captured physical binding");
            }
            if (std::find(queried.begin(), queried.end(), combined) != queried.end()) {
                const auto* samplerWords = sampler.guestDescriptor.data() + *samplerElement.constant * 4u;
                const auto mip = (samplerWords[2] >> 26u) & 3u;
                const auto minimum = mip ? float(samplerWords[1] & 4095u) / 256.0f : 0.0f;
                const auto maximum = mip ? float((samplerWords[1] >> 12u) & 4095u) / 256.0f : 0.0f;
                if (mip > 2u || minimum > maximum) Fail("minimum LOD query sampler metadata is invalid");
                QueryPair pair{{}, {}, minimum, maximum, mip != 2u};
                pair.samplerSet = sampler.descriptorSet;
                pair.samplerBinding = sampler.binding;
                pair.samplerElement = *samplerElement.constant;
                if (imageElement.constant) {
                    pair.relativeViewMinimums.push_back(relativeMin(image, *imageElement.constant));
                } else {
                    pair.imageIndex = imageElement.index;
                    for (std::uint32_t i = 0; i < image.count; ++i)
                        pair.relativeViewMinimums.push_back(relativeMin(image, i));
                }
                queryPairs.emplace(combined, std::move(pair));
            }
            if (std::find(sampled.begin(), sampled.end(), combined) != sampled.end())
                accesses.push_back({combined, imageElement, samplerElement, definitions.at(words[3]).operands[0]});
        }
        using PairKey = std::array<std::uint32_t, 6>;
        using GroupKey = std::array<std::uint32_t, 5>;
        std::map<PairKey, std::uint32_t> unique;
        std::map<GroupKey, std::uint32_t> groups;
        const auto groupKey = [](const DescriptorBinding& image, const DescriptorBinding& sampler, std::uint32_t samplerElement) {
            return GroupKey{image.descriptorSet, image.binding, sampler.descriptorSet, sampler.binding, samplerElement};
        };
        const auto addPair = [&](const DescriptorBinding& image, std::uint32_t imageElement,
                                 const DescriptorBinding& sampler, std::uint32_t samplerElement) {
            const PairKey key{image.descriptorSet, image.binding, imageElement,
                              sampler.descriptorSet, sampler.binding, samplerElement};
            const auto existing = unique.find(key);
            if (existing != unique.end()) return existing->second;
            if (shadowPairs.size() >= 64u * 32u) Fail("minimum LOD pair bank exceeds original image and sampler capacity");
            const auto* words = sampler.guestDescriptor.data() + samplerElement * 4u;
            const auto mip = (words[2] >> 26u) & 3u;
            const auto bias = words[2] & 16383u;
            ShadowPair pair{key[0], key[1], key[2], key[3], key[4], key[5], 0, 0, relativeMin(image, imageElement),
                mip ? float(words[1] & 4095u) / 256.0f : 0.0f,
                mip ? float((words[1] >> 12u) & 4095u) / 256.0f : 0.0f,
                float(static_cast<std::int32_t>((bias ^ 8192u) - 8192u)) / 256.0f,
                (((words[2] >> 20u) & 3u) >= 2u || ((words[2] >> 22u) & 3u) >= 2u) ?
                    float(1u << std::min(4u, (words[0] >> 9u) & 7u)) : 1.0f};
            const auto index = static_cast<std::uint32_t>(shadowPairs.size());
            shadowPairs.push_back(pair);
            unique.emplace(key, index);
            return index;
        };
        for (const auto& access : accesses) {
            if (access.image.constant) continue;
            const auto& image = bindingFor(access.image.variable);
            const auto& sampler = bindingFor(access.sampler.variable);
            const auto key = groupKey(image, sampler, *access.sampler.constant);
            if (groups.contains(key)) continue;
            bool needsFloor = false;
            for (std::uint32_t i = 0; i < image.count; ++i) needsFloor |= relativeMin(image, i) != 0.0f;
            if (!needsFloor) continue;
            const auto firstPair = static_cast<std::uint32_t>(shadowPairs.size());
            for (std::uint32_t i = 0; i < image.count; ++i) {
                if (addPair(image, i, sampler, *access.sampler.constant) != firstPair + i)
                    Fail("minimum LOD dynamic image pair group is not contiguous");
            }
            groups.emplace(key, firstPair);
        }
        for (const auto& access : accesses) {
            const auto& image = bindingFor(access.image.variable);
            const auto& sampler = bindingFor(access.sampler.variable);
            const auto group = groups.find(groupKey(image, sampler, *access.sampler.constant));
            if (!access.image.constant) {
                if (group == groups.end()) continue;
                minimumLodPairs.emplace(access.combined, ShadowSelection{group->second, access.image.index});
            } else {
                const auto pair = group != groups.end() ? group->second + *access.image.constant :
                    relativeMin(image, *access.image.constant) != 0.0f ?
                    addPair(image, *access.image.constant, sampler, *access.sampler.constant) :
                    std::numeric_limits<std::uint32_t>::max();
                if (pair == std::numeric_limits<std::uint32_t>::max()) continue;
                minimumLodPairs.emplace(access.combined, ShadowSelection{pair, {}});
            }
            if (minimumLodSamplerType != 0 && minimumLodSamplerType != access.samplerType)
                Fail("minimum LOD original sampler type is inconsistent");
            minimumLodSamplerType = access.samplerType;
        }
    }

    void InstallMinimumLodBank(std::uint32_t originalCount, spv::ExecutionModel execution) {
        if (shadowPairs.empty()) return;
        if (originalCount > 32u || shadowPairs.size() > 64u * 32u)
            Fail("minimum LOD generated bank exceeds the original capture capacity");
        const auto ids = ir.increase_bound_by(3);
        auto array = get<spirv_cross::SPIRType>(minimumLodSamplerType);
        array.op = spv::OpTypeArray;
        array.parent_type = minimumLodSamplerType;
        array.array = {static_cast<std::uint32_t>(shadowPairs.size()) * 2u};
        array.array_size_literal = {true};
        set<spirv_cross::SPIRType>(ids, array);
        auto pointer = get<spirv_cross::SPIRType>(ids);
        pointer.op = spv::OpTypePointer;
        pointer.pointer = true;
        pointer.pointer_depth = 1;
        pointer.parent_type = ids;
        pointer.storage = spv::StorageClassUniformConstant;
        set<spirv_cross::SPIRType>(ids + 1, pointer);
        minimumLodBank = ids + 2;
        set<spirv_cross::SPIRVariable>(minimumLodBank, ids + 1, spv::StorageClassUniformConstant);
        set_name(minimumLodBank, "minimumLodSamplers");
        set_decoration(minimumLodBank, spv::DecorationDescriptorSet, 1);
        auto binding = 0u;
        const auto resources = get_shader_resources();
        while (std::any_of(resources.separate_samplers.begin(), resources.separate_samplers.end(), [&](const auto& sampler) {
            return sampler.id != minimumLodBank && get_decoration(sampler.id, spv::DecorationDescriptorSet) == 1 &&
                get_decoration(sampler.id, spv::DecorationBinding) == binding;
        })) ++binding;
        set_decoration(minimumLodBank, spv::DecorationBinding, binding);
        spirv_cross::MSLResourceBinding native{};
        native.stage = execution;
        native.desc_set = 1;
        native.binding = binding;
        native.basetype = spirv_cross::SPIRType::Sampler;
        native.count = static_cast<std::uint32_t>(shadowPairs.size()) * 2u;
        native.msl_sampler = originalCount;
        add_msl_resource_binding(native);
        for (std::uint32_t i = 0; i < shadowPairs.size(); ++i) {
            shadowPairs[i].magArgument = originalCount + i * 2u;
            shadowPairs[i].minArgument = originalCount + i * 2u + 1u;
        }
    }

    const std::vector<ShadowPair>& MinimumLodPairs() const { return shadowPairs; }
    bool RequiresTextureLodQueries() const { return requiresTextureLodQueries; }
    std::vector<CapturedSamplerRequirement> CapturedSamplers() const {
        std::vector<CapturedSamplerRequirement> result;
        const auto add = [&](std::uint32_t descriptorSet, std::uint32_t binding, std::uint32_t element) {
            const auto present = std::any_of(result.begin(), result.end(), [&](const auto& item) {
                return item.descriptorSet == descriptorSet && item.binding == binding && item.element == element;
            });
            if (!present) result.push_back({descriptorSet, binding, element});
        };
        for (const auto& pair : shadowPairs) add(pair.samplerSet, pair.samplerBinding, pair.samplerElement);
        for (const auto& [id, pair] : queryPairs) {
            static_cast<void>(id);
            add(pair.samplerSet, pair.samplerBinding, pair.samplerElement);
        }
        return result;
    }

    void ConfigureMeshPosition(std::uint32_t position, std::uint32_t vertices, bool flipY, bool fixupDepth) {
        meshPosition = position;
        meshVertices = vertices;
        meshFlipY = flipY;
        meshFixupDepth = fixupDepth;
    }

    InterfaceLayout InputLayout() const { return Layout(stage_in_var_id); }
    InterfaceLayout OutputLayout() const { return Layout(stage_out_var_id); }

protected:
    static std::string QueryNumber(float value) {
        std::ostringstream text;
        text << std::scientific << std::setprecision(std::numeric_limits<float>::max_digits10) << value << 'f';
        return text.str();
    }

    std::string QueryCoord(std::uint32_t imageId, std::uint32_t coordId) {
        auto coord = to_expression(coordId);
        const auto& image = expression_type(imageId);
        const auto& type = expression_type(coordId);
        switch (image.image.dim) {
            case spv::Dim1D:
                if (!get_msl_options().texture_1D_as_2D) Fail("logical 1D LOD queries require native 2D storage");
                if (type.vecsize > 1) coord = enclose_expression(coord) + ".x";
                return "float2(" + coord + ", 0.5f)";
            case spv::Dim2D:
                if (type.vecsize > 2) coord = enclose_expression(coord) + ".xy";
                return coord;
            case spv::Dim3D:
            case spv::DimCube:
                if (type.vecsize > 3) coord = enclose_expression(coord) + ".xyz";
                return coord;
            default:
                Fail("LOD query has an unsupported original image dimension");
        }
    }

    std::string QueryImageExpression(std::uint32_t imageId) {
        const auto* combined = maybe_get<spirv_cross::SPIRCombinedImageSampler>(imageId);
        return combined ? to_expression(combined->image) : to_expression(imageId);
    }

    std::string QuerySamplerLodExpression(std::uint32_t imageId, std::uint32_t coordId) {
        requiresTextureLodQueries = true;
        return QueryImageExpression(imageId) + ".calculate_unclamped_lod(" + to_sampler_expression(imageId) +
            ", " + QueryCoord(imageId, coordId) + ")";
    }

    void emit_instruction(const spirv_cross::Instruction& instruction) override {
        if (instruction.op != spv::OpImageQueryLod) {
            CompilerMSL::emit_instruction(instruction);
            return;
        }
        if (!get_msl_options().supports_msl_version(2, 2)) Fail("LOD queries require MSL 2.2 or newer");
        if (get_execution_model() != spv::ExecutionModelFragment) Fail("native LOD queries require a fragment stage");
        const auto* words = stream(instruction);
        const auto found = queryPairs.find(words[2]);
        if (found == queryPairs.end()) Fail("LOD query has no original descriptor metadata");
        const auto& pair = found->second;
        const auto id = words[1];
        emit_uninitialized_temporary_expression(words[0], id);
        const auto result = to_expression(id);
        statement(result, ".y = ", QuerySamplerLodExpression(words[2], words[3]), ";");
        std::string floor;
        if (pair.imageIndex) {
            const auto name = "spvMinimumLodQueryFloors" + std::to_string(id);
            std::string values;
            for (const auto value : pair.relativeViewMinimums) {
                if (!values.empty()) values += ", ";
                values += QueryNumber(value);
            }
            statement("const array<float, ", pair.relativeViewMinimums.size(), "> ", name, " = {", values, "};");
            floor = name + "[" + to_expression(*pair.imageIndex) + "]";
            inherit_expression_dependencies(id, *pair.imageIndex);
        } else {
            floor = QueryNumber(pair.relativeViewMinimums.front());
        }
        auto selected = "max(clamp(min(max(" + result + ".y, " + QueryNumber(pair.samplerMin) + "), " +
            QueryNumber(pair.samplerMax) + "), 0.0f, float(" + QueryImageExpression(words[2]) +
            ".get_num_mip_levels() - 1u)), " + floor + ")";
        if (pair.nearestMip) selected = "floor(" + selected + " + 0.5f)";
        statement(result, ".x = ", selected, ";");
        register_control_dependent_expression(id);
        minimumLodEmittedOperations.insert(id);
    }

    std::string to_texture_op(const spirv_cross::Instruction& instruction, bool sparse, bool* forward,
                             spirv_cross::SmallVector<std::uint32_t>& inherited) override {
        const auto op = static_cast<spv::Op>(instruction.op);
        if (op != spv::OpImageSampleImplicitLod && op != spv::OpImageSampleExplicitLod &&
            op != spv::OpImageSampleDrefImplicitLod && op != spv::OpImageSampleDrefExplicitLod)
            return CompilerMSL::to_texture_op(instruction, sparse, forward, inherited);
        const auto* words = stream(instruction);
        const auto found = minimumLodPairs.find(words[2]);
        if (found == minimumLodPairs.end()) return CompilerMSL::to_texture_op(instruction, sparse, forward, inherited);
        minimumLodChoice = true;
        auto minExpression = CompilerMSL::to_texture_op(instruction, sparse, forward, inherited);
        const auto selector = minimumLodSelector;
        minimumLodChoice = false;
        auto magExpression = CompilerMSL::to_texture_op(instruction, sparse, forward, inherited);
        minimumLodChoice.reset();
        const auto ids = ir.increase_bound_by(2);
        emit_op(words[0], ids, minExpression, false);
        emit_op(words[0], ids + 1, magExpression, false);
        for (const auto dependency : inherited) {
            inherit_expression_dependencies(ids, dependency);
            inherit_expression_dependencies(ids + 1, dependency);
        }
        inherited.push_back(ids);
        inherited.push_back(ids + 1);
        *forward = false;
        minimumLodEmittedOperations.insert(words[1]);
        return "(" + selector + " ? " + to_expression(ids) + " : " + to_expression(ids + 1) + ")";
    }

    TextureFunctionArguments MinimumLodArguments(const TextureFunctionArguments& args) {
        if (!minimumLodChoice) return args;
        const auto found = minimumLodPairs.find(args.base.img);
        if (found == minimumLodPairs.end()) return args;
        const auto& pair = shadowPairs[found->second.firstPair];
        auto result = args;
        std::string lambdaPrime;
        if (args.grad_x || args.grad_y) {
            if (!args.grad_x || !args.grad_y) Fail("minimum LOD sampling requires both original gradients");
            const auto image = QueryImageExpression(args.base.img);
            std::string dimensions;
            switch (args.base.imgtype->image.dim) {
                case spv::Dim1D: dimensions = "float(" + image + ".get_width())"; break;
                case spv::Dim2D:
                    dimensions = "float2(" + image + ".get_width(), " + image + ".get_height())"; break;
                case spv::Dim3D:
                    dimensions = "float3(" + image + ".get_width(), " + image + ".get_height(), " + image + ".get_depth())"; break;
                default: Fail("minimum LOD gradient has an unsupported original geometry");
            }
            const auto gradientLength = [&](std::uint32_t gradient) {
                const auto scaled = "(" + to_unpacked_expression(gradient) + ") * " + dimensions;
                return args.base.imgtype->image.dim == spv::Dim1D ? "abs(" + scaled + ")" : "length(" + scaled + ")";
            };
            const auto major = "max(" + gradientLength(args.grad_x) + ", " + gradientLength(args.grad_y) + ")";
            if (pair.samplerAnisotropy > 1.0f) {
                const auto minor = "min(" + gradientLength(args.grad_x) + ", " + gradientLength(args.grad_y) + ")";
                const auto cap = QueryNumber(std::min(pair.samplerAnisotropy, 16.0f));
                const auto eta = "((" + major + ") == 0.0f ? 1.0f : ((" + minor + ") == 0.0f ? " + cap +
                    " : min((" + major + ") / (" + minor + "), " + cap + ")))";
                lambdaPrime = "log2((" + major + ") / (" + eta + ")) + " + QueryNumber(pair.samplerBias);
            } else {
                lambdaPrime = "log2(" + major + ") + " + QueryNumber(pair.samplerBias);
            }
        } else if (args.lod) {
            lambdaPrime = "(" + to_expression(args.lod) + ") + " + QueryNumber(pair.samplerBias);
        } else {
            if (get_execution_model() != spv::ExecutionModelFragment)
                Fail("implicit LOD sampling requires the original fragment stage");
            lambdaPrime = QuerySamplerLodExpression(args.base.img, args.coord);
            if (args.bias)
                lambdaPrime = "(" + lambdaPrime + ") - " + QueryNumber(pair.samplerBias) + " + clamp(" +
                    QueryNumber(pair.samplerBias) + " + (" + to_expression(args.bias) + "), -16.0f, 16.0f)";
        }
        auto minimum = QueryNumber(pair.samplerMin);
        if (args.min_lod) minimum = "max(" + minimum + ", " + to_expression(args.min_lod) + ")";
        const auto bounded = "min(max((" + lambdaPrime + "), " + minimum + "), " + QueryNumber(pair.samplerMax) + ")";
        minimumLodSelector = "(" + bounded + " > 0.0f)";
        if ((args.grad_x || args.grad_y) && pair.samplerAnisotropy <= 1.0f) {
            const auto ids = ir.increase_bound_by(2);
            auto& scalar = set<spirv_cross::SPIRType>(ids, spirv_cross::SPIRType{spv::OpTypeFloat});
            scalar.basetype = spirv_cross::SPIRType::Float;
            scalar.width = 32;
            scalar.vecsize = 1;
            scalar.columns = 1;
            emit_op(ids, ids + 1, "(" + bounded + ") - " + QueryNumber(pair.samplerBias), false);
            inherit_expression_dependencies(ids + 1, args.grad_x);
            inherit_expression_dependencies(ids + 1, args.grad_y);
            if (args.min_lod) inherit_expression_dependencies(ids + 1, args.min_lod);
            result.lod = ids + 1;
            result.grad_x = result.grad_y = result.min_lod = 0;
        }
        const auto& original = get<spirv_cross::SPIRCombinedImageSampler>(args.base.img);
        const auto sampledType = original.combined_type;
        const auto image = original.image;
        const auto ids = ir.increase_bound_by(2);
        const auto& selection = found->second;
        const auto element = selection.imageIndex ?
            "((" + std::to_string(selection.firstPair) + "u + " + to_expression(*selection.imageIndex) +
                ") * 2u + " + (*minimumLodChoice ? "1u" : "0u") + ")" :
            std::to_string(selection.firstPair * 2u + (*minimumLodChoice ? 1u : 0u)) + "u";
        emit_op(minimumLodSamplerType, ids, to_expression(minimumLodBank) + "[" + element + "]", true);
        if (selection.imageIndex) inherit_expression_dependencies(ids, *selection.imageIndex);
        set<spirv_cross::SPIRCombinedImageSampler>(ids + 1, sampledType, image, ids);
        result.base.img = ids + 1;
        return result;
    }

    std::string to_function_args(const TextureFunctionArguments& originalArgs, bool* forward) override {
        const auto args = MinimumLodArguments(originalArgs);
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

    void EmitNonInlineFunctionPrototype(spirv_cross::SPIRFunction& func) {
        using namespace spirv_cross;
        using namespace spv;
        if (func.self != ir.default_entry_point)
            add_function_overload(func);
        local_variable_names = resource_names;
        std::string decl;
        processing_entry_point = false;
        if (!processing_entry_point)
            statement("static __attribute__((noinline))");
        auto &type = get<SPIRType>(func.return_type);
        if (!type.array.empty() && msl_options.force_native_arrays)
        {
            decl += "void";
        }
        else
        {
            decl += func_type_decl(type);
        }
        decl += " ";
        decl += to_name(func.self);
        decl += "(";
        if (!type.array.empty() && msl_options.force_native_arrays)
        {
            decl += "thread ";
            decl += type_to_glsl(type);
            decl += " (&spvReturnValue)";
            decl += type_to_array_glsl(type, 0);
            if (!func.arguments.empty())
                decl += ", ";
        }
        for (auto &arg : func.arguments)
        {
            uint32_t name_id = arg.id;
            auto *var = maybe_get<SPIRVariable>(arg.id);
            if (var)
            {
                if (arg.alias_global_variable && var->basevariable)
                    name_id = var->basevariable;
                var->parameter = &arg;
            }
            add_local_variable_name(name_id);
            decl += argument_decl(arg);
            bool is_dynamic_img_sampler = has_extended_decoration(arg.id, SPIRVCrossDecorationDynamicImageSampler);
            auto &arg_type = get<SPIRType>(arg.type);
            if (arg_type.basetype == SPIRType::SampledImage && !is_dynamic_img_sampler)
            {
                uint32_t planes = 1;
                if (auto *constexpr_sampler = find_constexpr_sampler(name_id))
                    if (constexpr_sampler->ycbcr_conversion_enable)
                        planes = constexpr_sampler->planes;
                for (uint32_t i = 1; i < planes; i++)
                    decl += join(", ", argument_decl(arg), plane_name_suffix, i);
                if (arg_type.image.dim != DimBuffer)
                {
                    if (arg_type.array.empty() || (var ? is_var_runtime_size_array(*var) : is_runtime_size_array(arg_type)))
                    {
                        decl += join(", ", sampler_type(arg_type, arg.id, false), " ", to_sampler_expression(name_id));
                    }
                    else
                    {
                        const char *sampler_address_space =
                                descriptor_address_space(name_id,
                                                         StorageClassUniformConstant,
                                                         "thread const");
                        decl += join(", ", sampler_address_space, " ", sampler_type(arg_type, name_id, false), "& ",
                                     to_sampler_expression(name_id));
                    }
                }
            }
            if (msl_options.swizzle_texture_samples && has_sampled_images && is_sampled_image_type(arg_type) &&
                !is_dynamic_img_sampler)
            {
                bool arg_is_array = !arg_type.array.empty();
                decl += join(", constant uint", arg_is_array ? "* " : "& ", to_swizzle_expression(name_id));
            }
            if (buffer_requires_array_length(name_id))
            {
                bool arg_is_array = !arg_type.array.empty();
                decl += join(", constant uint", arg_is_array ? "* " : "& ", to_buffer_size_expression(name_id));
            }
            if (&arg != &func.arguments.back())
                decl += ", ";
        }
        decl += ")";
        statement(decl);
    }

    void emit_function_prototype(spirv_cross::SPIRFunction& function,
                                 const spirv_cross::Bitset& flags) override {
        if (function.self != ir.default_entry_point && (function.function_control & spv::FunctionControlDontInlineMask)) {
            EmitNonInlineFunctionPrototype(function);
            return;
        }
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
    struct MinimumLodImageUse {
        std::unordered_set<std::uint32_t> required;
        bool blocked = false;
    };
    std::map<std::array<std::uint32_t, 3>, MinimumLodImageUse> minimumLodImageUses;
    std::unordered_set<std::uint32_t> minimumLodEmittedOperations;
    bool minimumLodCertificateUnresolved = false;
    bool requiresTextureLodQueries = false;
    std::unordered_map<std::uint32_t, QueryPair> queryPairs;
    std::vector<ShadowPair> shadowPairs;
    std::unordered_map<std::uint32_t, ShadowSelection> minimumLodPairs;
    std::uint32_t minimumLodSamplerType = 0, minimumLodBank = 0;
    std::optional<bool> minimumLodChoice;
    std::string minimumLodSelector;
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
    compiler.PrepareMinimumLodCertificate(guest);
    compiler.PrepareMinimumLod(guest);
    const bool samplerArgumentBuffer = target.samplerArgumentBuffer || !compiler.MinimumLodPairs().empty();
    if (samplerArgumentBuffer && !target.supportsArgumentBuffersTier2)
        Fail("minimum LOD sampler bank requires native Tier2 support");
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
    options.argument_buffers = samplerArgumentBuffer;
    if (samplerArgumentBuffer) {
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
    const bool useSamplerBank = samplerArgumentBuffer && !resources.separate_samplers.empty();
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
    if (!compiler.MinimumLodPairs().empty() && !result.samplerArgumentBuffer) Fail("minimum LOD shadow samplers require native argument bank");
    compiler.InstallMinimumLodBank(sampler, execution);
    result.samplerArgumentCount += static_cast<std::uint32_t>(compiler.MinimumLodPairs().size()) * 2u;
    for (const auto& pair : compiler.MinimumLodPairs()) result.minimumLodPairs.push_back({pair.imageSet, pair.imageBinding, pair.imageElement, pair.samplerSet, pair.samplerBinding, pair.samplerElement, pair.magArgument, pair.minArgument, pair.relativeViewMin});
    try { result.source = compiler.compile(); }
    catch (const std::exception& error) { Fail(error.what()); }
    result.requiresTextureLodQueries = compiler.RequiresTextureLodQueries();
    result.minimumLodImages = compiler.MinimumLodImages();
    result.capturedSamplerRequirements = compiler.CapturedSamplers();
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
