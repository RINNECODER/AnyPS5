#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/Metal/PacketAutoreleasePool.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalShaderResources.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

// A long boot died with SIGKILL and no crash report once the title started
// drawing: every synchronous draw/dispatch snapshots all borrowed guest ranges
// into fresh MTLBuffers, and the autoreleased command buffer kept each snapshot
// alive until the whole submission ended. This drives the real per-packet
// snapshot (MetalShaderResources) over a synthetic multi-MiB range and checks
// that a packet's snapshot is released when the packet ends.
namespace {
using namespace AgcDriver;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t RangeBytes = std::size_t{64} << 20;
constexpr std::uint64_t GuestAddress = 0x10000000;
constexpr int Packets = 6;

struct PageFree {
    void operator()(std::byte* bytes) const { std::free(bytes); }
};

struct Harness {
    id<MTLDevice> device;
    Metal::MetalDevice backend;
    std::unique_ptr<std::byte, PageFree> guest;
    id<MTLBuffer> scratch;

    Harness(id<MTLDevice> device, id<MTLLibrary> library) : device(device), backend(device, library),
        guest(static_cast<std::byte*>(std::aligned_alloc(16384, RangeBytes))) {
        require(guest != nullptr, "guest range allocation failed");
        std::memset(guest.get(), 0x5a, RangeBytes);
        scratch = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        require(scratch != nil, "scratch buffer allocation failed");
    }

    // One packet as the driver runs it: snapshot, encode, commit, wait, complete.
    void Packet() {
        const NativeGuestMemory::BorrowedRange range{GuestAddress, std::span(guest.get(), RangeBytes), false};
        Metal::MetalShaderResources bindings(backend, std::span(&range, 1));
        id<MTLCommandBuffer> commands = backend.CommandBuffer();
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        NSUInteger offset = 0;
        for (id<MTLResource> resource : bindings.Residency()) {
            if (![resource conformsToProtocol:@protocol(MTLBuffer)]) continue;
            [blit copyFromBuffer:(id<MTLBuffer>)resource sourceOffset:0 toBuffer:scratch destinationOffset:offset size:4];
            offset += 4;
        }
        [blit endEncoding];
        [commands commit];
        backend.Wait(commands);
        static_cast<void>(bindings.Complete(commands));
        std::uint32_t copied = 0;
        std::memcpy(&copied, static_cast<const std::byte*>(scratch.contents) + 8, sizeof(copied));
        require(offset == 12 && copied == 0x5a5a5a5au, "packet did not read the snapshot of the guest range");
    }

    std::uint64_t Allocated() const { return device.currentAllocatedSize; }
};

// Proves the detector: without a per-packet pool the snapshots accumulate.
void submissionPoolAloneAccumulates(Harness& harness) {
    std::uint64_t baseline = 0, peak = 0;
    @autoreleasepool {
        baseline = harness.Allocated();
        for (int i = 0; i < Packets; ++i) {
            harness.Packet();
            peak = std::max(peak, harness.Allocated());
        }
    }
    require(peak - baseline >= (Packets - 1) * RangeBytes,
        "control: snapshots were released without a per-packet pool (" + std::to_string(peak - baseline) + " bytes)");
    std::cerr << "[packet-autorelease] control retained " << ((peak - baseline) >> 20) << " MiB over " << Packets << " packets\n";
}

void perPacketPoolReleasesSnapshots(Harness& harness) {
    std::uint64_t baseline = 0, peak = 0;
    @autoreleasepool {
        baseline = harness.Allocated();
        for (int i = 0; i < Packets; ++i) {
            Metal::DrainPacketAutoreleases([&] { harness.Packet(); });
            peak = std::max(peak, harness.Allocated());
        }
    }
    require(peak - baseline < 2 * RangeBytes,
        "per-packet pool kept " + std::to_string((peak - baseline) >> 20) + " MiB of snapshots alive");
}

void throwingPacketStillDrains(Harness& harness) {
    std::uint64_t baseline = 0, peak = 0;
    @autoreleasepool {
        baseline = harness.Allocated();
        for (int i = 0; i < Packets; ++i) {
            bool caught = false;
            try {
                Metal::DrainPacketAutoreleases([&] {
                    harness.Packet();
                    throw std::invalid_argument("skipped packet");
                });
            } catch (const std::invalid_argument& error) {
                caught = std::string(error.what()) == "skipped packet";
            }
            require(caught, "packet exception type or message changed");
            peak = std::max(peak, harness.Allocated());
        }
    }
    require(peak - baseline < 2 * RangeBytes,
        "throwing packets kept " + std::to_string((peak - baseline) >> 20) + " MiB of snapshots alive");
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            require(argc == 2, "usage: anyps5_metal_packet_autorelease_test <utility metallib>");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "Metal device unavailable");
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
            require(library != nil, "utility metallib load failed");
            Harness harness(device, library);
            submissionPoolAloneAccumulates(harness);
            perPacketPoolReleasesSnapshots(harness);
            throwingPacketStillDrains(harness);
            std::cout << "PASS: per-packet autorelease pool releases each packet's guest snapshot\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
}
