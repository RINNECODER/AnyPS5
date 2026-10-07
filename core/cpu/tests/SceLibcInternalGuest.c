typedef unsigned long size_t;
#define EXPORT __attribute__((visibility("protected")))
EXPORT volatile size_t SceInternalState[11];
EXPORT char SceInternalPrinted[64];
EXPORT void SceInternalTwin(void) {}

EXPORT int SceInternalInit(size_t argc, const void* argv, void* param) {
    (void)argc; (void)argv; (void)param; ++SceInternalState[0]; return 0;
}
EXPORT int SceInternalFini(size_t argc, const void* argv, void* param) {
    (void)argc; (void)argv; (void)param; ++SceInternalState[1]; return 0;
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
