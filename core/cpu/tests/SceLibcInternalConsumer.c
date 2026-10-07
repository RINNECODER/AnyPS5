typedef unsigned long size_t;
#define EXPORT __attribute__((visibility("protected")))
extern void* memcpy(void*, const void*, size_t);
EXPORT unsigned char SceInternalConsumerState[8];
extern int __cxa_atexit(void (*)(void*), void*, void*);
extern void __cxa_finalize(void*);
static unsigned consumer_dso;
static void consumer_callback(void* object) { ((unsigned char*)object)[7] = 0x5a; }
EXPORT int SceInternalConsumerInit(size_t argc, const void* argv, void* param) {
    (void)argc; (void)argv; (void)param;
    if (memcpy(SceInternalConsumerState, "orderok", 8) != SceInternalConsumerState) return 49;
    return __cxa_atexit(consumer_callback, SceInternalConsumerState, &consumer_dso);
}

EXPORT int SceInternalConsumerFini(size_t argc, const void* argv, void* param) {
    (void)argc; (void)argv; (void)param; __cxa_finalize(&consumer_dso); return 0;
}
