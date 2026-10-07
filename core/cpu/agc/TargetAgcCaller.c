#include <stdint.h>

// Typed x86 SysV call matching the observed target's WRITE_DATA use. Compiled
// separately for the guest; no host-injected scalar marshalling in this path.
typedef uint64_t (*WriteData)(void*, uint8_t, uint8_t, uint64_t,
                             const void*, uint32_t, uint8_t, uint8_t);
uint64_t agc_target_write(WriteData write, void* buffer, uint64_t destination,
                          const void* data) {
    return write(buffer, 5, 0, destination, data, 64, 0, 1);
}

struct Packet {
    uint64_t address;
    uint32_t words;
    uint8_t flags;
    uint8_t reserved[3];
};
typedef int32_t (*Submit)(const struct Packet*);
int32_t agc_target_submit(Submit submit, uint64_t address, uint32_t words) {
    // Target stores only fields0/8/12. Poison the unused bytes so requiring zero
    // padding fails, and reuse the local descriptor immediately after return.
    volatile struct Packet packet = {address, words, 0, {0xa5, 0x5a, 0xf1}};
    int32_t result = submit((const struct Packet*)&packet);
    packet.address = 0;
    packet.words = 0;
    packet.flags = 0xff;
    return result;
}


typedef int32_t (*CreateShader)(uint64_t*, void*, const void*);
int32_t agc_target_shader(CreateShader create, uint64_t* output, void* header, const void* code) {
    return create(output, header, code);
}
typedef uint64_t (*Dispatch)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
uint64_t agc_target_dispatch(Dispatch dispatch, void* buffer, uint32_t modifier) {
    return dispatch(buffer, 1, 1, 1, modifier);
}

uint64_t agc_target_acb_write(WriteData write, void* buffer, uint64_t destination, const void* data) {
    return write(buffer, 2, 0, destination, data, 2, 0, 1);
}
