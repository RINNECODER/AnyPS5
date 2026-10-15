#if defined(ANYPS5_METAL_BACKEND)
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#if defined(ANYPS5_ROSETTA_DIRECT)
#include "prx/libSceAgcDriver/Execution/Metal/DirectDriver.hpp"
#include <stdexcept>
#endif
#else
#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#endif
#include "prx/libc/include/Shutdown.hpp"
#include <condition_variable>
#include <mutex>

namespace {
#if defined(ANYPS5_METAL_BACKEND) && defined(ANYPS5_ROSETTA_DIRECT)
// Rosetta direct path: the driver configures itself on first use and borrows guest memory as identity ranges.
struct BackendDriver {
    static AgcDriver::Metal::MetalDriver& Get() { return AgcDriver::Metal::DirectDriver(); }
};
#elif defined(ANYPS5_METAL_BACKEND)
using BackendDriver = AgcDriver::Metal::MetalDriver;
#else
using BackendDriver = AgcDriver::DriverDetail::Driver;
#endif

#if defined(ANYPS5_METAL_BACKEND)
// Parks a guest thread whose driver stopped. The Rosetta library links libc and parks in
// LibcAwaitExit; the guest-replay and host-fixture builds have no libc (no process exit to await),
// so their threads block for good.
[[noreturn]] void ParkGuestThread() {
#if defined(ANYPS5_ROSETTA_DIRECT)
    LibcAwaitExit_nid_postfix();
#else
    std::mutex mutex;
    std::condition_variable never;
    std::unique_lock lock(mutex);
    for (;;) never.wait(lock);
#endif
}
#endif

template <typename Operation>
void InvokeEntry(Operation&& operation) {
#if defined(ANYPS5_METAL_BACKEND)
    try {
        operation();
    } catch (const AgcDriver::Metal::DriverStopped&) {
        // The driver stopped under this guest thread: park it rather than unwind through guest frames.
        ParkGuestThread();
    } catch (const ProcessShutdown&) {
        ParkGuestThread();
    }
#else
    try {
        operation();
    } catch (const ProcessShutdown&) {
        LibcAwaitExit_nid_postfix();
    }
#endif
}
}

namespace AgcDriver {

void Submit(const Packet* packet, std::uint32_t queue, [[maybe_unused]] const char* call) {
#if defined(ANYPS5_METAL_BACKEND) && defined(ANYPS5_ROSETTA_DIRECT)
    // Descriptors and host-built command words (VideoOut's flip packet) live outside guest memory.
    if (packet == nullptr) throw std::invalid_argument("AGC driver: null submission packet");
    InvokeEntry([&] { AgcDriver::Metal::DirectSubmit(*packet, queue, call); });
#else
    InvokeEntry([&] { BackendDriver::Get().Submit(packet, queue); });
#endif
}

void WaitIdle() {
    BackendDriver::Get().WaitIdle();
}

void Shutdown() {
    BackendDriver::Get().Shutdown();
}

void RegisterShader(const Shader* shader) {
    BackendDriver::Get().RegisterShader(shader);
}

void SuspendPoint() {
    BackendDriver::Get().SuspendPoint();
}

void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    InvokeEntry([&] { BackendDriver::Get().RegisterVideoOutput(handle, output); });
}

void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    BackendDriver::Get().UnregisterVideoOutput(handle, output);
}

void AttachWindow(const PresentationWindow& window) {
#if defined(ANYPS5_METAL_BACKEND)
    // The Metal backend binds the window's layer on the first present.
    static_cast<void>(window);
#else
    DriverDetail::Driver::Get().AttachWindow(window);
#endif
}

void PresentClear(const PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    InvokeEntry([&] { BackendDriver::Get().Present(window, nullptr, opaque, gpuReady, context); });
}

void PresentBuffer(const PresentationWindow& window, const DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    InvokeEntry([&] { BackendDriver::Get().Present(window, &buffer, true, gpuReady, context); });
}

void ReleaseWindow(void* window) {
    BackendDriver::Get().ReleaseWindow(window);
}

void ReportFailure(std::exception_ptr error) {
    BackendDriver::Get().ReportFailure(error);
}

}

extern "C" void AgcDriverRaiseWorkerThreadPriority_nid_postfix(const char* role) {
#if defined(ANYPS5_METAL_BACKEND)
    // macOS has no RLIMIT_RTTIME or SCHED_RESET_ON_FORK path; the Linux priority policy does not apply.
    static_cast<void>(role);
#else
    AgcDriver::RaiseWorkerThreadPriority(role);
#endif
}

extern "C" void AgcDriverWaitIdle_nid_postfix() {
    InvokeEntry([&] { AgcDriver::WaitIdle(); });
}

static std::mutex& VulkanLoaderMutex() {
    static std::mutex mutex;
    return mutex;
}

extern "C" void AgcDriverLockVulkanLoader_nid_postfix() {
    VulkanLoaderMutex().lock();
}

extern "C" void AgcDriverUnlockVulkanLoader_nid_postfix() {
    VulkanLoaderMutex().unlock();
}

extern "C" void AgcDriverShutdown_nid_postfix() {
    AgcDriver::Shutdown();
}

extern "C" void AgcDriverRegisterShader_nid_postfix(const Shader* shader) {
    InvokeEntry([&] { AgcDriver::RegisterShader(shader); });
}

extern "C" void AgcDriverSuspendPoint_nid_postfix() {
    InvokeEntry([&] { AgcDriver::SuspendPoint(); });
}

extern "C" std::uint64_t AgcDriverDispatchesAdoptedAhead_nid_postfix() {
    return AgcDriver::DriverDetail::Driver::Get().DispatchesAdoptedAhead();
}

extern "C" void AgcDriverRegisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::RegisterVideoOutput(handle, output);
}

extern "C" void AgcDriverUnregisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::UnregisterVideoOutput(handle, output);
}

extern "C" void AgcDriverAttachWindow_nid_postfix(const AgcDriver::PresentationWindow& window) {
    AgcDriver::AttachWindow(window);
}

extern "C" void AgcDriverPresentClear_nid_postfix(const AgcDriver::PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentClear(window, opaque, gpuReady, context);
}

extern "C" void AgcDriverPresentBuffer_nid_postfix(const AgcDriver::PresentationWindow& window, const AgcDriver::DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentBuffer(window, buffer, gpuReady, context);
}

extern "C" void AgcDriverReleaseWindow_nid_postfix(void* window) {
    AgcDriver::ReleaseWindow(window);
}

extern "C" void AgcDriverReportFailure_nid_postfix(std::exception_ptr error) {
    AgcDriver::ReportFailure(error);
}

// Ahead-of-time shader artifact preparation. The Vulkan driver resolves and
// caches prepared pipelines here; the Metal backend compiles its pipelines when
// a draw or dispatch first uses them, so the exports only need to exist there.
extern "C" void AgcDriverResolveShaderAbi_nid_postfix(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
#if defined(ANYPS5_METAL_BACKEND)
    static_cast<void>(shader);
    static_cast<void>(context);
    static_cast<void>(primitive);
#else
    try {
        AgcDriver::DriverDetail::Driver::Get().ResolveShaderAbi(shader, context, primitive);
    } catch (const ProcessShutdown&) {
        LibcAwaitExit_nid_postfix();
    }
#endif
}

extern "C" void AgcDriverResolveGraphicsAbi_nid_postfix(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) {
#if defined(ANYPS5_METAL_BACKEND)
    static_cast<void>(vertex);
    static_cast<void>(pixel);
    static_cast<void>(primitiveType);
#else
    try {
        AgcDriver::DriverDetail::Driver::Get().ResolveGraphicsAbi(vertex, pixel, primitiveType);
    } catch (const ProcessShutdown&) {
        LibcAwaitExit_nid_postfix();
    }
#endif
}

extern "C" void AgcDriverResolveGraphicsStagesAbi_nid_postfix(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
#if defined(ANYPS5_METAL_BACKEND)
    static_cast<void>(stages);
    static_cast<void>(context);
    static_cast<void>(primitive);
#else
    try {
        AgcDriver::DriverDetail::Driver::Get().ResolveGraphicsStagesAbi(stages, context, primitive);
    } catch (const ProcessShutdown&) {
        LibcAwaitExit_nid_postfix();
    }
#endif
}
