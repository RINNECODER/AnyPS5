#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERREGISTRY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERREGISTRY_HPP

#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct Shader;

namespace ShaderRecompiler {
class ShaderPreparationContext;
}

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::DriverDetail {

struct ShaderSnapshot;
using ShaderRegistry = std::map<std::uint64_t, std::vector<std::shared_ptr<const ShaderSnapshot>>>;

struct PreparedShaderState {
    struct Entry {
        std::size_t codeOffset;
        std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
    };
    struct Rectangle {
        std::uint64_t vertexId;
        std::uint64_t fragmentId;
        std::shared_ptr<const ShaderRecompiler::RectListShaders> shaders;
    };
    struct RectangleProgress {
        std::weak_ptr<const ShaderSnapshot> fragment;
        std::size_t vertexCount;
        std::size_t fragmentCount;
    };
    struct GraphicsAbi {
        std::vector<std::uint64_t> key;
        std::weak_ptr<const ShaderRegistry> registry;
        std::vector<std::weak_ptr<const ShaderSnapshot>> stages;
    };
    std::vector<Entry> entries;
    std::vector<std::vector<std::uint64_t>> registeredAbis;
    std::vector<GraphicsAbi> graphicsAbis;
    std::vector<Rectangle> rectangles;
    std::vector<RectangleProgress> rectangleProgress;
    std::vector<std::weak_ptr<const ShaderSnapshot>> fragments;
    bool rectangleRequested = false;
    bool deferred = false;
};
struct PreparedShaders : PreparedShaderState {
    std::mutex mutex;
    std::condition_variable settled;
    std::exception_ptr failure;
    bool pending = false;
};

struct RegisteredShaderState {
    Registers shader;
    Registers context;
    Registers userConfig;
};

struct ShaderSnapshot {
    std::uint64_t codeAddress;
    std::uint64_t headerAddress;
    std::uint8_t type;
    std::vector<std::uint32_t> code;
    std::vector<std::byte> header;
    std::shared_ptr<PreparedShaders> prepared = std::make_shared<PreparedShaders>();
    std::shared_ptr<const RegisteredShaderState> registeredState;
    std::shared_ptr<std::atomic<std::uint8_t>> workgroupUse = std::make_shared<std::atomic<std::uint8_t>>(0u);
    std::shared_ptr<std::atomic<bool>> rawGraphicsNoted = std::make_shared<std::atomic<bool>>(false);
};


std::shared_ptr<const ShaderSnapshot> ReadRawShader(std::uint64_t address);
std::shared_ptr<const ShaderSnapshot> ProgramSnapshot(const ShaderRegistry& shaders, std::uint64_t address, std::initializer_list<std::uint8_t> types);
void NoteRawGraphicsDraw(const ShaderSnapshot& snapshot);
bool RawGraphicsDrawn();
bool RawGraphicsDrawnIn(std::uint64_t begin, std::uint64_t end);
bool ProgramUsesWorkgroup(const ShaderSnapshot& snapshot, std::size_t codeOffset);

std::shared_ptr<const ShaderRecompiler::SourceHandle> PrepareShaderWithDiagnostics(const ShaderRecompiler::RecompileRequest& request, ShaderRecompiler::ShaderPreparationContext* preparation = nullptr);

std::uint64_t NullPixelProgramAddress();
ShaderSnapshot PrepareNullPixelProgram(const VulkanDevice& device);
std::optional<ShaderRecompiler::ShaderFloatMode> RegisteredFloatMode(const ShaderSnapshot& snapshot);
std::optional<ShaderRecompiler::ShaderFloatMode> DispatchFloatMode(const ShaderSnapshot& snapshot, const Registers& live);
std::shared_ptr<const ShaderSnapshot> RegisteredHeader(const ShaderRegistry& registry, const Shader* shader);
// Inline so the Metal driver, which does not link ShaderRegistry.cpp, resolves programs identically.
inline std::shared_ptr<const ShaderSnapshot> RegisteredProgram(const ShaderRegistry& registry, std::uint64_t address, std::initializer_list<std::uint8_t> types) {
    auto found = registry.upper_bound(address);
    if (found == registry.begin()) return {};
    --found;
    std::shared_ptr<const ShaderSnapshot> result;
    bool registered = false;
    for (const auto& snapshot : found->second) {
        if (address - snapshot->codeAddress >= snapshot->code.size() * sizeof(std::uint32_t)) continue;
        registered = true;
        if (std::find(types.begin(), types.end(), snapshot->type) == types.end()) continue;
        if (result != nullptr && result->type != snapshot->type) throw std::runtime_error("AGC driver: registered program has ambiguous shader binary types");
        result = snapshot;
    }
    if (registered && result == nullptr) throw std::runtime_error("AGC driver: registered program refers to an incompatible shader binary type");
    return result;
}
void PublishRegisteredShader(std::shared_ptr<ShaderRegistry>& registry, const std::shared_ptr<const ShaderSnapshot>& snapshot);
void SubmitRegistrationPreparation(std::function<void()> task);
void CloseRegistrationPreparation();

void ResolvePreparedGraphics(const ShaderSnapshot& front, const std::shared_ptr<const ShaderSnapshot>& fragment, std::uint32_t primitiveType, const ShaderRecompiler::SpirvTarget& target);

ShaderRecompiler::RectListShaders PreparedRectangle(const ShaderSnapshot& snapshot, std::uint64_t vertexId, std::uint64_t fragmentId);
ShaderRecompiler::RectListShaders DrawRectangle(const ShaderSnapshot& front, const std::shared_ptr<const ShaderSnapshot>& fragment, std::uint64_t vertexId, std::uint64_t fragmentId, const ShaderRecompiler::SpirvTarget& target);

std::shared_ptr<const ShaderRecompiler::SourceHandle> SourceHandleFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request);
ShaderRecompiler::PreparedShaderInvocation InvocationFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request);

}

#endif
