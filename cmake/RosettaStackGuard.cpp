// Prebuilt third-party code (ffmpeg, zlib, libjpeg-turbo) still references the
// host stack protector's guard. libc.prx exports the guest's __stack_chk_guard,
// which the NID patcher renames, so every other library gets a private copy.
#include <cstdint>

extern "C" __attribute__((visibility("hidden"))) uintptr_t __stack_chk_guard = static_cast<uintptr_t>(0x2f3d9a1c5b7e8d41ull);
