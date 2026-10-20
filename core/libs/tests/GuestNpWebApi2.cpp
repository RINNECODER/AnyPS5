#include "SceTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

struct NpWebApi2MemoryPoolStats {
    std::size_t pool_size;
    std::size_t max_inuse_size;
    std::size_t current_inuse_size;
    std::int32_t reserved;
};

extern "C" {
int APS5_VABI sceNpWebApi2Initialize(int, std::size_t);
int APS5_VABI sceNpWebApi2Terminate(int);
int APS5_VABI sceNpWebApi2GetMemoryPoolStats(int, NpWebApi2MemoryPoolStats*);
int APS5_VABI sceNpWebApi2PushEventCreateHandle(int);
int APS5_VABI sceNpWebApi2PushEventAbortHandle(int, int);
int APS5_VABI sceNpWebApi2PushEventDeleteHandle(int, int);
int APS5_VABI sceNpWebApi2PushEventSetHandleTimeout(int, int, std::uint32_t);
int APS5_VABI sceNpWebApi2CreateUserContext(int, int);
int APS5_VABI sceNpWebApi2CreateRequest(int, const char*, const char*, const char*, const void*, std::int64_t*);
int APS5_VABI sceNpWebApi2SetRequestTimeout(std::int64_t, std::uint32_t);
int APS5_VABI sceNpWebApi2DeleteRequest(std::int64_t);
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    constexpr int invalidContext = static_cast<int>(0x80553404);
    constexpr int invalidHandle = static_cast<int>(0x8055340D);
    const int context = sceNpWebApi2Initialize(1, 0x20000);
    const int other = sceNpWebApi2Initialize(1, 0x8000);
    Require(context > 0 && other > 0 && context != other);
    NpWebApi2MemoryPoolStats stats{1, 1, 1, 1};
    Require(sceNpWebApi2GetMemoryPoolStats(context, &stats) == 0);
    Require(stats.pool_size == 0x20000 && stats.max_inuse_size == 0 && stats.current_inuse_size == 0 && stats.reserved == 0);
    Require(sceNpWebApi2GetMemoryPoolStats(other, &stats) == 0 && stats.pool_size == 0x8000);
    Require(sceNpWebApi2GetMemoryPoolStats(context, nullptr) == 0);
    Require(sceNpWebApi2GetMemoryPoolStats(-1, nullptr) == invalidContext);
    Require(sceNpWebApi2PushEventCreateHandle(-1) == invalidContext);
    const int handle = sceNpWebApi2PushEventCreateHandle(context);
    Require(handle > 0);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle + 1000) == invalidHandle);
    Require(sceNpWebApi2PushEventSetHandleTimeout(context, handle, 30000) == 0);
    Require(sceNpWebApi2PushEventAbortHandle(other, handle) == invalidHandle);
    Require(sceNpWebApi2PushEventDeleteHandle(other, handle) == invalidHandle);
    Require(sceNpWebApi2PushEventSetHandleTimeout(other, handle, 0) == invalidHandle);
    Require(sceNpWebApi2PushEventSetHandleTimeout(context, handle, 0) == 0);
    Require(sceNpWebApi2PushEventSetHandleTimeout(context, handle, UINT32_MAX) == 0);
    Require(sceNpWebApi2PushEventDeleteHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventDeleteHandle(context, handle) == invalidHandle);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == invalidHandle);
    Require(sceNpWebApi2PushEventSetHandleTimeout(context, handle, 1) == invalidHandle);
    const int survivingHandle = sceNpWebApi2PushEventCreateHandle(other);
    const int user = sceNpWebApi2CreateUserContext(context, 1);
    std::int64_t request = 0;
    Require(user > 0 && sceNpWebApi2CreateRequest(user, "sessionManager", "/v1/sessions", "GET", nullptr, &request) == 0);
    Require(sceNpWebApi2SetRequestTimeout(request, 30000000) == 0);
    Require(sceNpWebApi2DeleteRequest(request) == 0);
    Require(sceNpWebApi2Terminate(context) == 0);
    Require(sceNpWebApi2GetMemoryPoolStats(context, &stats) == invalidContext);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == invalidContext);
    Require(sceNpWebApi2PushEventSetHandleTimeout(context, handle, 30000) == invalidContext);
    Require(sceNpWebApi2PushEventAbortHandle(other, survivingHandle) == 0);
    Require(sceNpWebApi2Terminate(other) == 0);
    return 0;
}
