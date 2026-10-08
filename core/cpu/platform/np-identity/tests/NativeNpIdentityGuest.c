#include <stdint.h>

struct Invocation { uint64_t user, output, receipt; };
struct Receipt { uint64_t raw, signed_result, failed, marker; };
__attribute__((visibility("protected"))) void _start(struct Invocation*);
static void (*volatile linked_relative)(struct Invocation*) = _start;
__asm__(".type sceNpGetOnlineId,@function");

// Genuine undefined FUNC/PLT import. The guest consumes low EAX as the observed
// caller does; raw RAX separately records the provider's return ABI.
__attribute__((visibility("protected")))
void _start(struct Invocation* invocation) {
    uint64_t user = invocation->user, output = invocation->output, raw;
    if (linked_relative != _start) return;
    __asm__ volatile("call sceNpGetOnlineId@PLT"
        : "=a"(raw), "+D"(user), "+S"(output)
        : : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    volatile struct Receipt* receipt = (void*)invocation->receipt;
    const int32_t result = (int32_t)raw;
    receipt->raw = raw;
    receipt->signed_result = (uint64_t)(int64_t)result;
    receipt->failed = result != 0;
    receipt->marker = 0x4e504944454e5449ULL;
}
