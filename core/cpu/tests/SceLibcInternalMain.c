typedef unsigned long size_t;
#define EXPORT __attribute__((visibility("protected")))
extern void* memcpy(void*, const void*, size_t);
extern void* memset(void*, int, size_t);
extern size_t strlen(const char*);
extern int strcmp(const char*, const char*);
extern int strncmp(const char*, const char*, size_t);
extern char* strncpy(char*, const char*, size_t);
extern int snprintf(char*, size_t, const char*, ...);
extern int printf(const char*, ...);
extern char* strstr(const char*, const char*);
extern volatile size_t SceInternalState[11];
extern unsigned char SceInternalConsumerState[8];
EXPORT void* SceInternalAddresses[] = {memcpy, memset, strlen, strcmp, strncmp, strncpy, snprintf, printf, strstr};
EXPORT unsigned char SceInternalOutput[160];

EXPORT int SceInternalMain(void) {
    if (SceInternalState[0] != 1 || SceInternalState[1]) return 31;
    for (unsigned i = 0; i < 8; ++i) if (SceInternalConsumerState[i] != "orderok"[i]) return 43;
    for (unsigned i = 0; i < sizeof SceInternalOutput; ++i) SceInternalOutput[i] = 0xa7;
    if (memcpy(SceInternalOutput + 1, "native", 6) != SceInternalOutput + 1) return 32;
    if (memset(SceInternalOutput + 8, 0x134, 3) != SceInternalOutput + 8) return 33;
    if (strlen("guest") != 5 || strcmp("ab", "ac") >= 0 || strcmp("ac", "ab") <= 0) return 34;
    if (strncmp("abc", "abd", 2) || strncmp("abc", "abd", 3) >= 0 || strncmp("z", "a", 0)) return 35;
    if (strncpy((char*)SceInternalOutput + 16, "xy", 5) != (char*)SceInternalOutput + 16) return 36;
    if (strncpy((char*)SceInternalOutput + 24, "longer", 3) != (char*)SceInternalOutput + 24) return 37;
    const char* format = "%s:%u,%u,%u,%u,%u,%u,%u:%.1f";
    int size = snprintf((char*)SceInternalOutput + 40, 64, format, "guest", 11u, 22u, 33u, 44u, 55u, 66u, 77u, 3.5);
    if (size != 30) return 38;
    if (snprintf((char*)SceInternalOutput + 112, 5, format, "guest", 11u, 22u, 33u, 44u, 55u, 66u, 77u, 3.5) != size) return 39;
    if (snprintf((void*)0, 0, "%s:%u", "count", 19u) != 8) return 40;
    if (printf("print:%u", 91u) != 8) return 41;
    const char* text = "ababa!";
    if (strstr(text, "aba") != text || strstr(text, "ba!") != text + 3 || strstr(text, "zzz") || strstr(text, "") != text) return 42;
    return 0;
}
