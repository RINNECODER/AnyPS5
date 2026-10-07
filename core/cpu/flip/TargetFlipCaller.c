#include <stdint.h>

// Synthetic SysV callers for the corroborated imported semantics. These are
// compiled guest inputs, not retail instructions or a claim of retail PM4 bytes.
struct Arguments { uint64_t value[8]; };
typedef uint64_t (*Flip)(void*, uint32_t, int32_t, uint32_t, int64_t);
typedef uint64_t (*Wait)(void*, uint32_t, uint32_t);

uint64_t flip_target_emit(void* function, const struct Arguments* a) {
    return ((Flip)function)((void*)a->value[0], (uint32_t)a->value[1],
        (int32_t)a->value[2], (uint32_t)a->value[3], (int64_t)a->value[4]);
}
uint64_t flip_target_wait(void* function, const struct Arguments* a) {
    return ((Wait)function)((void*)a->value[0], (uint32_t)a->value[1], (uint32_t)a->value[2]);
}
uint64_t flip_target_sequence(void* function, const struct Arguments* a) {
    ((Wait)(uintptr_t)a->value[5])((void*)a->value[0], (uint32_t)a->value[1], (uint32_t)a->value[2]);
    return ((Flip)function)((void*)a->value[0], (uint32_t)a->value[1],
        (int32_t)a->value[2], (uint32_t)a->value[3], (int64_t)a->value[4]);
}

struct Packet { uint64_t address; uint32_t words; uint8_t flags, reserved[3]; };
_Static_assert(sizeof(struct Packet) == 16, "Independent guest packet descriptor ABI");
typedef int32_t (*Submit)(const struct Packet*);
int32_t flip_target_submit(void* function, const struct Arguments* a) {
    // Unused descriptor padding is not required to be zero. The native adapter
    // must snapshot the descriptor and commands before this local is reused.
    volatile struct Packet packet = {a->value[0], (uint32_t)a->value[1],
        (uint8_t)a->value[2], {0x5a, 0xf1, 0xa5}};
    int32_t result = ((Submit)function)((const struct Packet*)&packet);
    packet.address = 0;
    packet.words = 0;
    packet.flags = 0xff;
    return result;
}
