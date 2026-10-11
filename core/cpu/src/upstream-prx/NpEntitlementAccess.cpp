// Upstream libSceNpEntitlementAccess, compiled for the macOS runner, and the exports a
// guest may call through the bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNpEntitlementAccess/Export.cpp"
#include "Marshal.hpp"
#include <cstdlib>
#include <mach-o/dyld.h>
#include <system_error>

namespace Cpu::UpstreamPrx {

template <> struct GuestLayout<NpUnifiedEntitlementLabel> { static constexpr std::size_t Size = 20; };
template <> struct GuestLayout<NpEntitlementAccessAddcontEntitlementInfo> { static constexpr std::size_t Size = 28; };

namespace {

// Upstream finds the owned add-on list beside the executable through /proc/self/exe,
// which macOS lacks, so its lookup would throw on the first guest call. Upstream reads
// the list once and caches it, so before main (no other thread can touch the
// environment yet) load its cache from the same place: the list beside this executable
// when present, otherwise an empty list (no entitlements). The environment is restored
// afterwards, so child processes never inherit the default. An explicit
// ANYPS5_ENTITLEMENTS keeps upstream's meaning, including its error for an unreadable
// file; so does a malformed default list, whose error the first guest call reports.
const bool defaultEntitlements = [] {
    if (const char* configured = std::getenv("ANYPS5_ENTITLEMENTS"); configured && *configured) return false;
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string executable(size, '\0');
    if (_NSGetExecutablePath(executable.data(), &size) != 0) return false;
    executable.resize(std::strlen(executable.c_str()));
    std::error_code error;
    auto path = std::filesystem::weakly_canonical(executable, error);
    if (error) path = executable;
    const auto list = path.parent_path() / "anyps5-entitlements.ini";
    if (setenv("ANYPS5_ENTITLEMENTS", std::filesystem::exists(list, error) ? list.c_str() : "/dev/null", 1) != 0) return false;
    try { (void)OwnedAddons(); }
    catch (const std::exception&) { return false; }
    unsetenv("ANYPS5_ENTITLEMENTS");
    return true;
}();

// Not bridged: sceNpEntitlementAccessPoll/Request{Service,Unified}EntitlementInfo and
// sceNpEntitlementAccessGetPftFlag are NotImplemented upstream.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessAbortRequest),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessDeleteRequest),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessGenerateTransactionId, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessGetAddcontEntitlementInfo, I32, In, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessGetAddcontEntitlementInfoList, I32, OutArray<2>, I32, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessGetEntitlementKey, I32, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessGetSkuFlag, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessInitialize, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessPollConsumeEntitlement),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessPollServiceEntitlementInfoList),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessPollUnifiedEntitlementInfoList),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessRequestConsumeServiceEntitlement),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessRequestConsumeUnifiedEntitlement),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessRequestServiceEntitlementInfoList),
    ANYPS5_UPSTREAM_EXPORT(sceNpEntitlementAccessRequestUnifiedEntitlementInfoList),
};

}

Library NpEntitlementAccessExports() {
    (void)defaultEntitlements;
    return {"libSceNpEntitlementAccess", exports};
}

}
