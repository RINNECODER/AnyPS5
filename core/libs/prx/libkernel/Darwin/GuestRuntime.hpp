#ifndef CORE_LIBS_PRX_LIBKERNEL_DARWIN_GUESTRUNTIME_HPP
#define CORE_LIBS_PRX_LIBKERNEL_DARWIN_GUESTRUNTIME_HPP

// Fork-only: macOS x86_64 (Rosetta) runtime for relinked titles. See GuestRuntime.cpp.
#if defined(__APPLE__) && defined(__x86_64__)

#include <cstddef>
#include <cstdint>

namespace Aps5Darwin {

// Written by the relinker into each image's __APS5DATA,__meta at 0x20
// (core/relinker/elfpatcher/src/macos/MacOsImage.cpp, MetaTlsDescriptor).
struct GuestTlsDescriptor {
    std::uint64_t Present;
    std::uint64_t Image;
    std::uint64_t FileSize;
    std::uint64_t BlockSize;
    std::uint64_t Alignment;
    std::uint64_t Key;
};
static_assert(sizeof(GuestTlsDescriptor) == 0x30, "relinker TLS descriptor layout");

void* AllocateGuestTls(GuestTlsDescriptor* descriptor);
void* GuestThreadPointer(GuestTlsDescriptor* descriptor);
std::size_t RegisteredTlsImages();
std::uint64_t LiveTlsBlocks();

}

extern "C" {
void Aps5GuestTlsRegister_nid_no_patch(Aps5Darwin::GuestTlsDescriptor* descriptor);
void* Aps5GuestTlsGetAddr_nid_no_patch(const std::uint64_t* index);
[[noreturn]] void Aps5StartGuest_nid_no_patch(int (*entry)(void*, void (*)()), const std::uint64_t* hostFrame);
}

#endif

#endif
