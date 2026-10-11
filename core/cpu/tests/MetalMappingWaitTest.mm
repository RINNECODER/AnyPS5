#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// #293: a guest mapping change must not wait for the whole GPU to go idle.
// (a) A queue parked in WAIT_REG_MEM on a label the CPU writes later must not
//     deadlock a mapping change on the CPU thread, and the parked work must
//     see the new mapping when it resumes.
// (b) A change that leaves the GPU-visible set unchanged must not wait for
//     outstanding GPU work at all.
// (c) A change that alters a GPU-visible range still lets accepted, running
//     work finish first, and later work sees the new backing.
// Every wait is bounded: a watchdog unblocks the GPU so a regression fails
// with a message instead of hanging.
namespace {
using AgcDriver::NativeGuestMemory::BorrowedRange;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t Commands = 0x500000;
constexpr std::uint64_t Labels = 0x510000;
constexpr std::uint64_t Target = 0x520000;
constexpr std::uint64_t Page = 4096;

struct alignas(Page) Backing {
    std::array<std::byte, Page> bytes{};
    std::span<std::byte> Span() { return bytes; }
    std::uint32_t Word(std::size_t offset) const {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    void Store(std::size_t offset, std::uint32_t value) {
        std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(bytes.data() + offset))
            .store(value, std::memory_order_release);
    }
};

std::uint32_t Low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }
std::uint32_t High(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32u); }

void WaitLabel(std::vector<std::uint32_t>& words, std::uint64_t address, std::uint32_t value) {
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(address), High(address), value, 0xffffffffu, 0x19u});
}
void WriteData(std::vector<std::uint32_t>& words, std::uint64_t address, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(address), High(address), value});
}
void EopInterrupt(std::vector<std::uint32_t>& words, std::uint64_t address, std::uint32_t value) {
    words.insert(words.end(), {0xc0064900u, 0u, (1u << 29u) | (1u << 24u), Low(address), High(address), value, 0u, 0u});
}

struct Fixture {
    Backing commands, labels, target, replacement, spare;
    std::uint64_t generation = 1;
    std::mutex mutex;
    std::condition_variable changed;
    bool holdEop = false;
    bool eopEntered = false;
    bool eopReleased = false;
    bool eopReturned = false;
    AgcDriver::Metal::MetalDriver driver;

    std::vector<BorrowedRange> Ranges(Backing& targetBacking) {
        return {{Commands, commands.Span(), true}, {Labels, labels.Span(), true},
            {Target, targetBacking.Span(), true, reinterpret_cast<std::uintptr_t>(&targetBacking)}};
    }

    void Configure(id<MTLDevice> device, id<MTLLibrary> library) {
        const auto ranges = Ranges(target);
        driver.Configure((__bridge void*)device, (__bridge void*)library, ranges, [this](std::uint32_t queue) {
            require(queue == 0, "EOP interrupt arrived on an unexpected queue");
            std::unique_lock lock(mutex);
            if (!holdEop) return;
            eopEntered = true;
            changed.notify_all();
            // Held with the worker inside the packet: the GPU is busy, not parked.
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return eopReleased; });
            eopReturned = true;
            changed.notify_all();
        }, generation);
    }

    void Submit(const std::vector<std::uint32_t>& words) {
        std::memcpy(commands.bytes.data(), words.data(), words.size() * sizeof(std::uint32_t));
        driver.SubmitCommandBuffer(Commands, static_cast<std::uint32_t>(words.size()), 0, 0);
    }

    // Returns the seconds the mapping call blocked.
    double Mutate(Backing& targetBacking, const std::function<void()>& onCpu) {
        const auto ranges = Ranges(targetBacking);
        const auto start = Clock::now();
        driver.MutateBorrowedRanges(ranges, ++generation, onCpu, std::make_shared<int>(0), std::make_shared<int>(1));
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    void ReleaseEop() {
        std::lock_guard lock(mutex);
        eopReleased = true;
        changed.notify_all();
    }

    void WaitEopEntered() {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return eopEntered; }),
            "GPU work never reached the held EOP interrupt");
    }

    // Bounded drain: a watchdog writes the label so a hang becomes a failure.
    void DrainWithWatchdog(std::uint32_t label, const char* what) {
        std::atomic<bool> done{false}, fired{false};
        std::thread watchdog([&] {
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!done && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!done) { fired = true; labels.Store(0, label); ReleaseEop(); }
        });
        driver.WaitIdle();
        done = true;
        watchdog.join();
        require(!fired, what);
    }
};

// (a) WAIT_REG_MEM on a CPU-written label + overlapping mapping change.
void parkedWaitDoesNotDeadlock(id<MTLDevice> device, id<MTLLibrary> library) {
    Fixture f;
    f.Configure(device, library);
    std::vector<std::uint32_t> words;
    WaitLabel(words, Labels, 1);
    WriteData(words, Target + 64, 0x293a);
    f.Submit(words);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(f.target.Word(64) == 0 && f.replacement.Word(64) == 0, "GPU passed WAIT_REG_MEM before the CPU wrote the label");

    std::atomic<bool> mapped{false}, watchdogFired{false};
    std::thread watchdog([&] {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!mapped && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!mapped) { watchdogFired = true; f.labels.Store(0, 1); }
    });
    bool cpuRan = false;
    const auto blocked = f.Mutate(f.replacement, [&] { cpuRan = true; });
    mapped = true;
    watchdog.join();
    require(!watchdogFired,
        "mapping change blocked on GPU work parked in WAIT_REG_MEM for a label only the CPU can write (deadlock)");
    require(cpuRan, "mapping change did not run its CPU mutation");
    std::cerr << "[mapping-wait] parked WAIT_REG_MEM: mapping change returned in " << blocked << " s\n";

    // The CPU now satisfies the wait; the resumed packet must use the new backing.
    f.labels.Store(0, 1);
    f.DrainWithWatchdog(1, "GPU did not finish after the CPU wrote the awaited label");
    require(f.replacement.Word(64) == 0x293a, "resumed GPU work did not write through the new mapping");
    require(f.target.Word(64) == 0, "resumed GPU work wrote through a stale, replaced mapping");
    f.driver.Shutdown();
}

// (b) GPU-visible set unchanged: no wait at all, even with GPU work in flight.
void unchangedSetSkipsWait(id<MTLDevice> device, id<MTLLibrary> library) {
    Fixture f;
    f.Configure(device, library);
    f.holdEop = true;
    std::vector<std::uint32_t> words;
    EopInterrupt(words, Labels + 8, 0xb0);
    f.Submit(words);
    f.WaitEopEntered();
    bool cpuRan = false, gpuStillBusy = false;
    const auto blocked = f.Mutate(f.target, [&] {
        cpuRan = true;
        std::lock_guard lock(f.mutex);
        gpuStillBusy = !f.eopReturned;
    });
    const bool busyAfter = [&] { std::lock_guard lock(f.mutex); return !f.eopReturned; }();
    f.ReleaseEop();
    require(cpuRan && gpuStillBusy && busyAfter,
        "a mapping change that leaves the GPU-visible set unchanged waited for in-flight GPU work");
    std::cerr << "[mapping-wait] unchanged GPU-visible set: mapping change returned in " << blocked << " s\n";
    f.DrainWithWatchdog(0, "GPU did not finish after the EOP was released");
    require(f.labels.Word(8) == 0xb0, "EOP label was not written");
    f.driver.Shutdown();
}

// (c) Overlapping change: running work finishes first; later work sees the new backing.
void overlappingChangeWaits(id<MTLDevice> device, id<MTLLibrary> library) {
    Fixture f;
    f.Configure(device, library);
    f.holdEop = true;
    std::vector<std::uint32_t> words;
    WriteData(words, Target + 16, 0xc1);
    EopInterrupt(words, Labels + 8, 0xc0);
    f.Submit(words);
    f.WaitEopEntered();
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        f.ReleaseEop();
    });
    bool drainedFirst = false;
    f.Mutate(f.replacement, [&] {
        std::lock_guard lock(f.mutex);
        drainedFirst = f.eopReturned;
    });
    releaser.join();
    require(drainedFirst, "CPU mutation of a GPU-visible range ran before accepted, running GPU work finished");
    require(f.target.Word(16) == 0xc1 && f.replacement.Word(16) == 0, "pre-change GPU work did not use the old backing");
    f.holdEop = false;
    words.clear();
    WriteData(words, Target + 32, 0xc2);
    f.Submit(words);
    f.DrainWithWatchdog(0, "post-change GPU work did not finish");
    require(f.replacement.Word(32) == 0xc2 && f.target.Word(32) == 0, "post-change GPU work did not use the new backing");
    f.driver.Shutdown();
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            require(argc == 2, "usage: anyps5_metal_mapping_wait_test <utility.metallib>");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "Metal device unavailable");
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
            require(library != nil, "cannot load the utility metallib");
            parkedWaitDoesNotDeadlock(device, library);
            unchangedSetSkipsWait(device, library);
            overlappingChangeWaits(device, library);
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "anyps5_metal_mapping_wait: pass\n";
    return 0;
}
