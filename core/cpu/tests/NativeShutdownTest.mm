#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include <cpu/Cpu.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

// A process stop (wall limit, idle limit, window close or a signal) fires the
// VideoOut shutdown token while a Metal worker still owns VideoOut work. The
// worker then sees ProcessShutdown, which is not a std::exception. Native
// shutdown must treat it as the requested stop it is: return normally, publish
// no completion, and never let it reach std::terminate in anyps5_cpu_run.
// Modes: rendering-wait (worker parked in RenderingWait::Wait on buffer reuse)
// and gpu-ready (worker reaches FlipRequest::GpuReady after the stop).
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t Display = 0x400000000ULL;
constexpr std::uint64_t Commands = 0x500000000ULL;
constexpr std::size_t LabelOffset = 2048;
constexpr std::uint32_t Width = 64, Height = 64;
constexpr auto RW = Cpu::Permission::Read | Cpu::Permission::Write;

struct Page {
    std::unique_ptr<std::byte, decltype(&std::free)> bytes{nullptr, &std::free};
    Page() {
        void* allocation = nullptr;
        require(posix_memalign(&allocation, 65536, 65536) == 0, "Cannot allocate native guest backing");
        bytes.reset(static_cast<std::byte*>(allocation));
        std::memset(bytes.get(), 0, 65536);
    }
    std::span<std::byte> Bytes() { return {bytes.get(), 65536}; }
};

struct Owners { Page display, commands; };

// Holds the presenter inside an actual Present (after GpuReady, before the
// flip completes) so the buffer-reuse ticket stays pending.
struct HeldPresentation {
    AgcDriver::PresentationWindow inner{};
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false, entered = false, released = false;
};

void heldDrawableSize(void* context, std::uint32_t* width, std::uint32_t* height) {
    auto& state = *static_cast<HeldPresentation*>(context);
    {
        std::unique_lock lock(state.mutex);
        if (state.armed) {
            state.armed = false;
            state.entered = true;
            state.changed.notify_all();
            state.changed.wait_for(lock, std::chrono::seconds(20), [&] { return state.released; });
        }
    }
    state.inner.getDrawableSize(state.inner.context, width, height);
}

void* heldLayer(void* context) {
    auto& state = *static_cast<HeldPresentation*>(context);
    return state.inner.metalLayer(state.inner.context);
}

struct Completions { unsigned flips = 0; };
std::uint64_t clockTime(void*) { return 101; }
std::uint64_t clockCounter(void*) { return 202; }
void flipEvent(void* context, VideoOutConfig&, std::int64_t) { ++static_cast<Completions*>(context)->flips; }

bool isProcessShutdown(std::exception_ptr error) {
    if (!error) return false;
    try { std::rethrow_exception(error); }
    catch (const ProcessShutdown&) { return true; }
    catch (...) { return false; }
}

template<class Predicate>
void pumpUntil(AnyPS5::Host::NativeMetalSession& session, Predicate&& done, const char* reason) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, reason);
        session.Window().PumpMainThread(std::chrono::milliseconds(1));
    }
}

void pumpFor(AnyPS5::Host::NativeMetalSession& session, std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until)
        session.Window().PumpMainThread(std::chrono::milliseconds(1));
}

void run(const char* utility, const std::string& mode) {
    require(mode == "rendering-wait" || mode == "gpu-ready", "Unknown native shutdown mode");
    Cpu::Machine machine;
    auto owners = std::make_shared<Owners>();
    machine.MapBorrowed(Display, owners->display.Bytes(), RW);
    machine.MapBorrowed(Commands, owners->commands.Bytes(), RW);
    const std::array ranges{
        AgcDriver::NativeGuestMemory::BorrowedRange{Display, owners->display.Bytes(), false, 301},
        AgcDriver::NativeGuestMemory::BorrowedRange{Commands, owners->commands.Bytes(), true, 302}};
    AnyPS5::Host::NativeMetalSessionConfiguration configuration;
    configuration.window = {"Native shutdown " + mode, Width, Height};
    configuration.utilityMetallib = utility;
    configuration.initialRanges = ranges;
    configuration.initialRangeOwner = owners;
    configuration.initialGeneration = 23;
    auto session = AnyPS5::Host::NativeMetalSession::CreateMainThread(configuration);
    configuration.initialRangeOwner.reset();

    HeldPresentation held;
    held.inner = session->Window().Presentation(Width, Height);
    auto presentation = held.inner;
    presentation.context = &held;
    presentation.getDrawableSize = heldDrawableSize;
    presentation.metalLayer = heldLayer;
    Completions completions;
    const VideoOutCompletionCallbacks callbacks{&completions, clockTime, clockCounter, flipEvent};
    auto backend = std::make_unique<Cpu::SceNativeVideoOutBackend>(machine, presentation, callbacks,
        std::stop_token{}, Cpu::SceVideoOutMemoryConfiguration{ranges, owners, 23});
    // On an early failure, free a held presenter before the backend joins it.
    struct Release {
        HeldPresentation& state;
        ~Release() {
            { std::lock_guard lock(state.mutex); state.released = true; }
            state.changed.notify_all();
        }
    } release{held};
    const auto video = backend->GetCallbacks();
    const auto handle = video.Open(255, 0, 0, std::nullopt);
    require(handle > 0, "Native VideoOut did not open an output");
    const auto config = backend->GetConfig(handle);
    Cpu::SceVideoOutAttribute attribute;
    attribute.Width = Width;
    attribute.Height = Height;
    attribute.PixelFormat = 0x8000000000000000ULL;
    const std::array<Cpu::SceVideoOutBuffer, 1> buffers{{{Display, 0, {}}}};
    require(video.RegisterBuffers(handle, 0, 0, buffers, attribute, 0) == 0, "Native display registration failed");

    const auto h = static_cast<std::uint32_t>(handle);
    const std::array<std::uint32_t, 6> flip0{0xc004105cu, h, 0, 1, 0, 0};
    const std::array<std::uint32_t, 4> wait0{0xc0021018u, h, 0, 0};
    const auto label = Commands + LabelOffset;
    // WAIT_REG_MEM, memory space, "equal" compare, reference 1, full mask.
    const std::array<std::uint32_t, 7> waitLabel{0xc0053c00u, 0x13u, static_cast<std::uint32_t>(label),
        static_cast<std::uint32_t>(label >> 32u), 1, 0xffffffffu, 0};
    std::vector<std::uint32_t> stream;
    if (mode == "rendering-wait") {
        // wait0 captures flip0's reuse ticket; the held presenter keeps it pending.
        stream.insert(stream.end(), flip0.begin(), flip0.end());
        stream.insert(stream.end(), wait0.begin(), wait0.end());
    } else {
        // The worker polls the label, so the stop lands before GpuReady.
        stream.insert(stream.end(), waitLabel.begin(), waitLabel.end());
        stream.insert(stream.end(), flip0.begin(), flip0.end());
    }
    std::memcpy(owners->commands.Bytes().data(), stream.data(), stream.size() * sizeof(std::uint32_t));
    const auto pendingFlips = [&] {
        std::lock_guard lock(config->mutex);
        return config->flipStatus.flipPendingNum;
    };
    const auto stoppedByShutdown = [&] {
        std::lock_guard lock(config->mutex);
        return isProcessShutdown(config->failure);
    };

    if (mode == "rendering-wait") {
        { std::lock_guard lock(held.mutex); held.armed = true; }
        session->Driver().SubmitCommandBuffer(Commands, static_cast<std::uint32_t>(stream.size()), 0, 0);
        pumpUntil(*session, [&] { std::lock_guard lock(held.mutex); return held.entered; },
            "Flip did not reach the held actual presentation");
        // flip0 is presented but not complete, so wait0 cannot finish.
        pumpFor(*session, std::chrono::milliseconds(100));
        require(pendingFlips() == 1 && completions.flips == 0, "Held flip completed before the stop");
        backend->RequestStop();
        pumpUntil(*session, stoppedByShutdown, "Rendering wait did not observe the process stop");
        { std::lock_guard lock(held.mutex); held.released = true; }
        held.changed.notify_all();
    } else {
        session->Driver().SubmitCommandBuffer(Commands, static_cast<std::uint32_t>(stream.size()), 0, 0);
        pumpFor(*session, std::chrono::milliseconds(100));
        require(pendingFlips() == 1 && completions.flips == 0, "Label-gated flip ran before its label");
        backend->RequestStop();
        __atomic_store_n(reinterpret_cast<std::uint32_t*>(owners->commands.Bytes().data() + LabelOffset),
            1u, __ATOMIC_SEQ_CST);
        pumpUntil(*session, [&] { return pendingFlips() == 0; },
            "Flip readiness after the stop did not retire its reservation");
        require(stoppedByShutdown(), "Flip readiness after the stop did not observe ProcessShutdown");
    }

    // Same order as SceNativeGraphicsSession: close request, VideoOut, then driver/window.
    session->Window().RequestCloseMainThread();
    backend->Shutdown();
    backend.reset();
    try {
        session->ShutdownAfterCpuStoppedMainThread();
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("Requested native stop reported a failure: ") + error.what());
    } catch (...) {
        throw std::runtime_error("Non-standard exception escaped native shutdown; anyps5_cpu_run would std::terminate");
    }
    require(completions.flips == 0, "Stopped flip published a completion");
    std::weak_ptr<Owners> retained = owners;
    owners.reset();
    session.reset();
    require(retained.expired(), "Native shutdown retained guest backing");
    std::cout << "PASS native " << mode << " stop: ProcessShutdown handled as a requested stop; no completion, no terminate\n";
}

}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            require(argc == 3, "Usage: NativeShutdownTest utility.metallib rendering-wait|gpu-ready");
            run(argv[1], argv[2]);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            return 1;
        }
    }
}
