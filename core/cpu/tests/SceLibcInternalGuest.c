typedef unsigned long size_t;
#define EXPORT __attribute__((visibility("protected")))
EXPORT volatile size_t SceInternalState[11];
EXPORT char SceInternalPrinted[64];
extern void __cxa_finalize(void*);
EXPORT void SceInternalTwin(void) {}

EXPORT int SceInternalInit(size_t argc, const void* argv, void* param) {
    (void)argc; (void)argv; (void)param; ++SceInternalState[0];
    return 0;
}
EXPORT int SceInternalFini(size_t argc, const void* argv, void* param) {
    (void)argc; (void)argv; (void)param; __cxa_finalize((void*)0); ++SceInternalState[1]; return 0;
}
EXPORT void* memcpy(void* destination, const void* source, size_t size) {
    if (SceInternalState[0] != 1) return (void*)0;
    ++SceInternalState[2];
    unsigned char* d = destination; const unsigned char* s = source;
    for (size_t i = 0; i < size; ++i) d[i] = s[i];
    return destination;
}
EXPORT void* memset(void* destination, int value, size_t size) {
    ++SceInternalState[3];
    unsigned char* d = destination;
    for (size_t i = 0; i < size; ++i) d[i] = (unsigned char)value;
    return destination;
}
EXPORT size_t strlen(const char* text) {
    ++SceInternalState[4]; size_t size = 0;
    while (text[size]) ++size;
    return size;
}
EXPORT int strcmp(const char* a, const char* b) {
    ++SceInternalState[5];
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
EXPORT int strncmp(const char* a, const char* b, size_t size) {
    ++SceInternalState[6];
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
EXPORT char* strncpy(char* destination, const char* source, size_t size) {
    ++SceInternalState[7]; size_t i = 0;
    for (; i < size && source[i]; ++i) destination[i] = source[i];
    for (; i < size; ++i) destination[i] = 0;
    return destination;
}

static void character(char* destination, size_t capacity, size_t* size, char value) {
    if (*size + 1 < capacity) destination[*size] = value;
    ++*size;
}
static void decimal(char* destination, size_t capacity, size_t* size, unsigned value) {
    char digits[10]; unsigned count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) character(destination, capacity, size, digits[--count]);
}
static int format_output(char* destination, size_t capacity, const char* format, __builtin_va_list args) {
    size_t size = 0;
    while (*format) {
        if (*format != '%') { character(destination, capacity, &size, *format++); continue; }
        ++format;
        if (*format == 's') {
            const char* text = __builtin_va_arg(args, const char*);
            while (*text) character(destination, capacity, &size, *text++);
            ++format;
        } else if (*format == 'u') {
            decimal(destination, capacity, &size, __builtin_va_arg(args, unsigned)); ++format;
        } else if (format[0] == '.' && format[1] == '1' && format[2] == 'f') {
            double value = __builtin_va_arg(args, double); unsigned whole = (unsigned)value;
            decimal(destination, capacity, &size, whole);
            character(destination, capacity, &size, '.');
            character(destination, capacity, &size, (char)('0' + (unsigned)((value - whole) * 10.0)));
            format += 3;
        } else { return -1; }
    }
    if (capacity) destination[size < capacity ? size : capacity - 1] = 0;
    return (int)size;
}
EXPORT int snprintf(char* destination, size_t capacity, const char* format, ...) {
    ++SceInternalState[8]; __builtin_va_list args; __builtin_va_start(args, format);
    int result = format_output(destination, capacity, format, args);
    __builtin_va_end(args); return result;
}
EXPORT int printf(const char* format, ...) {
    ++SceInternalState[9]; __builtin_va_list args; __builtin_va_start(args, format);
    int result = format_output(SceInternalPrinted, sizeof SceInternalPrinted, format, args);
    __builtin_va_end(args); return result;
}
EXPORT char* strstr(const char* text, const char* pattern) {
    ++SceInternalState[10];
    for (;;) {
        size_t i = 0;
        while (pattern[i] && text[i] && pattern[i] == text[i]) ++i;
        if (!pattern[i]) return (char*)text;
        if (!*text++) return (void*)0;
    }
}

/* Source fixtures implement guest services; the test checks forwarding, not
   conformance of these small models to the private libc implementation. */
struct Stats { unsigned short size, version; unsigned reserved;
    size_t max_system, current_system, max_inuse, current_inuse; };
struct Heap { unsigned char* base; size_t capacity, used, peak; int active; unsigned char* pointer; };
static struct Heap* heaps[2];
EXPORT size_t SceInternalExtra[8];
extern int* guest_error(void) __asm__("9BcDykPmo1I");
EXPORT void* sceLibcMspaceCreate(const char* name, void* base, size_t capacity, int flags) {
    if (!name || name[0] != 'h' || !base || ((size_t)base & 7) ||
        capacity < 0x5a1 || (capacity & 7) || (flags & ~5)) return (void*)0;
    for (unsigned i = 0; i < 2; ++i) if (!heaps[i] || !heaps[i]->active) {
        heaps[i] = base; *heaps[i] = (struct Heap){base, capacity, 0, 0, 1, (void*)0};
        ++SceInternalExtra[0]; return heaps[i];
    }
    return (void*)0;
}
static struct Heap* owner(void* handle) {
    for (unsigned i = 0; i < 2; ++i) if (heaps[i] && handle == heaps[i] && heaps[i]->active) return heaps[i];
    return (void*)0;
}
EXPORT void* sceLibcMspaceMalloc(void* handle, size_t size) {
    struct Heap* h = owner(handle);
    if (!h || h->used || !size || size > h->capacity - 256) return (void*)0;
    h->used = size; h->pointer = h->base + 128;
    if (size > h->peak) h->peak = size; return h->pointer;
}
EXPORT void* sceLibcMspaceRealloc(void* handle, void* pointer, size_t size) {
    struct Heap* h = owner(handle);
    if (!pointer) return sceLibcMspaceMalloc(handle, size);
    if (!h || pointer != h->pointer || !h->used || size > h->capacity - 256) return (void*)0;
    if (!size) { h->used = 0; h->pointer = (void*)0; return (void*)0; }
    if (size > h->used) {
        unsigned char* next = h->base + (h->pointer == h->base + 128 ? 256 : 128);
        for (size_t i = 0; i < h->used; ++i) next[i] = h->pointer[i];
        h->pointer = next;
    }
    h->used = size; if (size > h->peak) h->peak = size; return h->pointer;
}
EXPORT __attribute__((noreturn)) void abort(void) {
    __asm__ volatile("movl $0xa002000b, %%fs:0x28; int $0x45; ud2" ::: "memory");
    __builtin_unreachable();
}
EXPORT __attribute__((noreturn)) void __cxa_pure_virtual(void) {
    __asm__ volatile("movl $0xa002000a, %%fs:0x28; int $0x45; ud2" ::: "memory");
    __builtin_unreachable();
}
EXPORT int sceLibcMspaceFree(void* handle, void* pointer) {
    if (!pointer) return 0;
    struct Heap* h = owner(handle);
    if (!h || pointer != h->pointer || !h->used) abort();
    h->used = 0; h->pointer = (void*)0;
    ++SceInternalExtra[1]; return 0;
}
EXPORT int sceLibcMspaceMallocStats(void* handle, struct Stats* stats) {
    struct Heap* h = owner(handle);
    if (!h || !stats || stats->size != 40 || stats->version != 1) return 1;
    stats->max_system = stats->current_system = h->capacity;
    stats->max_inuse = h->peak; stats->current_inuse = h->used; return 0;
}
EXPORT int sceLibcMspaceDestroy(void* handle) {
    struct Heap* h = owner(handle); if (!h) return 1;
    h->active = 0; ++SceInternalExtra[2]; return 0;
}
struct Registration { void (*callback)(void*); void* object; void* dso; };
static struct Registration registrations[8];
static unsigned registration_count;
EXPORT int __cxa_atexit(void (*callback)(void*), void* object, void* dso) {
    if (registration_count == 8) return -1;
    registrations[registration_count++] = (struct Registration){callback, object, dso}; return 0;
}
EXPORT void __cxa_finalize(void* dso) {
    /* Snapshot the traversal; registrations made in a callback are left for a
       later invocation. Current entries are consumed before callback reentry. */
    for (unsigned i = registration_count; i; --i) {
        struct Registration* r = &registrations[i - 1];
        if (r->callback && (!dso || r->dso == dso)) {
            void (*callback)(void*) = r->callback; r->callback = (void*)0; callback(r->object);
        }
    }
}
static unsigned char new_arena[32];
static int new_live;
EXPORT void* _Znwm(size_t size) {
    if (new_live || !size || size > sizeof new_arena) return (void*)0;
    new_live = 1; SceInternalExtra[3] = 1; return new_arena;
}
EXPORT void _ZdlPv(void* pointer) {
    if (!pointer) return;
    if (pointer != new_arena || !new_live) abort();
    new_live = 0; SceInternalExtra[3] = 0; ++SceInternalExtra[4];
}
EXPORT char* strncat(char* destination, const char* source, size_t count) {
    size_t end = 0, i = 0; while (destination[end]) ++end;
    while (i < count && source[i]) { destination[end + i] = source[i]; ++i; }
    destination[end + i] = 0; return destination;
}
static int digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}
EXPORT size_t _Stoul(const char* text, char** end, int base) {
    const char* original = text; size_t value = 0; int negative = 0, overflow = 0, any = 0;
    if (base < 0 || base == 1 || base > 36) { if (end) *end = (char*)original; return 0; }
    while (*text == ' ' || *text == '\t') ++text;
    if (*text == '+' || *text == '-') negative = *text++ == '-';
    if ((!base || base == 16) && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
    if (!base) base = text[0] == '0' ? 8 : 10;
    for (;;) {
        int d = digit((unsigned char)*text); if (d < 0 || d >= base) break;
        if (value > (~(size_t)0 - (unsigned)d) / (unsigned)base) overflow = 1;
        value = value * (unsigned)base + (unsigned)d; any = 1; ++text;
    }
    if (end) *end = (char*)(any ? text : original);
    if (!any) return 0;
    if (overflow) { *guest_error() = 34; return ~(size_t)0; }
    return negative ? -value : value;
}
