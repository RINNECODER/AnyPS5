#include <atomic>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::GuestMemory {
void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable);
}

// The replay executables run guest memory as plain host memory with no write watch, so VideoOut's
// buffer-label store is a checked release store.
extern "C" void AgcDriverStoreGuestLabel_nid_postfix(std::uint64_t* pointer, std::uint64_t value) {
    AgcDriver::GuestMemory::CheckRange(pointer, sizeof(*pointer), std::atomic_ref<std::uint64_t>::required_alignment, true);
    std::atomic_ref(*pointer).store(value, std::memory_order_release);
}
