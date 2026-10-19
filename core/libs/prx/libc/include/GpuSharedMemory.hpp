#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GPUSHAREDMEMORY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GPUSHAREDMEMORY_HPP

#include <cstddef>
#include <cstdint>

// Registers the guest memory a separate native arm64 process has to map (a GPU driver server) and
// publishes, for each region, the shm object and offset that backs it. Enabled by
// ANYPS5_GPU_SHARED_MEMORY=1; the manifest is written to the path in
// ANYPS5_GPU_SHARED_MEMORY_MANIFEST on demand and at process exit. See docs/dev/debt/libc.md.
namespace GpuSharedMemory {

inline constexpr const char* EnableEnvironment = "ANYPS5_GPU_SHARED_MEMORY";
inline constexpr const char* ManifestEnvironment = "ANYPS5_GPU_SHARED_MEMORY_MANIFEST";
inline constexpr const char* ObjectPrefix = "/aps5-gpu-";
inline constexpr std::size_t MaximumObjectName = 32;

extern "C" {

bool GpuSharedMemoryEnabled_nid_postfix();
// Creates a shm object of `bytes` named ObjectPrefix + <pid> + "-" + suffix, records it for the
// exit-time shm_unlink and writes its name (at most MaximumObjectName bytes, terminator included)
// into `name`. Returns the descriptor, or -1.
int GpuSharedMemoryCreateObject_nid_postfix(const char* suffix, std::uint64_t bytes, char* name, std::size_t nameCapacity);
// Records an already-created object for the exit-time shm_unlink, so a reader can open it by name.
void GpuSharedMemoryAdoptObject_nid_postfix(const char* name);
bool GpuSharedMemoryRegister_nid_postfix(std::uint64_t address, std::uint64_t bytes, const char* object, std::uint64_t offset);
void GpuSharedMemoryUnregister_nid_postfix(std::uint64_t address, std::uint64_t bytes);
std::size_t GpuSharedMemoryRegionCount_nid_postfix();
bool GpuSharedMemoryPublish_nid_postfix();

}

}

#endif
