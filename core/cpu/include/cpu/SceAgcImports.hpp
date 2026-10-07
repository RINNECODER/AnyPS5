#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace Cpu {
struct SceImport;

// Each selection admits one exact source ABI contract. Empty selection is the
// production default: a source NID name alone does not qualify a target caller.
enum class AgcAbiContract {
    SubmitDcbPacket16, AgrSubmitDcbPacket16, SubmitAcbPacket16,
    SubmitMultiDcbs, AgrSubmitMultiDcbs, SubmitMultiAcbs,
    DcbWriteDataCommandBuffer56, AcbWriteDataCommandBuffer56,
    DcbSetShRegisterDirect, DcbSetCxRegisterDirect, DcbSetUcRegisterDirect,
    CbDispatchCommandBuffer56, CreateShaderRelativeHeader96, SuspendPoint,
    AddEqEvent, DeleteEqEvent, DcbSetFlipCommandBuffer56,
    DcbWaitUntilSafeForRenderingCommandBuffer56
};

// These policies constrain the admitted target use, independently of the source
// adapter enum used by synthetic callers. Selection requires the caller to have
// verified the exact owned image identity before invoking this API.
enum class AgcArgumentPolicy { SourceContract, TargetPacket20Bit, TargetMemoryWrite, TargetShader, TargetDispatch, TargetSuspend, TargetFlip, TargetRenderingWait, TargetGraphicsEvent };
struct AgcAbiAdmission {
    AgcAbiContract Contract;
    AgcArgumentPolicy Policy;
    std::string_view Evidence;
};
std::span<const AgcAbiAdmission> QualifiedAgcAdmissionsForImage(std::string_view verifiedSha256);

struct AgcReadableRange { std::uint64_t Address; std::size_t Size; };
struct SceAgcBackend {
    std::function<void(std::uint64_t, std::uint32_t, std::uint8_t, std::uint32_t)> Submit;
    // Validate ranges, publish staged guest header and capture it while native
    // mapping admission is held. Registration must not invoke CPU mapping APIs.
    std::function<void(std::uint64_t, std::span<const AgcReadableRange>, const std::function<void()>&,
                       const std::function<void()>&)> RegisterShader;
    std::function<void()> Suspend;
    // Equeue handles and user data remain numeric guest values. The CPU event
    // owner must supply these callbacks and connect Metal EOP delivery. Handle
    // pinning, graphics filter -14, EV_ADD|EV_CLEAR, count/reset and deletion
    // lifetime belong to that owner. A callback's int32 result is returned in
    // EAX; neither callback may interpret handle/user data as a host pointer.
    // Metal completion producers publish host mailbox receipts only; the
    // persistent guest scheduler owner performs event writes and wake delivery.
    std::function<std::int32_t(std::uint64_t, std::int32_t, std::uint64_t)> AddEvent;
    std::function<std::int32_t(std::uint64_t, std::int32_t)> DeleteEvent;
};

class SceAgcImports {
public:
    SceAgcImports(Machine& machine, SceAgcBackend backend,
                  std::span<const AgcAbiContract> admittedContracts = {},
                  std::uint64_t gateBase = 0x7ffdf3000000);
    SceAgcImports(Machine& machine, SceAgcBackend backend,
                  std::span<const AgcAbiAdmission> admissions,
                  std::uint64_t gateBase = 0x7ffdf3000000);
    ~SceAgcImports();
    SceAgcImports(const SceAgcImports&) = delete;
    SceAgcImports& operator=(const SceAgcImports&) = delete;
    // Only function type 2, size zero is accepted. Scope/NID admission precedes
    // allocation; unresolved target contracts cannot create a callable gate.
    std::uint64_t Resolve(const SceImport& import, std::uint8_t symbolType, std::uint64_t symbolSize);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
