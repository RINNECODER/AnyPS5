#import <Foundation/Foundation.h>
#include "MetalComputeDispatch.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgc/Misc/include/Suspend.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <atomic>
#include <cstring>
#include "prx/libSceAgcDriver/Execution/include/ComputeDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::array<std::uint32_t, 17> Vop1ControlCode{
    0x34020082, 0xe0302000, 0x80000401, 0xbf8c3f70, 0x7e000000, 0x7e003600, 0x7e008200, 0x4a080881,
    0xd5800000, 0x00000000, 0xd59b0000, 0x00000000, 0xd5c10000, 0x00000000, 0xe0702000, 0x80010401, 0xbf810000,
};
constexpr std::array<std::uint32_t, 22> VccBaseCode{
    0xbeea0400, 0x34020082, 0x34040084, 0x340c0085, 0x4a0c0cff, 0x00001000, 0x4ad404ff, 0x00000048,
    0xdc308010, 0x046a0001, 0xdcc98700, 0x0c6a0201, 0xdc3887b8, 0x006a006a, 0xbf8c3f70, 0xdc788000,
    0x006a0006, 0xdc708010, 0x006a0406, 0xdc708014, 0x006a0c06, 0xbf810000,
};

constexpr std::array<std::uint32_t, 24> Wave32SubgroupCode{
    0x34020084, 0xd765000a, 0x000100c1, 0x3604009f, 0x7d880488, 0xbe880f6a, 0x7e160208, 0xd7600009,
    0x00010700, 0x7e180209, 0xbe9e037e, 0x7e1a0280, 0x7da80488, 0x7e1a0281, 0xbefe031e, 0xe0701000,
    0x80010a01, 0xe0701004, 0x80010b01, 0xe0701008, 0x80010c01, 0xe070100c, 0x80010d01, 0xbf810000,
};

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::vector<std::uint32_t> Packet(std::uint32_t opcode, std::span<const std::uint32_t> payload) {
    Require(!payload.empty(), "PM4 replay packet payload is empty");
    std::vector<std::uint32_t> packet{
        0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1u) << 16u) | (opcode << 8u)};
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

void SetShaderRegisters(AgcDriver::QueueState& queue, std::uint32_t first,
                        std::span<const std::uint32_t> words) {
    std::vector<std::uint32_t> payload{first};
    payload.insert(payload.end(), words.begin(), words.end());
    const auto packet = Packet(0x76, payload);
    AgcDriver::Pm4::Validate(packet, 0x20);
    AgcDriver::Pm4::Execute(packet, queue);
}

std::array<std::uint32_t, 4> Descriptor(std::uint64_t address, std::uint32_t words) {
    return {static_cast<std::uint32_t>(address),
        static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), words, 0x11016facu};
}

void Configure(AgcDriver::QueueState& queue, std::uint64_t codeAddress,
               std::span<const std::uint32_t> userData) {
    const std::array<std::uint32_t, 3> threads{64, 1, 1};
    const std::array<std::uint32_t, 2> program{
        static_cast<std::uint32_t>(codeAddress >> 8u), static_cast<std::uint32_t>(codeAddress >> 40u)};
    const std::array<std::uint32_t, 1> rsrc2{static_cast<std::uint32_t>(userData.size()) << 1u};
    SetShaderRegisters(queue, 0x207, threads);
    SetShaderRegisters(queue, 0x20c, program);
    SetShaderRegisters(queue, 0x213, rsrc2);
    SetShaderRegisters(queue, 0x240, userData);
}

std::array<std::uint32_t, 8> UserData(std::uint64_t input, std::uint64_t output, std::uint32_t words) {
    const auto in = Descriptor(input, words), out = Descriptor(output, words);
    std::array<std::uint32_t, 8> userData;
    std::copy(in.begin(), in.end(), userData.begin());
    std::copy(out.begin(), out.end(), userData.begin() + 4);
    return userData;
}

class PublicationFlip final : public AgcDriver::IFlipRequest {
public:
    std::atomic<unsigned> ready{0};
    std::mutex mutex;
    std::exception_ptr failure;
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override { ++ready; }
    void Fail(std::exception_ptr error) noexcept override {
        std::lock_guard lock(mutex);
        if (!failure) failure = error;
    }
};

class PublicationOutput final : public AgcDriver::IVideoOutput, public AgcDriver::IRenderingWait,
    public std::enable_shared_from_this<PublicationOutput> {
public:
    std::mutex mutex;
    std::condition_variable changed;
    std::exception_ptr failure;
    bool entered = false, awakened = false, retire = false, exited = false;
    std::vector<std::shared_ptr<PublicationFlip>> flips;
    std::function<void()> onFailure;
    std::atomic<bool> checkedCallback{false};
    std::shared_ptr<AgcDriver::IRenderingWait> CaptureRenderingWait(std::uint32_t index) override {
        Require(index == 0, "Publication rendering wait references the wrong buffer");
        return shared_from_this();
    }
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override {
        auto flip = std::make_shared<PublicationFlip>();
        flips.push_back(flip);
        return flip;
    }
    void Wait() override {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        // The owner takes this same mutex after observing entered: the worker
        // has actually released it into this wait, not merely been launched.
        changed.wait(lock, [&] { return failure != nullptr; });
        awakened = true;
        changed.notify_all();
        changed.wait(lock, [&] { return retire; });
        exited = true;
        std::rethrow_exception(failure);
    }
    void Fail(std::exception_ptr error) noexcept override {
        {
            std::lock_guard lock(mutex);
            if (!failure) failure = error;
        }
        changed.notify_all();
        if (onFailure && !checkedCallback.exchange(true)) onFailure();
    }
    void ReleaseWorker() {
        std::lock_guard lock(mutex);
        retire = true;
        changed.notify_all();
    }
};

template<class F> std::exception_ptr PublicationError(F&& action) {
    try { action(); } catch (...) { return std::current_exception(); }
    return {};
}

void PublicationFailureReplay(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr std::uint64_t CommandsAddress = 0x910000, LabelAddress = 0x920000,
        HeaderAddress = 0x930000, CodeAddress = 0x940000;
    // Active graphics submission stalls before its flip and EOP; the second
    // submission's reservation is pending on that same worker.
    std::array<std::uint32_t, 18> commands{
        AgcDriver::RenderingWaitPacketHeader, 7, 0, 0,
        AgcDriver::FlipPacketHeader, 7, 0, 1, 0, 0,
        0xc0064900, 0, (1u << 29) | (1u << 24), static_cast<std::uint32_t>(LabelAddress + 4), 0, 1, 0, 0};
    const auto originalCommands = commands;
    std::array<std::uint32_t, 3> label{0xcafef00d, 0, 0xdeadbeef};
    const auto originalLabel = label;
    std::array<std::uint32_t, 1> code{0xbf810000};
    Shader header{};
    header.file_header = 0x34333231;
    header.version = 0x18;
    header.code = reinterpret_cast<const volatile void*>(CodeAddress);
    header.header_size = sizeof(header);
    header.shader_size = sizeof(code);
    const auto originalHeader = header;
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 4> ranges{{
        {CommandsAddress, std::as_writable_bytes(std::span(commands)), false},
        {LabelAddress, std::as_writable_bytes(std::span(label)), true},
        {HeaderAddress, std::as_writable_bytes(std::span(&header, 1)), true},
        {CodeAddress, std::as_writable_bytes(std::span(code)), false}}};
    const std::array<AgcDriver::Metal::ReadableGuestRange, 2> readable{{
        {HeaderAddress, sizeof(header)}, {CodeAddress, sizeof(code)}}};
    const auto publishError = std::make_exception_ptr(std::runtime_error("shader publication failed"));
    const auto rollbackError = std::make_exception_ptr(std::runtime_error("shader rollback failed"));
    const auto laterError = std::make_exception_ptr(std::runtime_error("later driver failure"));
    std::atomic<unsigned> interrupts{0};
    AgcDriver::Metal::MetalDriver driver;
    driver.Configure((__bridge void*)device, (__bridge void*)library, ranges,
        [&](std::uint32_t) { ++interrupts; });
    auto retained = std::make_shared<PublicationOutput>();
    auto registered = std::make_shared<PublicationOutput>();
    std::exception_ptr callbackAdmission;
    registered->onFailure = [&] {
        // Reentry here must see the terminal error, not publication reentry.
        callbackAdmission = PublicationError([&] { driver.SuspendPoint(); });
        // A different thread acquiring gpuMutex catches lock retention even
        // though the mutex is recursive on the publication thread.
        std::thread gpuProbe([&] { driver.ReleaseWindow(nullptr); });
        gpuProbe.join();
    };
    std::future<std::exception_ptr> drain;
    try {
        driver.RegisterVideoOutput(7, retained);
        driver.RegisterVideoOutput(8, registered);
        driver.SubmitCommandBuffer(CommandsAddress, commands.size(), 0, 0);
        {
            std::unique_lock lock(retained->mutex);
            Require(retained->changed.wait_for(lock, std::chrono::seconds(5), [&] { return retained->entered; }),
                "Publication worker never blocked in the actual rendering wait");
        }
        driver.SubmitCommandBuffer(CommandsAddress + 4 * sizeof(std::uint32_t), commands.size() - 4, 0, 0);
        driver.UnregisterVideoOutput(7, retained);
        Require(retained->flips.size() == 2, "Publication fixture lacks active and pending flip reservations");
        bool published = false, rolledBack = false;
        const auto reported = PublicationError([&] {
            driver.RegisterShaderWithPublication(HeaderAddress, readable,
                [&] { published = true; std::rethrow_exception(publishError); },
                [&] { rolledBack = true; std::rethrow_exception(rollbackError); });
        });
        Require(published && rolledBack && reported == rollbackError,
            "Publication did not execute both injected failures and propagate rollback failure");
        std::cout << "Publication rollback injected after actual rendering-wait entry; starting WaitIdle drain" << std::endl;
        drain = std::async(std::launch::async, [&] { return PublicationError([&] { driver.WaitIdle(); }); });
        bool woke;
        {
            std::unique_lock lock(retained->mutex);
            woke = retained->changed.wait_for(lock, std::chrono::seconds(2), [&] { return retained->awakened; });
        }
        if (!woke) {
            // Do not rescue with Shutdown/ReportFailure before observing the
            // missing wake. Cleanup below rescues and joins the baseline worker.
            Require(drain.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
                "Publication drain returned early while its rendering worker remained asleep");
            throw std::runtime_error("Rollback failure omitted output wakeup; WaitIdle remains blocked");
        }
        Require(drain.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
            "Publication drain returned before the awakened worker retired");
        Require(registered->failure == rollbackError && retained->failure == rollbackError &&
            callbackAdmission == rollbackError, "Publication fanout lost the failure or retained publication/driver locks");
        Require(retained->flips[1]->failure == rollbackError && retained->flips[1]->ready == 0,
            "Publication failure did not retire its pending flip without success");
        Require(PublicationError([&] { driver.SubmitCommandBuffer(CommandsAddress, commands.size(), 0, 0); }) == rollbackError &&
            PublicationError([&] { driver.RegisterShader(reinterpret_cast<const Shader*>(HeaderAddress)); }) == rollbackError &&
            PublicationError([&] { driver.SuspendPoint(); }) == rollbackError,
            "Publication failure admitted new work");
        retained->ReleaseWorker();
        Require(drain.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "Publication drain did not finish after rendering-worker retirement");
        Require(drain.get() == rollbackError && retained->exited,
            "Publication drain reported success or the wrong terminal failure");
        driver.ReportFailure(laterError);
        Require(PublicationError([&] { driver.WaitIdle(); }) == rollbackError &&
            PublicationError([&] { driver.Shutdown(); }) == rollbackError,
            "Publication shutdown or later failure replaced the sticky first failure");
        for (const auto& flip : retained->flips)
            Require(flip->failure == rollbackError && flip->ready == 0, "Publication failure leaked a flip or reported GPU success");
        Require(interrupts == 0 && label == originalLabel && commands == originalCommands &&
            std::memcmp(&header, &originalHeader, sizeof(header)) == 0 && code[0] == 0xbf810000,
            "Publication failure delivered EOP or changed borrowed memory");
    } catch (...) {
        const auto error = std::current_exception();
        retained->ReleaseWorker();
        // Shutdown is the existing rescue path even on the unfixed baseline.
        PublicationError([&] { driver.Shutdown(); });
        if (drain.valid()) drain.get();
        std::rethrow_exception(error);
    }
    std::cout << "Shader publication rollback failure: registered/retained output wakeup, pending/active flip retirement, closed admission, drained worker and sticky failure passed\n";
}

void OriginalWave32Subgroup(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr std::uint32_t Sentinel = 0xdeadbeef, Threads = 64, Results = 4, GuardWords = 64;
    constexpr std::uint64_t CodeAllocation = 0x700000, CodeAddress = CodeAllocation + GuardWords * 4,
        OutputAllocation = 0x710000, OutputAddress = OutputAllocation + GuardWords * 4,
        HeaderAllocation = 0x720000, HeaderAddress = HeaderAllocation + GuardWords * 4,
        CommandsAllocation = 0x730000, CommandsAddress = CommandsAllocation + GuardWords * 4,
        PacketAllocation = 0x740000, PacketAddress = PacketAllocation + GuardWords * 4, LabelAddress = 0x750000;
    std::array<std::uint32_t, GuardWords * 2 + Wave32SubgroupCode.size()> code;
    code.fill(Sentinel);
    std::copy(Wave32SubgroupCode.begin(), Wave32SubgroupCode.end(), code.begin() + GuardWords);
    const auto originalCode = code;
    std::array<std::uint32_t, GuardWords * 2 + Threads * Results> output;
    output.fill(Sentinel);
    auto expectedOutput = output;
    constexpr std::array<const char*, Results> names{
        "v_mbcnt_lo_u32_b32", "s_bcnt1 of a VCC compare", "v_readlane_b32 lane 3", "v_cmpx EXEC"};
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto lane = tid % 32u;
        const std::array<std::uint32_t, Results> expected{lane, 8u, tid - lane + 3u, lane < 8u ? 1u : 0u};
        std::copy(expected.begin(), expected.end(), expectedOutput.begin() + GuardWords + tid * Results);
    }
    std::array<std::byte, GuardWords * 8 + sizeof(Shader) + sizeof(ShaderUserData)> headerBytes;
    headerBytes.fill(std::byte{0x7b});
    std::fill_n(headerBytes.begin() + GuardWords * 4, sizeof(Shader) + sizeof(ShaderUserData), std::byte{0});
    Shader header{};
    header.file_header = 0x34333231;
    header.version = 0x18;
    header.code = reinterpret_cast<const volatile void*>(CodeAddress);
    header.user_data = reinterpret_cast<ShaderUserData*>(HeaderAddress + sizeof(Shader));
    header.header_size = sizeof(Shader) + sizeof(ShaderUserData);
    header.shader_size = sizeof(Wave32SubgroupCode);
    std::memcpy(headerBytes.data() + GuardWords * 4, &header, sizeof(header));
    const auto originalHeader = headerBytes;
    std::array<std::uint32_t, 8> userData{};
    const auto descriptor = Descriptor(OutputAddress, Threads * Results);
    std::copy(descriptor.begin(), descriptor.end(), userData.begin() + 4);
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        std::vector<std::uint32_t> payload{first};
        payload.insert(payload.end(), values.begin(), values.end());
        const auto packet = Packet(0x76, payload);
        words.insert(words.end(), packet.begin(), packet.end());
    };
    const std::array<std::uint32_t, 3> threads{Threads, 1, 1};
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(CodeAddress >> 8u), 0};
    const std::array<std::uint32_t, 1> rsrc2{static_cast<std::uint32_t>(userData.size()) << 1u};
    registers(0x207, threads);
    registers(0x20c, program);
    registers(0x213, rsrc2);
    registers(0x240, userData);
    const std::array<std::uint32_t, 4> dispatchPayload{1, 1, 1, 0x8041};
    const auto dispatch = Packet(0x15, dispatchPayload);
    words.insert(words.end(), dispatch.begin(), dispatch.end());
    const std::array<std::uint32_t, 8> eop{
        0xc0064900, 0, (1u << 29) | (1u << 24), static_cast<std::uint32_t>(LabelAddress + 4), 0, 1, 0, 0};
    words.insert(words.end(), eop.begin(), eop.end());
    std::array<std::uint32_t, GuardWords * 2 + 128> commands;
    commands.fill(Sentinel);
    Require(words.size() <= 128, "Original wave32 commands exceed their guarded allocation");
    std::copy(words.begin(), words.end(), commands.begin() + GuardWords);
    const auto originalCommands = commands;
    std::array<std::byte, GuardWords * 8 + sizeof(::Packet)> packetBytes;
    packetBytes.fill(std::byte{0x7b});
    const ::Packet packet{reinterpret_cast<std::uint32_t*>(CommandsAddress), static_cast<std::uint32_t>(words.size()), 0, {}};
    std::memcpy(packetBytes.data() + GuardWords * 4, &packet, sizeof(packet));
    const auto originalPacket = packetBytes;
    std::array<std::uint32_t, 3> label{Sentinel, 0, Sentinel};
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 6> ranges{{
        {CodeAllocation, std::as_writable_bytes(std::span(code)), false},
        {OutputAllocation, std::as_writable_bytes(std::span(output)), true},
        {HeaderAllocation, headerBytes, false},
        {CommandsAllocation, std::as_writable_bytes(std::span(commands)), false},
        {PacketAllocation, packetBytes, false},
        {LabelAddress, std::as_writable_bytes(std::span(label)), true}}};
    std::atomic<unsigned> callbacks{0};
    AgcDriver::Metal::MetalDriver driver;
    driver.Configure((__bridge void*)device, (__bridge void*)library, ranges, [&](std::uint32_t queue) {
        Require(queue == 0x20 && label[1] == 1, "Original wave32 EOP queue or completed label differs");
        callbacks.fetch_add(1);
    });
    try {
        driver.RegisterShader(reinterpret_cast<const Shader*>(HeaderAddress));
        driver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0x20);
        driver.WaitIdle();
        for (std::uint32_t tid = 0; tid < Threads; ++tid)
            for (std::uint32_t result = 0; result < Results; ++result) {
                const auto index = GuardWords + tid * Results + result;
                Require(output[index] == expectedOutput[index],
                    "Original wave32 thread " + std::to_string(tid) + " " + names[result] + " is " +
                    std::to_string(output[index]) + ", expected " + std::to_string(expectedOutput[index]));
            }
        Require(output == expectedOutput, "Original wave32 changed its output allocation guards");
        Require(label == std::array<std::uint32_t, 3>{Sentinel, 1, Sentinel} && callbacks.load() == 1,
            "Original wave32 did not finish exactly one guarded EOP");
        Require(code == originalCode && headerBytes == originalHeader && commands == originalCommands && packetBytes == originalPacket,
            "Original wave32 changed borrowed read-only code, header, commands, packet or allocation guards");
        driver.Shutdown();
    } catch (...) {
        try { driver.Shutdown(); } catch (...) {}
        throw;
    }
    std::cout << "Original RDNA wave32 subgroup: public driver RegisterShader/Submit methods, two waves, lane/count/readlane/EXEC goldens and guarded EOP passed\n";
}

namespace RetainedDsPermute {
alignas(256) constexpr std::array<std::uint32_t, 37> Code{
    0x34020084, 0x34060086, 0xe0301000, 0x80000401, 0xe0301004, 0x80000501, 0xbf8c3f70, 0x7e1402ff,
    0xdeaddead, 0x7e1602ff, 0xdeaddead, 0x7e1802ff, 0xdeaddead, 0x7e1a02ff, 0xdeaddead, 0xdac80000,
    0x0a000504, 0xdacc0000, 0x0b000504, 0xdac80008, 0x0c000504, 0xbe94037e, 0xbefe03ff, 0x0000ffff,
    0xdac80000, 0x0d000504, 0xbefe0314, 0xbf8cc07f, 0xe0701000, 0x80010a03, 0xe0701004, 0x80010b03,
    0xe0701008, 0x80010c03, 0xe070100c, 0x80010d03, 0xbf810000,
};

constexpr std::uint32_t Rows[32][2] = {
    {0x000000f4u, 0x00001000u},
    {0x000000dcu, 0x00001001u},
    {0x000000f2u, 0x00001002u},
    {0x000000d9u, 0x00001003u},
    {0x0000000eu, 0x00001004u},
    {0x00000017u, 0x00001005u},
    {0x00000015u, 0x00001006u},
    {0x0000005cu, 0x00001007u},
    {0x000000d5u, 0x00001008u},
    {0x0000002bu, 0x00001009u},
    {0x000000bcu, 0x0000100au},
    {0x000000cfu, 0x0000100bu},
    {0x000000abu, 0x0000100cu},
    {0x000000dau, 0x0000100du},
    {0x0000004eu, 0x0000100eu},
    {0x00000040u, 0x0000100fu},
    {0x0000009bu, 0x00001010u},
    {0x00000036u, 0x00001011u},
    {0x0000009bu, 0x00001012u},
    {0x00000009u, 0x00001013u},
    {0x00000094u, 0x00001014u},
    {0x000000aeu, 0x00001015u},
    {0x00000028u, 0x00001016u},
    {0x000000ffu, 0x00001017u},
    {0x0000006eu, 0x00001018u},
    {0x000000a3u, 0x00001019u},
    {0x00000064u, 0x0000101au},
    {0x000000cdu, 0x0000101bu},
    {0x000000b9u, 0x0000101cu},
    {0x000000dcu, 0x0000101du},
    {0x000000feu, 0x0000101eu},
    {0x00000082u, 0x0000101fu},
};

constexpr std::uint32_t Expected[32][4] = {
    {0x0000101fu, 0x0000101du, 0x00000000u, 0x00000000u},
    {0x00000000u, 0x00001017u, 0x0000101eu, 0x00000000u},
    {0x00001013u, 0x0000101cu, 0x0000101fu, 0x00000000u},
    {0x00001004u, 0x00001016u, 0x00000000u, 0x00001004u},
    {0x00000000u, 0x00001003u, 0x00001013u, 0x00000000u},
    {0x00001014u, 0x00001005u, 0x00001004u, 0x00001006u},
    {0x00001012u, 0x00001005u, 0x00000000u, 0x00000000u},
    {0x00000000u, 0x00001017u, 0x00001014u, 0x00000000u},
    {0x00001019u, 0x00001015u, 0x00001012u, 0x00000000u},
    {0x00000000u, 0x0000100au, 0x00000000u, 0x00000000u},
    {0x00001016u, 0x0000100fu, 0x00001019u, 0x0000100cu},
    {0x00001015u, 0x00001013u, 0x00000000u, 0x00000000u},
    {0x00000000u, 0x0000100au, 0x00001016u, 0x00000000u},
    {0x00001011u, 0x00001016u, 0x00001015u, 0x00000000u},
    {0x0000101cu, 0x00001013u, 0x00000000u, 0x00000000u},
    {0x0000100au, 0x00001010u, 0x00001011u, 0x0000100au},
    {0x0000100fu, 0x00001006u, 0x0000101cu, 0xdeaddeadu},
    {0x00000000u, 0x0000100du, 0x0000100au, 0xdeaddeadu},
    {0x00000000u, 0x00001006u, 0x0000100fu, 0xdeaddeadu},
    {0x0000101bu, 0x00001002u, 0x00000000u, 0xdeaddeadu},
    {0x00000000u, 0x00001005u, 0x00000000u, 0xdeaddeadu},
    {0x00001008u, 0x0000100bu, 0x0000101bu, 0xdeaddeadu},
    {0x0000100du, 0x0000100au, 0x00000000u, 0xdeaddeadu},
    {0x0000101du, 0x0000101fu, 0x00001008u, 0xdeaddeadu},
    {0x00000000u, 0x0000101bu, 0x0000100du, 0xdeaddeadu},
    {0x0000101au, 0x00001008u, 0x0000101du, 0xdeaddeadu},
    {0x00000000u, 0x00001019u, 0x00000000u, 0xdeaddeadu},
    {0x00001018u, 0x00001013u, 0x0000101au, 0xdeaddeadu},
    {0x00001002u, 0x0000100eu, 0x00000000u, 0xdeaddeadu},
    {0x00001000u, 0x00001017u, 0x00001018u, 0xdeaddeadu},
    {0x00000000u, 0x0000101fu, 0x00001002u, 0xdeaddeadu},
    {0x0000101eu, 0x00001000u, 0x00001000u, 0xdeaddeadu},
};

constexpr const char* Names[4] = {"permute", "bpermute", "permute_offset8", "permute_exec_low16"};

constexpr std::array<std::uint32_t, 128> Input = [] {
    std::array<std::uint32_t, 128> input{};
    for (std::uint32_t tid = 0; tid < 32; ++tid) {
        input[tid * 4] = Rows[tid][0];
        input[tid * 4 + 1] = Rows[tid][1];
    }
    return input;
}();
constexpr std::array<std::uint32_t, 512> FullExpected = [] {
    std::array<std::uint32_t, 512> output{};
    output.fill(0xdeadbeefu);
    for (std::uint32_t tid = 0; tid < 32; ++tid)
        for (std::uint32_t result = 0; result < 4; ++result)
            output[tid * 16 + result] = Expected[tid][result];
    return output;
}();
}

namespace RetainedReadLane {
constexpr std::array<std::uint32_t, 21> RetainedReadLaneCode{
    0x34020082, 0xe0302000, 0x80000401, 0xe0302004, 0x80000501, 0xbf8c3f70, 0xd7600008, 0x00014b04,
    0x7e280505, 0xd7600009, 0x00002904, 0x7e140208, 0x7e160209, 0x7e180214, 0xe0702000, 0x80010a01,
    0xe0702004, 0x80010b01, 0xe0702008, 0x80010c01, 0xbf810000,
};

constexpr std::array<const char*, 3> RetainedReadLaneNames{
    "v_readlane_b32 inline 37 normalized to lane 5",
    "v_readlane_b32 loaded SGPR selector 69 normalized to lane 5",
    "v_readfirstlane_b32 loaded selector 69"};

constexpr std::array<std::uint32_t, 128> RetainedReadLaneInput = [] {
    std::array<std::uint32_t, 128> input{};
    for (std::uint32_t tid = 0; tid < 32; ++tid) {
        input[tid * 4] = 0xa0000000u + tid * 0x01010101u;
        input[tid * 4 + 1] = 69u + tid;
    }
    return input;
}();

constexpr std::array<std::uint32_t, 128> RetainedReadLaneExpected = [] {
    std::array<std::uint32_t, 128> expected{};
    for (std::uint32_t tid = 0; tid < 32; ++tid) {
        expected[tid * 4] = 0xa5050505u;
        expected[tid * 4 + 1] = 0xa5050505u;
        expected[tid * 4 + 2] = 69u;
        expected[tid * 4 + 3] = 0xdeadbeefu;
    }
    return expected;
}();
}

struct RetainedSubgroupCase {
    const char* name;
    std::span<const std::uint32_t> code, input, expected;
    std::uint32_t outputRowWords, descriptorStrideBytes;
    std::span<const char* const> resultNames;
};

void RetainedSubgroupReplay(id<MTLDevice> device, id<MTLLibrary> library, const RetainedSubgroupCase& test) {
    constexpr std::uint32_t Threads = 32, GuardWords = 64, Sentinel = 0xdeadbeef;
    constexpr std::uint64_t CodeAllocation = 0x800000, CodeAddress = CodeAllocation + GuardWords * 4,
        InputAllocation = 0x810000, InputAddress = InputAllocation + GuardWords * 4,
        OutputAllocation = 0x820000, OutputAddress = OutputAllocation + GuardWords * 4,
        HeaderAllocation = 0x830000, HeaderAddress = HeaderAllocation + GuardWords * 4,
        CommandsAllocation = 0x840000, CommandsAddress = CommandsAllocation + GuardWords * 4,
        PacketAllocation = 0x850000, PacketAddress = PacketAllocation + GuardWords * 4, LabelAddress = 0x860000;
    std::vector<std::uint32_t> code(test.code.size() + GuardWords * 2, Sentinel);
    std::copy(test.code.begin(), test.code.end(), code.begin() + GuardWords);
    const auto originalCode = code;
    std::vector<std::uint32_t> input(test.input.size() + GuardWords * 2, Sentinel);
    std::copy(test.input.begin(), test.input.end(), input.begin() + GuardWords);
    const auto originalInput = input;
    std::vector<std::uint32_t> output(test.expected.size() + GuardWords * 2, Sentinel);
    auto expectedOutput = output;
    std::copy(test.expected.begin(), test.expected.end(), expectedOutput.begin() + GuardWords);
    std::array<std::byte, GuardWords * 8 + sizeof(Shader) + sizeof(ShaderUserData)> headerBytes;
    headerBytes.fill(std::byte{0x7b});
    std::fill_n(headerBytes.begin() + GuardWords * 4, sizeof(Shader) + sizeof(ShaderUserData), std::byte{0});
    Shader header{};
    header.file_header = 0x34333231;
    header.version = 0x18;
    header.code = reinterpret_cast<const volatile void*>(CodeAddress);
    header.user_data = reinterpret_cast<ShaderUserData*>(HeaderAddress + sizeof(Shader));
    header.header_size = sizeof(Shader) + sizeof(ShaderUserData);
    header.shader_size = static_cast<std::uint32_t>(test.code.size_bytes());
    std::memcpy(headerBytes.data() + GuardWords * 4, &header, sizeof(header));
    const auto originalHeader = headerBytes;
    const auto descriptor = [&](std::uint64_t address, std::span<const std::uint32_t> payload) {
        const auto count = test.descriptorStrideBytes == 0 ? payload.size_bytes() : payload.size();
        return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(address),
            static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (test.descriptorStrideBytes << 16u),
            static_cast<std::uint32_t>(count), test.descriptorStrideBytes == 0 ? 0x31016facu : 0x11016facu};
    };
    const auto in = descriptor(InputAddress, test.input), out = descriptor(OutputAddress, test.expected);
    std::array<std::uint32_t, 8> userData{};
    std::copy(in.begin(), in.end(), userData.begin());
    std::copy(out.begin(), out.end(), userData.begin() + 4);
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        std::vector<std::uint32_t> payload{first};
        payload.insert(payload.end(), values.begin(), values.end());
        const auto packet = Packet(0x76, payload);
        words.insert(words.end(), packet.begin(), packet.end());
    };
    const std::array<std::uint32_t, 3> threads{Threads, 1, 1};
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(CodeAddress >> 8u), 0};
    const std::array<std::uint32_t, 1> rsrc2{static_cast<std::uint32_t>(userData.size()) << 1u};
    registers(0x207, threads);
    registers(0x20c, program);
    registers(0x213, rsrc2);
    registers(0x240, userData);
    const std::array<std::uint32_t, 4> dispatchPayload{1, 1, 1, 0x8041};
    const auto dispatch = Packet(0x15, dispatchPayload);
    words.insert(words.end(), dispatch.begin(), dispatch.end());
    const std::array<std::uint32_t, 8> eop{
        0xc0064900, 0, (1u << 29) | (1u << 24), static_cast<std::uint32_t>(LabelAddress + 4), 0, 1, 0, 0};
    words.insert(words.end(), eop.begin(), eop.end());
    std::array<std::uint32_t, GuardWords * 2 + 128> commands;
    commands.fill(Sentinel);
    Require(words.size() <= 128, "Retained subgroup commands exceed their guarded allocation");
    std::copy(words.begin(), words.end(), commands.begin() + GuardWords);
    const auto originalCommands = commands;
    std::array<std::byte, GuardWords * 8 + sizeof(::Packet)> packetBytes;
    packetBytes.fill(std::byte{0x7b});
    const ::Packet packet{reinterpret_cast<std::uint32_t*>(CommandsAddress), static_cast<std::uint32_t>(words.size()), 0, {}};
    std::memcpy(packetBytes.data() + GuardWords * 4, &packet, sizeof(packet));
    const auto originalPacket = packetBytes;
    std::array<std::uint32_t, 3> label{Sentinel, 0, Sentinel};
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 7> ranges{{
        {CodeAllocation, std::as_writable_bytes(std::span(code)), false},
        {InputAllocation, std::as_writable_bytes(std::span(input)), false},
        {OutputAllocation, std::as_writable_bytes(std::span(output)), true},
        {HeaderAllocation, headerBytes, false},
        {CommandsAllocation, std::as_writable_bytes(std::span(commands)), false},
        {PacketAllocation, packetBytes, false},
        {LabelAddress, std::as_writable_bytes(std::span(label)), true}}};
    std::atomic<unsigned> callbacks{0};
    AgcDriver::Metal::MetalDriver driver;
    driver.Configure((__bridge void*)device, (__bridge void*)library, ranges, [&](std::uint32_t queue) {
        Require(queue == 0x20 && label[1] == 1, "Retained subgroup EOP queue or completed label differs");
        callbacks.fetch_add(1);
    });
    try {
        driver.RegisterShader(reinterpret_cast<const Shader*>(HeaderAddress));
        driver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0x20);
        driver.WaitIdle();
        for (std::uint32_t tid = 0; tid < Threads; ++tid)
            for (std::uint32_t result = 0; result < test.resultNames.size(); ++result) {
                const auto index = GuardWords + tid * test.outputRowWords + result;
                Require(output[index] == expectedOutput[index],
                    std::string(test.name) + " lane " + std::to_string(tid) + " " + test.resultNames[result] + " is " +
                    std::to_string(output[index]) + ", expected " + std::to_string(expectedOutput[index]));
            }
        Require(output == expectedOutput, std::string(test.name) + " changed untouched output padding or allocation guards");
        Require(label == std::array<std::uint32_t, 3>{Sentinel, 1, Sentinel} && callbacks.load() == 1,
            std::string(test.name) + " did not finish exactly one guarded EOP");
        Require(input == originalInput && code == originalCode && headerBytes == originalHeader &&
                commands == originalCommands && packetBytes == originalPacket,
            std::string(test.name) + " changed read-only input, code, header, commands, packet or allocation guards");
        driver.Shutdown();
    } catch (...) {
        try { driver.Shutdown(); } catch (...) {}
        throw;
    }
    std::cout << test.name << ": public driver RegisterShader/Submit methods, original goldens, padding and guarded EOP passed\n";
}

void RetainedSubgroupReplays(id<MTLDevice> device, id<MTLLibrary> library) {
    const std::array<RetainedSubgroupCase, 2> cases{{
        {"Original DS permute", RetainedDsPermute::Code, RetainedDsPermute::Input, RetainedDsPermute::FullExpected,
            16, 0, RetainedDsPermute::Names},
        {"Original readlane", RetainedReadLane::RetainedReadLaneCode, RetainedReadLane::RetainedReadLaneInput,
            RetainedReadLane::RetainedReadLaneExpected, 4, 4, RetainedReadLane::RetainedReadLaneNames}}};
    for (const auto& test : cases) RetainedSubgroupReplay(device, library, test);
}

void OriginalSubmitExports(id<MTLDevice> device, id<MTLLibrary> library) {
    alignas(8) ::Packet packet{reinterpret_cast<std::uint32_t*>(0x500000), 8, 0, {}};
    std::array<std::uint32_t, 8> commands{0xc0064900, 0, (1u << 29) | (1u << 24), 0x600004, 0, 1, 0, 0};
    std::array<std::uint32_t, 3> label{0xcafef00d, 0, 0xdeadbeef};
    const auto originalPacket = packet;
    std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 3> ranges{{
        {0x400000, std::as_writable_bytes(std::span(&packet, 1)), false},
        {0x500000, std::as_writable_bytes(std::span(commands)), false},
        {0x600000, std::as_writable_bytes(std::span(label)), true}}};
    std::uint32_t expectedQueue = 0, expectedLabel = 1;
    std::atomic<unsigned> callbacks{0};
    auto& driver = AgcDriver::Metal::MetalDriver::Get();
    driver.Configure((__bridge void*)device, (__bridge void*)library, ranges, [&](std::uint32_t queue) {
        Require(queue == expectedQueue && label[1] == expectedLabel, "EOP queue or completed label differs");
        Require(label[0] == 0xcafef00d && label[2] == 0xdeadbeef, "allocation guard changed");
        callbacks.fetch_add(1);
    });
    Require(sceAgcSuspendPoint() == 0, "Original AGC suspend did not use the configured native driver");
    auto* guestPacket = reinterpret_cast<const ::Packet*>(0x400000);
    struct Case { std::uint32_t queue; int (*submit)(std::uint32_t, const ::Packet*); };
    const std::array<Case, 4> cases{{
        {0, [](std::uint32_t, const ::Packet* value) { return sceAgcDriverSubmitDcb(value); }},
        {0, [](std::uint32_t, const ::Packet* value) { return sceAgcDriverAgrSubmitDcb(value); }},
        {0x20, sceAgcDriverSubmitAcb}, {0x57, sceAgcDriverSubmitAcb}}};
    for (const auto& test : cases) {
        expectedQueue = test.queue;
        commands[5] = expectedLabel;
        const auto originalCommands = commands;
        Require(test.submit(test.queue, guestPacket) == 0, "Original Submit wrapper returned an error");
        driver.WaitIdle();
        Require(callbacks.load() == expectedLabel && label[1] == expectedLabel,
            "Original Submit wrapper did not complete its label and exactly one EOP");
        Require(commands == originalCommands, "Original Submit wrapper modified read-only guest PM4 commands");
        ++expectedLabel;
    }
    for (auto queue : {0x1fu, 0x58u}) {
        bool rejected = false;
        try {
            sceAgcDriverSubmitAcb(queue, guestPacket);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("unsupported compute queue") != std::string::npos;
            if (!rejected) throw;
        }
        Require(rejected, "Original SubmitAcb accepted an invalid compute queue");
    }
    Require(callbacks.load() == 4 && label[1] == 4, "Invalid queue rejection changed the completed EOP count or label");
    Require(std::memcmp(&packet, &originalPacket, sizeof(packet)) == 0, "read-only guest packet changed");
    driver.Shutdown();
    std::cout << "Original AGC Submit exports: checked nonidentity Packet/PM4 capture, compute queue bounds and completed EOP labels passed\n";
}

void CheckedPm4Memory() {
    constexpr std::uint32_t address = 0x600000;
    constexpr std::array<std::uint32_t, 4> initial{0xfeedface, 0, 0, 0xcafef00d};
    std::array<std::uint32_t, 4> memory = initial;
    const std::array<std::uint32_t, 5> writePayload{0x100, address + 4, 0, 0x10203040, 0x50607080};
    const std::array<std::uint32_t, 6> waitPayload{0x13, address + 4, 0, 0x10203040, 0xffffffff, 0x190};
    const auto write = Packet(0x37, writePayload), wait = Packet(0x3c, waitPayload);
    AgcDriver::QueueState queue;
    AgcDriver::Pm4::Validate(write, 0x20);
    AgcDriver::Pm4::Validate(wait, 0x20);
    const auto bytes = std::as_writable_bytes(std::span(memory));
    {
        const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 2> ranges{{
            {address, bytes.first(8), true}, {address + 8, bytes.subspan(8), true}}};
        const AgcDriver::NativeGuestMemory::BorrowedRangesScope scope(ranges);
        Require(!AgcDriver::Pm4::WaitSatisfied(wait), "PM4 wait was satisfied before the label write");
        AgcDriver::Pm4::Execute(write, queue);
        constexpr std::array<std::uint32_t, 4> expected{0xfeedface, 0x10203040, 0x50607080, 0xcafef00d};
        Require(memory == expected, "PM4 native checked WRITE_DATA changed the wrong borrowed bytes");
        Require(AgcDriver::Pm4::WaitSatisfied(wait), "PM4 native checked WAIT_REG_MEM missed the preceding label write");
        AgcDriver::Pm4::Execute(wait, queue);
    }
    for (const bool mappedReadOnly : {true, false}) {
        memory = initial;
        std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges{{address, bytes.first(8), true}};
        if (mappedReadOnly) ranges.push_back({address + 8, bytes.subspan(8), false});
        const AgcDriver::NativeGuestMemory::BorrowedRangesScope scope(ranges);
        bool rejected = false;
        try {
            AgcDriver::Pm4::Execute(write, queue);
        } catch (const std::exception& error) {
            const std::string reason(error.what());
            rejected = reason.find(mappedReadOnly ? "read-only" : "not borrowed") != std::string::npos;
            if (!rejected) throw;
        }
        Require(rejected, "PM4 native checked write accepted a partial unmapped or read-only range");
        Require(memory == initial, "PM4 native checked write partially modified memory before rejecting the range");
    }
    std::cout << "PM4 native checked label write/wait and atomic range-permission rejection passed\n";
}

void DispatchPm4(AgcDriver::Metal::MetalComputeDispatch& adapter, const AgcDriver::QueueState& queue,
                 std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,
                 std::uint32_t count = 1, std::uint32_t initiator = 0x8041) {
    const std::array<std::uint32_t, 4> payload{count, 1, 1, initiator};
    const auto packet = Packet(0x15, payload);
    AgcDriver::Pm4::Validate(packet, 0x20);
    const auto state = AgcDriver::DecodeComputeDispatch(queue, packet);
    const auto fault = adapter.DispatchSynchronously(state, Vop1ControlCode, ranges);
    Require(fault.state == ShaderRecompiler::BdaAbi::FaultState::Empty, "PM4 descriptor dispatch published a guest GPU fault");
}

void Pm4DescriptorReplay(id<MTLDevice> device) {
    constexpr std::uint64_t inputAddress = 0x100000, firstAddress = 0x200000, secondAddress = 0x300000;
    constexpr std::uint32_t codeAddress = 0x500000;
    std::array<std::uint32_t, 64 * 4> input, first, second;
    input.fill(0xa5a5a5a5);
    first.fill(0xdeadbeef);
    second.fill(0xabad1dea);
    for (std::uint32_t lane = 0; lane < 64; ++lane) input[lane * 4] = lane * 0x01010101u + 7u;
    const auto original = input;
    auto expectedFirst = first, expectedSecond = second;
    for (std::uint32_t lane = 0; lane < 64; ++lane) {
        expectedFirst[lane * 4] = original[lane * 4] + 1u;
        expectedSecond[lane * 4] = original[lane * 4] + 2u;
    }
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 3> ranges{{
        {inputAddress, std::as_writable_bytes(std::span(input)), false},
        {firstAddress, std::as_writable_bytes(std::span(first)), true},
        {secondAddress, std::as_writable_bytes(std::span(second)), true}}};
    AgcDriver::Metal::MetalComputeDispatch adapter(device);
    AgcDriver::QueueState queue;
    Configure(queue, codeAddress, UserData(inputAddress, firstAddress, input.size()));
    DispatchPm4(adapter, queue, ranges);
    Require(first == expectedFirst, "PM4 SET_SH_REG/DISPATCH_DIRECT produced an incorrect first buffer");
    Require(input == original, "PM4 descriptor dispatch modified the read-only input");
    Require(std::all_of(second.begin(), second.end(), [](auto value) { return value == 0xabad1dea; }),
            "PM4 first dispatch changed the future rebound destination");
    SetShaderRegisters(queue, 0x240, UserData(firstAddress, secondAddress, first.size()));
    DispatchPm4(adapter, queue, ranges);
    Require(second == expectedSecond, "PM4 live SGPR rebound dispatch missed the preceding GPU result or wrote the wrong destination");
    Require(first == expectedFirst && input == original, "PM4 rebound dispatch modified its input or an unrelated buffer");
    SetShaderRegisters(queue, 0x240, UserData(inputAddress, secondAddress, input.size()));
    DispatchPm4(adapter, queue, ranges, 17, 0x8061);
    for (std::uint32_t lane = 0; lane < 17; ++lane) expectedSecond[lane * 4] = original[lane * 4] + 1u;
    Require(second == expectedSecond, "PM4 thread-count dispatch did not preserve exactly 17 active stores and all excluded bytes");
    Require(first == expectedFirst && input == original, "PM4 partial dispatch changed another borrowed buffer");
    bool oversizedRejected = false;
    try {
        DispatchPm4(adapter, queue, ranges, std::numeric_limits<std::uint32_t>::max(), 0x8061);
    } catch (const std::invalid_argument& error) {
        oversizedRejected = std::string(error.what()).find("grid exceeds the shader integer range") != std::string::npos;
        if (!oversizedRejected) throw;
    }
    Require(oversizedRejected, "PM4 UINT32_MAX thread-count dispatch silently wrapped to an empty grid");
    Require(input == original && first == expectedFirst && second == expectedSecond,
            "PM4 oversized grid rejection modified borrowed memory");
    SetShaderRegisters(queue, 0x240, UserData(inputAddress + 1u, secondAddress, input.size() - 1u));
    bool unalignedRejected = false;
    try {
        DispatchPm4(adapter, queue, ranges);
    } catch (const std::invalid_argument& error) {
        unalignedRejected = std::string(error.what()).find("view must be DWORD aligned") != std::string::npos;
        if (!unalignedRejected) throw;
    }
    Require(unalignedRejected, "PM4 descriptor replay accepted an unaligned guest buffer view");
    Require(input == original && first == expectedFirst && second == expectedSecond,
            "PM4 unaligned descriptor rejection modified borrowed memory");
    std::cout << "PM4 SET_SH_REG/DISPATCH_DIRECT: 64-lane actual RDNA descriptor execution and ordered live-SGPR rebound passed\n";
    std::cout << "PM4 thread-count dispatch: ceil-group conversion and 17-thread partial mask passed\n";
    std::cout << "PM4 oversized thread-count and unaligned descriptor rejection passed\n";
}

void Pm4BdaReplay(id<MTLDevice> device, bool writable) {
    constexpr std::uint64_t address = 0x400000;
    std::vector<std::uint32_t> memory(16384, 0xcdcdcdcd);
    for (std::uint32_t word = 0; word < 768; ++word) memory[word] = 0x51000000u + word * 0x00010203u;
    const auto initial = memory;
    auto expected = initial;
    for (std::uint32_t lane = 0; lane < 64; ++lane) {
        for (std::uint32_t component = 0; component < 4; ++component)
            expected[1024 + lane * 8 + component] = initial[512 + lane * 4 + component];
        expected[1024 + lane * 8 + 4] = initial[4 + lane];
        expected[1024 + lane * 8 + 5] = initial[448 + lane];
        expected[448 + lane] = initial[448 + lane] + lane * 16u;
    }
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 1> ranges{{
        {address, std::as_writable_bytes(std::span(memory)), writable}}};
    AgcDriver::QueueState queue;
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(address), 0};
    Configure(queue, 0x500000, userData);
    const std::array<std::uint32_t, 4> payload{1, 1, 1, 0x8041};
    const auto packet = Packet(0x15, payload);
    AgcDriver::Pm4::Validate(packet, 0x20);
    const auto state = AgcDriver::DecodeComputeDispatch(queue, packet);
    AgcDriver::Metal::MetalComputeDispatch adapter(device);
    const auto fault = adapter.DispatchSynchronously(state, VccBaseCode, ranges);
    if (writable) {
        Require(fault.state == ShaderRecompiler::BdaAbi::FaultState::Empty, "PM4 automatic BDA dispatch published a GPU fault");
        Require(memory == expected, "PM4 automatic BDA resource binding or dirty-page copy-back produced incorrect memory");
    } else {
        Require(fault.state == ShaderRecompiler::BdaAbi::FaultState::Ready &&
                fault.reason == ShaderRecompiler::BdaAbi::FaultReason::Permission,
                "PM4 automatic BDA dispatch did not return the original guest permission fault");
        Require(fault.address >= address && fault.address < address + memory.size() * 4u && fault.bytes == 4,
                "PM4 automatic BDA fault did not identify a four-byte access inside the borrowed range");
        Require(memory == initial, "PM4 BDA permission fault modified read-only borrowed memory");
    }
    std::cout << "PM4 actual RDNA BDA dispatch: " << (writable ? "automatic resource binding, atomics, and coherent copy-back" : "read-only permission fault") << " passed\n";
}

}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "PM4 compute replay requires a Metal 3 device");
            std::cout << "Native PM4 compute replay on " << device.name.UTF8String << '\n';
            Require(argc == 2 || (argc == 3 && std::string(argv[2]) == "publication-failure"),
                "PM4 replay requires its utility Metal library path and optional publication-failure mode");
            NSError* error = nil;
            auto url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
            id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
            Require(library != nil, error ? error.localizedDescription.UTF8String : "PM4 utility library failed to load");
            PublicationFailureReplay(device, library);
            if (argc == 3) return 0;
            OriginalSubmitExports(device, library);
            OriginalWave32Subgroup(device, library);
            RetainedSubgroupReplays(device, library);
            CheckedPm4Memory();
            Pm4DescriptorReplay(device);
            Pm4BdaReplay(device, true);
            Pm4BdaReplay(device, false);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
