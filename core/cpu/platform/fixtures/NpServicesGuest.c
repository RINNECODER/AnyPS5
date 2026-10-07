// Build as freestanding x86-64 .text; all inputs are actual guest addresses.
typedef unsigned long long u64;
typedef int (*GetOnlineId)(unsigned, void*);
__attribute__((section(".text.entry")))
void NpServicesGuest(u64 gate, unsigned user, void* output, volatile u64* report) {
    report[0] = (u64)(long long)((GetOnlineId)gate)(user, output);
    report[1] = 0x4e504f46464c494eULL;
}
