typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

static u8 composite[4096];
static u8 pixels[4096];
static u64 tls[2];

static long service(u64 number, u64 a, u64 b, u64 c) {
    register u64 rax __asm__("rax") = number;
    register u64 rdi __asm__("rdi") = a;
    register u64 rsi __asm__("rsi") = b;
    register u64 rdx __asm__("rdx") = c;
    __asm__ volatile("syscall" : "+a"(rax) : "D"(rdi), "S"(rsi), "d"(rdx) : "rcx", "r11", "memory");
    return (long)rax;
}

static void text(const char* value) {
    u64 size = 0;
    while (value[size]) ++size;
    service(1, 1, (u64)value, size);
}

static void decimal(u64 value) {
    char buffer[21];
    u64 size = 0;
    do { buffer[20 - size++] = '0' + value % 10; value /= 10; } while (value);
    service(1, 1, (u64)(buffer + 21 - size), size);
}

static u32 crc32(const u8* bytes, u64 size) {
    u32 crc = ~0u;
    for (u64 i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int GuestMain(u64 argc, char** argv) {
    u64 limit = 1000;
    if (argc > 1) {
        limit = 0;
        for (const char* p = argv[1]; *p; ++p) {
            if (*p < '0' || *p > '9') return 2;
            limit = limit * 10 + (unsigned)(*p - '0');
            if (limit >= sizeof composite) return 2;
        }
        if (limit < 2) return 2;
    }
    for (u64 i = 0; i < sizeof composite; ++i) if (composite[i]) return 3;
    tls[0] = 0xdecafbad12345678ul;
    if (service(158, 0x1002, (u64)tls, 0)) return 4;
    u64 value;
    __asm__ volatile("movq %%fs:0,%0" : "=r"(value));
    if (value != tls[0]) return 5;
    if (service(158, 0x1003, (u64)&tls[1], 0) || tls[1] != (u64)tls) return 6;
    u64 count = 0, sum = 0;
    for (u64 i = 2; i <= limit; ++i) {
        if (composite[i]) continue;
        ++count;
        sum += i;
        for (u64 j = i * i; j <= limit; j += i) composite[j] = 1;
    }
    for (u64 i = 0; i < sizeof pixels; ++i) pixels[i] = (u8)((i * 37 + (i >> 3)) ^ (i >> 5));
    text("homebrew primes="); decimal(count);
    text(" sum="); decimal(sum);
    text(" buffer_crc32="); decimal(crc32(pixels, sizeof pixels));
    text(" tls=ok bss=ok\n");
    return 0;
}
