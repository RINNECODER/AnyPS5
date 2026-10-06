#include "MetalTestSupport.hpp"
#include "MetalGuestMemory.hpp"

#include <algorithm>
#include <limits>

namespace {
namespace Abi = ShaderRecompiler::BdaAbi;
using GuestMemory = AgcDriver::Metal::MetalGuestMemory;

template<class Function>
void Reject(Function&& function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    MetalTests::Require(rejected, message);
}

id<MTLComputePipelineState> guestPipeline(const MetalTests::Context& context) {
    NSString* source = @R"metal(
#include <metal_stdlib>
using namespace metal;
struct Header { uint version, count, entryBytes, reserved; };
struct Range { ulong begin, end; device uchar* address; uint permissions, reserved; };
struct Parameters { ulong address, value; uint bytes, reserved; };
kernel void GuestWrite(device const Header* header [[buffer(0)]], device atomic_uint* fault [[buffer(1)]], constant Parameters& parameters [[buffer(2)]]) {
    if (header->version != 1 || header->entryBytes != 32 || header->reserved != 0) {
        atomic_store_explicit(fault + 1, 4u, memory_order_relaxed);
        atomic_store_explicit(fault, 2u, memory_order_relaxed);
        return;
    }
    device const Range* ranges = reinterpret_cast<device const Range*>(header + 1);
    for (uint i = 0; i < header->count; ++i) {
        if (parameters.address < ranges[i].begin || parameters.address >= ranges[i].end || parameters.bytes > ranges[i].end - parameters.address) continue;
        if ((ranges[i].permissions & 2u) == 0) {
            device uint* words = reinterpret_cast<device uint*>(fault);
            words[1] = 2; words[2] = uint(parameters.address); words[3] = uint(parameters.address >> 32); words[4] = parameters.bytes;
            atomic_store_explicit(fault, 2u, memory_order_relaxed);
            return;
        }
        device uchar* destination = ranges[i].address + parameters.address - ranges[i].begin;
        if (parameters.bytes == 8) *reinterpret_cast<device ulong*>(destination) = parameters.value;
        else *reinterpret_cast<device uint*>(destination) = uint(parameters.value);
        uint page = uint(parameters.address >> 12) + 1u;
        uint hash = (page * 0x9e3779b1u) >> 20;
        for (uint probe = 0; probe < 8; ++probe) {
            uint expected = 0;
            device atomic_uint* slot = fault + 9 + ((hash + probe) & 4095);
            if (atomic_compare_exchange_weak_explicit(slot, &expected, page, memory_order_relaxed, memory_order_relaxed) || expected == page) return;
        }
        atomic_store_explicit(fault + 8, 1u, memory_order_relaxed);
        return;
    }
    device uint* words = reinterpret_cast<device uint*>(fault);
    words[1] = 1; words[2] = uint(parameters.address); words[3] = uint(parameters.address >> 32); words[4] = parameters.bytes;
    atomic_store_explicit(fault, 2u, memory_order_relaxed);
}
)metal";
    NSError* error = nil;
    auto options = [[MTLCompileOptions alloc] init];
    options.languageVersion = MTLLanguageVersion3_0;
    auto library = [context.device newLibraryWithSource:source options:options error:&error];
    if (library == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Guest-memory MSL compilation failed");
    auto pipeline = [context.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"GuestWrite"] error:&error];
    if (pipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Guest-memory Metal pipeline creation failed");
    return pipeline;
}

id<MTLCommandBuffer> dispatch(const MetalTests::Context& context, GuestMemory::DispatchSnapshot& snapshot,
                             id<MTLComputePipelineState> pipeline, std::uint64_t address, std::uint64_t value, std::uint32_t bytes = 4) {
    struct Parameters { std::uint64_t address, value; std::uint32_t bytes, reserved; };
    const Parameters parameters{address, value, bytes, 0};
    auto commands = [context.queue commandBuffer];
    auto encoder = [commands computeCommandEncoder];
    MetalTests::Require(commands != nil && encoder != nil, "Guest-memory command allocation failed");
    snapshot.DeclareResources(encoder);
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:snapshot.Table() offset:0 atIndex:0];
    [encoder setBuffer:snapshot.FaultBuffer() offset:0 atIndex:1];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) throw std::runtime_error(commands.error.localizedDescription.UTF8String ?: "Guest-memory GPU dispatch failed");
    return commands;
}
}

void RunGuestMemoryTests(const MetalTests::Context& context) {
    std::vector<std::byte> host(8192 + 24, std::byte{0x31});
    std::array<std::byte, 32> readOnly;
    readOnly.fill(std::byte{0x72});
    constexpr std::uint64_t address = 0x401018;
    constexpr std::uint64_t readAddress = 0x200000;
    GuestMemory memory(context.device);
    memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(address, host, true);
    memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(readAddress, readOnly, false);
    Reject([&] { memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(address + 1, readOnly, true); }, "Overlapping guest span was accepted");
    Reject([&] { memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(std::numeric_limits<std::uint64_t>::max() - 4, readOnly, false); }, "Guest address overflow was accepted");
    Reject([&] { memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(std::uint64_t{std::numeric_limits<std::uint32_t>::max()} << Abi::WrittenPageShift, readOnly, true); }, "Unrepresentable writable guest page was accepted");
    Reject([&] { memory.RegisterBorrowedHostSpanUntilSnapshotsComplete(0x1000, {}, true); }, "Empty guest span was accepted");

    auto snapshot = memory.CaptureAfterPriorSnapshotsComplete();
    Abi::Header header{};
    std::memcpy(&header, snapshot.Table().contents, sizeof(header));
    MetalTests::Require(header.version == Abi::Version && header.count == 2 && header.entryBytes == sizeof(Abi::Range) && header.reserved == 0, "Guest table header is incompatible with BdaAbi");
    std::array<Abi::Range, 2> ranges;
    std::memcpy(ranges.data(), static_cast<const std::byte*>(snapshot.Table().contents) + sizeof(header), sizeof(ranges));
    auto buffers = snapshot.Buffers();
    MetalTests::Require(ranges[0].begin == readAddress && ranges[0].end == readAddress + readOnly.size() && ranges[0].permissions == Abi::Read && ranges[0].reserved == 0 && ranges[0].deviceAddress == buffers[0].gpuAddress,
                        "Read-only guest range is unsorted or has an invalid Metal address");
    const auto writableBufferOffset = (address - buffers[1].gpuAddress) & 15u;
    MetalTests::Require(ranges[1].begin == address && ranges[1].end == address + host.size() && ranges[1].permissions == (Abi::Read | Abi::Write) && ranges[1].reserved == 0 && ranges[1].deviceAddress == buffers[1].gpuAddress + writableBufferOffset,
                        "Writable guest range has an invalid Metal address or permissions");
    MetalTests::Require(std::memcmp(static_cast<const std::byte*>(buffers[1].contents) + writableBufferOffset, host.data(), host.size()) == 0, "Guest capture did not copy caller bytes into Metal memory");
    auto pipeline = guestPipeline(context);
    constexpr auto writeOffset = std::size_t{4128};
    constexpr std::uint32_t value = 0x9172a3b4;
    auto commands = dispatch(context, snapshot, pipeline, address + writeOffset, value);
    host[1] = std::byte{0x53};
    host.back() = std::byte{0x54};
    const auto report = snapshot.CompleteAndCopyDirtyPagesToBorrowedHost(commands);
    MetalTests::Require(report.state == Abi::FaultState::Empty, "Valid guest GPU write returned a fault");
    std::uint32_t observed = 0;
    std::memcpy(&observed, host.data() + writeOffset, sizeof(observed));
    MetalTests::Require(observed == value, "Metal GPU-address write was not copied to the correct caller offset");
    for (std::size_t i = 0; i < host.size(); ++i) {
        if (i >= writeOffset && i < writeOffset + sizeof(value)) continue;
        const auto expected = i == 1 ? std::byte{0x53} : i == host.size() - 1 ? std::byte{0x54} : std::byte{0x31};
        MetalTests::Require(host[i] == expected, "Dirty-page copy-back overwrote unrelated or clean-page bytes");
    }
    Reject([&] { (void)snapshot.CompleteAndCopyDirtyPagesToBorrowedHost(commands); }, "Snapshot allowed duplicate completion");
    memory.Unregister(address);
    Reject([&] { memory.Unregister(address); }, "Unregister accepted an absent range");

    auto denied = memory.CaptureAfterPriorSnapshotsComplete();
    auto deniedCommands = dispatch(context, denied, pipeline, readAddress, 0xffffffff);
    const auto deniedFault = denied.CompleteAndCopyDirtyPagesToBorrowedHost(deniedCommands);
    MetalTests::Require(deniedFault.state == Abi::FaultState::Ready && deniedFault.reason == Abi::FaultReason::Permission && deniedFault.address == readAddress && deniedFault.bytes == 4,
                        "Read-only GPU guest write did not return its ABI permission fault");
    MetalTests::Require(std::all_of(readOnly.begin(), readOnly.end(), [](std::byte byte) { return byte == std::byte{0x72}; }), "Read-only guest bytes were modified");

    auto unknown = memory.CaptureAfterPriorSnapshotsComplete();
    auto unknownCommands = dispatch(context, unknown, pipeline, 0x900000, 1);
    const auto unknownFault = unknown.CompleteAndCopyDirtyPagesToBorrowedHost(unknownCommands);
    MetalTests::Require(unknownFault.state == Abi::FaultState::Ready && unknownFault.reason == Abi::FaultReason::Unmapped && unknownFault.address == 0x900000 && unknownFault.bytes == 4,
                        "Unmapped GPU guest write did not return its ABI address fault");

    auto invalid = memory.CaptureAfterPriorSnapshotsComplete();
    auto pending = [context.queue commandBuffer];
    Reject([&] { (void)invalid.CompleteAndCopyDirtyPagesToBorrowedHost(pending); }, "Guest copy-back accepted an uncompleted command buffer");
    auto* words = static_cast<std::uint32_t*>(invalid.FaultBuffer().contents);
    words[Abi::WrittenOverflowWord] = 1;
    Reject([&] { (void)invalid.CompleteAndCopyDirtyPagesToBorrowedHost(unknownCommands); }, "Guest copy-back accepted tracker overflow");
    words[Abi::WrittenOverflowWord] = 0;
    words[Abi::WrittenSlotsWord] = static_cast<std::uint32_t>(readAddress >> Abi::WrittenPageShift) + 1;
    Reject([&] { (void)invalid.CompleteAndCopyDirtyPagesToBorrowedHost(unknownCommands); }, "Guest copy-back accepted a read-only dirty page");

    std::array<std::byte, 24> unalignedHost;
    unalignedHost.fill(std::byte{0x61});
    GuestMemory unalignedMemory(context.device);
    unalignedMemory.RegisterBorrowedHostSpanUntilSnapshotsComplete(0x1001, unalignedHost, true);
    auto unalignedSnapshot = unalignedMemory.CaptureAfterPriorSnapshotsComplete();
    Abi::Range unalignedRange;
    std::memcpy(&unalignedRange, static_cast<const std::byte*>(unalignedSnapshot.Table().contents) + sizeof(Abi::Header), sizeof(unalignedRange));
    MetalTests::Require((unalignedRange.deviceAddress + 0x1004 - unalignedRange.begin) % 4 == 0 &&
                        (unalignedRange.deviceAddress + 0x1008 - unalignedRange.begin) % 8 == 0 &&
                        (unalignedRange.deviceAddress + 0x1010 - unalignedRange.begin) % 16 == 0,
                        "Guest-address translation lost scalar or vector alignment");
    auto scalarCommands = dispatch(context, unalignedSnapshot, pipeline, 0x1004, 0x8231fe47);
    (void)unalignedSnapshot.CompleteAndCopyDirtyPagesToBorrowedHost(scalarCommands);
    auto wideSnapshot = unalignedMemory.CaptureAfterPriorSnapshotsComplete();
    auto wideCommands = dispatch(context, wideSnapshot, pipeline, 0x1008, 0x71f0a9b8432d8e56ull, 8);
    (void)wideSnapshot.CompleteAndCopyDirtyPagesToBorrowedHost(wideCommands);
    std::uint32_t scalar = 0;
    std::uint64_t wide = 0;
    std::memcpy(&scalar, unalignedHost.data() + 3, sizeof(scalar));
    std::memcpy(&wide, unalignedHost.data() + 7, sizeof(wide));
    MetalTests::Require(scalar == 0x8231fe47 && wide == 0x71f0a9b8432d8e56ull, "Aligned scalar/wide GPU writes from an unaligned guest span copied to incorrect bytes");
    for (std::size_t i = 0; i < unalignedHost.size(); ++i) {
        if (i >= 3 && i < 15) continue;
        MetalTests::Require(unalignedHost[i] == std::byte{0x61}, "Unaligned guest mirror padding leaked into caller bytes");
    }
}
