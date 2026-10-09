#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"

namespace AgcDriver::DriverDetail {

void DecodeGraphicsPrograms(DrawDecode& decoded, const QueueState& queue, const ShaderRegistry& registry, bool staticAbi, bool includeFragment) {
    // The program walk lives in the backend-neutral DecodeDrawPrograms (DrawDispatch.cpp) so the Vulkan
    // driver, registration-time preparation and the Metal backend decode draws identically.
    std::vector<DrawDispatchProgram> programs;
    DecodeDrawPrograms(decoded.state, queue, registry, NullPixelProgramAddress(), staticAbi, includeFragment, programs, decoded.roles);
    decoded.programs.reserve(decoded.programs.size() + programs.size());
    for (auto& program : programs) {
        decoded.programs.push_back({program.binary, program.userDataBase, program.firstUserSgpr,
            std::move(program.userData), program.memory, std::move(program.snapshot), program.codeOffset});
    }
}

std::shared_ptr<DrawDecode> Driver::decodeDraw(const QueueState& queue, const Submission& submission) {
    auto product = std::make_shared<DrawDecode>();
    product->state = Graphics::DecodeState(queue);
    DecodeGraphicsPrograms(*product, queue, *submission.shaders, false, true);
    product->pixel = Graphics::DecodePixelStageInfo(queue.context, Graphics::ExportMappings(product->state), Graphics::PixelProgramSkipped(queue));
    return product;
}

void Driver::resolveDrawDecode(const QueueState& queue, const Submission& submission, std::shared_ptr<const DrawDecode>& decode, bool registerKey, std::uint64_t drawKey, bool profile) {
    if (decode == nullptr || verifyDrawRecipe()) {
        std::vector<Graphics::RegisterRead> readLog;
        struct LogScope {
            explicit LogScope(std::vector<Graphics::RegisterRead>* log) { Graphics::RegisterReadLog() = log; }
            ~LogScope() { Graphics::RegisterReadLog() = nullptr; }
        } logScope(verifyDrawRecipe() && registerKey ? &readLog : nullptr);
        auto fresh = decodeDraw(queue, submission);
        if (verifyDrawRecipe() && registerKey) {
            std::uint64_t facadeMismatches = 0;
            for (const auto read : readLog) {
                if (Graphics::DrawKeyCovers(read)) continue;
                ++facadeMismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: the decoders read %s register 0x%x, which DrawKeyRegisters lacks\n", Graphics::RegisterBankName(read.bank), read.offset);
            }
            std::uint64_t decodeMismatches = 0;
            if (decode != nullptr && !sameDecode(*decode, *fresh)) {
                decodeMismatches = 1;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: the entry's decode differs from a fresh decode (key 0x%llx, target 0x%llx)\n", static_cast<unsigned long long>(drawKey), static_cast<unsigned long long>(fresh->state.color.address));
            }
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.facadeMismatches += facadeMismatches;
            if (decode != nullptr) ++drawEntryCounters.verifyDecodes;
            drawEntryCounters.verifyDecodeMismatches += decodeMismatches;
        }
        decode = std::move(fresh);
    } else if (profile) {
        std::lock_guard cacheLock(drawCacheMutex);
        ++drawEntryCounters.decodeSkipped;
    }
}

}
