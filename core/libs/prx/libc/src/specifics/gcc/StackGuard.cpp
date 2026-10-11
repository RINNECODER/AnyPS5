#include <cstdint>
#include <cstdlib>

std::uintptr_t __stack_chk_guard = 0xDEADBEEFCAFEBABEull;

#if defined(__APPLE__)
// Host code built with the stack protector (the prebuilt ffmpeg, libjpeg-turbo and
// LibAtrac9 archives) binds ___stack_chk_guard to libc.prx at link time. With the
// _nid_postfix twin the NID patcher keeps that plain name for the host libraries and
// exports the twin under the guest NID.
std::uintptr_t __stack_chk_guard_nid_postfix = 0xDEADBEEFCAFEBABEull;
#endif
