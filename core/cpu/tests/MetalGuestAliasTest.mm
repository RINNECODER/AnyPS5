#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/Metal/PacketAutoreleasePool.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalGuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderResources.hpp"
#include <sys/mman.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

// #291: every synchronous draw/dispatch snapshotted all borrowed guest ranges (about 11 GB in a
// title boot) into fresh MTLBuffers. With MetalGuestHostAliases a packet binds persistent no-copy
// aliases of the guest backing instead. This drives the real per-packet bindings
// (MetalShaderResources) over a synthetic mmap'd guest arena, like the runtime's guest backing, and
// checks that a packet copies nothing proportional to guest memory and that CPU and GPU writes stay
// coherent across packets and across a remap of the backing.
namespace {
using namespace AgcDriver;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t WritableBytes = std::size_t{192} << 20;
constexpr std::size_t ReadOnlyBytes = std::size_t{64} << 20;
constexpr std::size_t ArenaBytes = WritableBytes + ReadOnlyBytes;
constexpr std::uint64_t WritableGuest = 0x10000000;
constexpr std::uint64_t ReadOnlyGuest = 0x40000000;
constexpr std::size_t ProbeOffset = (std::size_t{150} << 20) + 0x1230;
constexpr std::size_t WriteOffset = (std::size_t{100} << 20) + 0x40;
constexpr int Packets = 8;

std::byte* mapArena(std::byte* at = nullptr) {
    void* mapped = ::mmap(at, ArenaBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON | (at ? MAP_FIXED : 0), -1, 0);
    require(mapped != MAP_FAILED && (at == nullptr || mapped == at), "guest arena mapping failed");
    return static_cast<std::byte*>(mapped);
}

std::uint32_t load(const std::byte* bytes) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void store(std::byte* bytes, std::uint32_t value) { std::memcpy(bytes, &value, sizeof(value)); }

struct Harness {
    id<MTLDevice> device;
    Metal::MetalDevice backend;
    Metal::MetalGuestHostAliases aliases;
    std::byte* arena;
    id<MTLBuffer> scratch;
    id<MTLBuffer> constant;

    Harness(id<MTLDevice> device, id<MTLLibrary> library) : device(device), backend(device, library), aliases(device), arena(mapArena()) {
        std::memset(arena, 0x5a, ArenaBytes);
        scratch = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        constant = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        require(scratch != nil && constant != nil, "scratch buffer allocation failed");
        require(::mprotect(arena + WritableBytes, ReadOnlyBytes, PROT_READ) == 0, "read-only guest protection failed");
    }
    ~Harness() { ::munmap(arena, ArenaBytes); }

    std::array<NativeGuestMemory::BorrowedRange, 2> Ranges() const {
        return {NativeGuestMemory::BorrowedRange{WritableGuest, std::span(arena, WritableBytes), true},
            NativeGuestMemory::BorrowedRange{ReadOnlyGuest, std::span(arena + WritableBytes, ReadOnlyBytes), false}};
    }

    // A guest CPU store into the read-only view goes through the guest's own writable mapping.
    void GuestStoreReadOnly(std::size_t offset, std::uint32_t value) {
        require(::mprotect(arena + WritableBytes, ReadOnlyBytes, PROT_READ | PROT_WRITE) == 0, "guest store unprotect failed");
        store(arena + WritableBytes + offset, value);
        require(::mprotect(arena + WritableBytes, ReadOnlyBytes, PROT_READ) == 0, "guest store reprotect failed");
    }

    struct Result {
        std::uint64_t copied;
        std::uint32_t writable;
        std::uint32_t readOnly;
    };

    // One packet as the driver runs it: bind, encode (read two guest words, write one), commit,
    // wait, complete.
    Result Packet(std::span<const NativeGuestMemory::BorrowedRange> ranges, Metal::MetalGuestHostAliases* hostAliases,
        std::uint32_t gpuWrite) {
        Result result{};
        Metal::DrainPacketAutoreleases([&] {
            NativeGuestMemory::BorrowedRangesScope scope(ranges);
            Metal::MetalShaderResources bindings(backend, ranges, {}, hostAliases);
            result.copied = bindings.CopiedBytes();
            const auto writableRead = bindings.Buffer(WritableGuest + ProbeOffset, 4);
            const auto readOnlyRead = bindings.Buffer(ReadOnlyGuest + ProbeOffset - WritableBytes / 2, 4);
            const auto written = bindings.Buffer(WritableGuest + WriteOffset, 4, true);
            store(static_cast<std::byte*>(constant.contents), gpuWrite);
            id<MTLCommandBuffer> commands = backend.CommandBuffer();
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromBuffer:writableRead.buffer sourceOffset:writableRead.offset toBuffer:scratch destinationOffset:0 size:4];
            [blit copyFromBuffer:readOnlyRead.buffer sourceOffset:readOnlyRead.offset toBuffer:scratch destinationOffset:4 size:4];
            [blit copyFromBuffer:constant sourceOffset:0 toBuffer:written.buffer destinationOffset:written.offset size:4];
            [blit endEncoding];
            [commands commit];
            backend.Wait(commands);
            require(commands.status == MTLCommandBufferStatusCompleted, "packet command buffer failed");
            static_cast<void>(bindings.Complete(commands));
            result.writable = load(static_cast<const std::byte*>(scratch.contents));
            result.readOnly = load(static_cast<const std::byte*>(scratch.contents) + 4);
        });
        return result;
    }
};

constexpr std::size_t ReadOnlyProbe = ProbeOffset - WritableBytes / 2;

// Proves the counter: without aliases each packet copies all of guest memory.
void copiesWholeGuestWithoutAliases(Harness& harness) {
    const auto ranges = harness.Ranges();
    store(harness.arena + ProbeOffset, 0x11111111u);
    harness.GuestStoreReadOnly(ReadOnlyProbe, 0x22222222u);
    const auto result = harness.Packet(ranges, nullptr, 0x33333333u);
    require(result.copied == ArenaBytes, "control: snapshot copied " + std::to_string(result.copied) + " bytes, not the whole guest");
    require(result.writable == 0x11111111u && result.readOnly == 0x22222222u, "control: packet did not read the guest words");
    require(load(harness.arena + WriteOffset) == 0x33333333u, "control: GPU write did not reach the guest");
}

void aliasedPacketsCopyNothingAndStayCoherent(Harness& harness) {
    const auto ranges = harness.Ranges();
    const auto baseline = harness.device.currentAllocatedSize;
    std::uint64_t peak = baseline;
    for (int i = 0; i < Packets; ++i) {
        // Guest CPU stores between packets must be visible to the next packet.
        store(harness.arena + ProbeOffset, 0xa0000000u + static_cast<std::uint32_t>(i));
        harness.GuestStoreReadOnly(ReadOnlyProbe, 0xb0000000u + static_cast<std::uint32_t>(i));
        const auto gpuWrite = 0xc0000000u + static_cast<std::uint32_t>(i);
        const auto result = harness.Packet(ranges, &harness.aliases, gpuWrite);
        require(result.copied == 0, "aliased packet " + std::to_string(i) + " copied " + std::to_string(result.copied) + " guest bytes");
        require(result.writable == 0xa0000000u + static_cast<std::uint32_t>(i), "aliased packet missed a CPU store to writable guest memory");
        require(result.readOnly == 0xb0000000u + static_cast<std::uint32_t>(i), "aliased packet missed a CPU store to read-only guest memory");
        // GPU writes must be visible to the guest once the packet completes.
        require(load(harness.arena + WriteOffset) == gpuWrite, "aliased GPU write is not visible to the guest");
        peak = std::max<std::uint64_t>(peak, harness.device.currentAllocatedSize);
    }
    // The aliases count once toward the device (they are the guest pages), not once per packet.
    require(peak - baseline <= ArenaBytes + (std::size_t{16} << 20),
        "aliased packets grew device allocation by " + std::to_string((peak - baseline) >> 20) + " MiB");
    require(harness.aliases.Count() == 2, "aliases were not kept across packets (" + std::to_string(harness.aliases.Count()) + ")");
    std::cerr << "[guest-alias] " << Packets << " packets over " << (ArenaBytes >> 20) << " MiB guest: copied 0 bytes, device allocation grew "
              << ((peak - baseline) >> 10) << " KiB\n";
}

// A publication that remaps the backing at the same host address drops the aliases, so the
// next packet sees the new pages instead of the old ones.
void remappedBackingIsReboundAfterInvalidate(Harness& harness) {
    const auto ranges = harness.Ranges();
    static_cast<void>(harness.Packet(ranges, &harness.aliases, 1));
    harness.aliases.Invalidate(harness.arena, WritableBytes);
    require(harness.aliases.Count() == 1, "invalidate did not drop the remapped alias");
    mapArena(harness.arena);
    std::memset(harness.arena, 0x6b, ArenaBytes);
    require(::mprotect(harness.arena + WritableBytes, ReadOnlyBytes, PROT_READ) == 0, "read-only guest protection failed");
    harness.aliases.Invalidate(harness.arena + WritableBytes, ReadOnlyBytes);
    store(harness.arena + ProbeOffset, 0xd0d0d0d0u);
    const auto result = harness.Packet(ranges, &harness.aliases, 0xe0e0e0e0u);
    require(result.copied == 0, "remapped packet copied guest bytes");
    require(result.writable == 0xd0d0d0d0u && result.readOnly == 0x6b6b6b6bu, "packet read the pages of the replaced backing");
    require(load(harness.arena + WriteOffset) == 0xe0e0e0e0u, "GPU write did not reach the remapped backing");
}

// A span whose guest and host addresses disagree modulo 16 cannot alias; it falls back to the copy.
void misalignedSpanFallsBackToCopy(Harness& harness) {
    constexpr std::size_t Bytes = std::size_t{1} << 20;
    const std::array ranges{NativeGuestMemory::BorrowedRange{WritableGuest + 8, std::span(harness.arena, Bytes), true}};
    store(harness.arena + 0x100, 0x77777777u);
    Metal::MetalGuestHostAliases aliases(harness.device);
    std::uint64_t copied = 0;
    std::uint32_t read = 0;
    Metal::DrainPacketAutoreleases([&] {
        NativeGuestMemory::BorrowedRangesScope scope(ranges);
        Metal::MetalShaderResources bindings(harness.backend, ranges, {}, &aliases);
        copied = bindings.CopiedBytes();
        const auto binding = bindings.Buffer(WritableGuest + 8 + 0x100, 4);
        read = load(static_cast<const std::byte*>(binding.buffer.contents) + binding.offset);
    });
    require(copied == Bytes && read == 0x77777777u, "misaligned span did not fall back to a correct copy");
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            require(argc == 2, "usage: anyps5_metal_guest_alias_test <utility metallib>");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "no Metal device");
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
            require(library != nil, "utility metallib failed to load");
            Harness harness(device, library);
            copiesWholeGuestWithoutAliases(harness);
            aliasedPacketsCopyNothingAndStayCoherent(harness);
            remappedBackingIsReboundAfterInvalidate(harness);
            misalignedSpanFallsBackToCopy(harness);
            std::cerr << "[guest-alias] passed\n";
            return 0;
        } catch (const std::exception& failure) {
            std::cerr << "[guest-alias] " << failure.what() << '\n';
            return 1;
        }
    }
}
