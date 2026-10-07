typedef unsigned long size_t;
#define EXPORT __attribute__((visibility("protected")))
extern void* memcpy(void*, const void*, size_t);
EXPORT unsigned char SceInternalConsumerState[8];
EXPORT int SceInternalConsumerInit(size_t argc, const void* argv, void* param) {
    (void)argc; (void)argv; (void)param;
    if (memcpy(SceInternalConsumerState, "orderok", 8) != SceInternalConsumerState) return 49;
    return 0;
}
