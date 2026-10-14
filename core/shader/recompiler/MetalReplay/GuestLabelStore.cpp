#include <atomic>
#include <cstdint>

// The replay executables run guest memory as plain host memory with no write watch, so VideoOut's
// buffer-label store is a plain release store. It is not range-checked: the only caller passes a
// slot of VideoOut's own host-side label array, which is never part of the borrowed guest ranges a
// replay registers, so the borrowed-range check would reject every call ("not borrowed").
extern "C" void AgcDriverStoreGuestLabel_nid_postfix(std::uint64_t* pointer, std::uint64_t value) {
    std::atomic_ref(*pointer).store(value, std::memory_order_release);
}
