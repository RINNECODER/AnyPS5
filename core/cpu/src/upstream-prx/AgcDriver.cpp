// Upstream libSceAgcDriver leaf exports (resource registration, capture, submit validation,
// equeue event queries), compiled for the macOS runner. None of these touches the upstream
// Vulkan driver; submission, queues, labels and flips stay on the Metal path in
// SceAgcImports. The graphics scope is owned by SceAgcImports, which resolves these rows
// through this descriptor table instead of the generic bridge. Checked against the upstream
// bodies: every pointer below is either an integer output, a flat input copied from the
// guest, or discarded by upstream.
#include "prx/libSceAgcDriver/Resource/src/Registration.cpp"
#include "prx/libSceAgcDriver/Resource/src/Lifecycle.cpp"
#include "prx/libSceAgcDriver/State/src/Status.cpp"
#include "prx/libSceAgcDriver/Eq/src/Query.cpp"
#include "prx/libSceAgcDriver/Unimplemented.cpp"
#include "Marshal.hpp"
#include <nid/NidCompute.hpp>
#include <map>

namespace Cpu::UpstreamPrx {

// ident64, filter16, flags16, fflags32, data64, udata64 on both ABIs. udata is copied as a
// number; the upstream queries only read ident, filter and data.
template <> struct GuestLayout<KernelEvent> { static constexpr std::size_t Size = 32; };
static_assert(sizeof(KernelEvent) == 32 && offsetof(KernelEvent, data) == 16);

namespace {

// Registration takes seven arguments: the user data word is the caller's first stack slot.
// Upstream reports registration as unavailable; the handle keeps Out semantics, the name is
// discarded and the memory or GDS range is passed through as numbers.
template <auto Function, class Third, class Fourth>
void RegisterWithUserData(Machine& guest, std::string_view name) {
    using Parameters = typename Detail::Signature<decltype(Function)>::Parameters;
    static_assert(std::tuple_size_v<Parameters> == 7);
    Detail::Raw raw{};
    for (std::size_t index = 0; index < raw.size(); ++index) raw[index] = guest.Get(Detail::ArgumentRegisters[index]);
    Detail::Slot<Out, Parameters, 0> handle;
    Detail::Slot<I32, Parameters, 1> owner;
    Detail::Slot<Third, Parameters, 2> third;
    Detail::Slot<Fourth, Parameters, 3> fourth;
    Detail::Slot<Ignored, Parameters, 4> label;
    Detail::Slot<I32, Parameters, 5> type;
    handle.Load(guest, raw, name); owner.Load(guest, raw, name); third.Load(guest, raw, name);
    fourth.Load(guest, raw, name); label.Load(guest, raw, name); type.Load(guest, raw, name);
    std::uint64_t userData = 0;
    const auto stack = guest.Get(Register::Rsp);
    if (stack > std::numeric_limits<std::uint64_t>::max() - 16) Detail::Fault(name, 6, "overflows the guest address space");
    Detail::CopyIn(guest, stack + 8, &userData, sizeof(userData), Permission::Read, name, 6);
    std::int64_t result = 0;
    try {
        result = Function(handle.Get(), owner.Get(), third.Get(), fourth.Get(), label.Get(), type.Get(), userData);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(name) + ": " + error.what());
    }
    handle.Store(guest);
    guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
}

constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetEqEventType, In),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetEqContextId, In),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverIsCaptureInProgress),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverIsTraceInProgress),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverIsSubmitValidationEnabled),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverRequestCaptureStart, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverRequestCaptureStop),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverTriggerCapture),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetShaderDebuggingStatus),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverSetSubmitValidationMode, I32),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetSubmitValidationMode, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverSetSubmitValidationConfig, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetSubmitValidationConfig, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverSetValidationErrorOutputFrequency, I32),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverRegisterWorkloadStream, I32, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverUnregisterWorkloadStream, I32),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverInitResourceRegistration),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverQueryResourceRegistrationUserMemoryRequirements, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverRegisterOwner, Out, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverRegisterDefaultOwner, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetDefaultOwner, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverUnregisterOwnerAndResources, I32),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverUnregisterResource, I32),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverUnregisterAllResourcesForOwner, I32),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetResourceRegistrationMaxNameLength, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetOwnerName, I32, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetResourceName, I32, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetResourceType, I32, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetResourceUserData, I32, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverSetResourceUserData, I32, I64),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetResourceBaseAddressAndSizeInBytes, I32, Ignored, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverGetResourceShaderGuid, I32, Ignored),
    ANYPS5_UPSTREAM_EXPORT(sceAgcDriverFindResourcesPublic, Ignored, Ignored),
    Export{"sceAgcDriverRegisterResource", &RegisterWithUserData<&::sceAgcDriverRegisterResource, Opaque, I64>},
    Export{"sceAgcDriverRegisterGdsResource", &RegisterWithUserData<&::sceAgcDriverRegisterGdsResource, I32, I32>},
};

}

const Export* AgcDriverLeafExport(std::string_view nid) {
    static const auto byNid = [] {
        std::map<std::string, const Export*, std::less<>> table;
        for (const auto& entry : exports)
            if (!table.emplace(Nid::ComputeNid(std::string(entry.Name), ""), &entry).second)
                throw std::logic_error("Duplicate AGC driver leaf NID for " + std::string(entry.Name));
        return table;
    }();
    const auto found = byNid.find(nid);
    return found == byNid.end() ? nullptr : found->second;
}

}
