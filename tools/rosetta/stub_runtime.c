// Logging stubs for imports that no x86_64 library exports yet. The launcher
// (tools/rosetta/launcher, anyps5_rosetta_run) writes a stub dylib with one
// entry per symbol (movl $index, %eax; jmp *_aps5_stub_common) plus the name
// tables below, and injects it with DYLD_INSERT_LIBRARIES. That dylib loads
// this runtime (built as aps5_stub_runtime.dylib, linked with -undefined
// dynamic_lookup so the tables bind to the stub dylib by flat lookup). A stub
// logs its first call and returns 0 to the guest, so a run shows which NIDs a
// title needs next.
//
// The stub dylib also carries tracing stubs for `--trace` (movl $index,
// %r11d; jmp *_aps5_trace_common). The guest binds imports by flat lookup, and
// inserted libraries come first, so a guest call lands in the tracing stub. It
// logs the call (thread, time, first three arguments, and the bytes behind
// pointer arguments that hold a C string) and jumps to the next definition of
// the same symbol in another loaded image with every argument register intact.
// ANYPS5_TRACE_LIMIT caps the logged calls per symbol (default 16), and
// ANYPS5_TRACE_AFTER=SECONDS ignores calls made earlier, so a title's steady
// state can be traced without the start-up calls using up the limit.
// ANYPS5_TRACE_RETURNS=1 also logs what each logged call returns (%rax): the
// stub swaps the return address for a trampoline that logs and returns to the
// caller. A C++ exception unwinding through a hooked call cannot pass the
// trampoline, so this is opt-in.
#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern const char* const aps5_stub_names[];
extern const unsigned aps5_stub_count;
extern const char* const aps5_trace_symbols[];
extern const char* const aps5_trace_names[];
extern const unsigned aps5_trace_count;

static unsigned char* g_called;

static void emit(const char* line, int length, size_t capacity) {
    if (length > 0) (void)!write(STDERR_FILENO, line, (size_t)length < capacity ? (size_t)length : capacity - 1);
}

void aps5_stub_hit(unsigned index) {
    if (g_called == NULL) g_called = calloc(aps5_stub_count, 1);
    if (g_called == NULL || g_called[index]) return;
    g_called[index] = 1;
    char line[256];
    emit(line, snprintf(line, sizeof line, "[rosetta] unimplemented import %s\n", aps5_stub_names[index]), sizeof line);
}

// Copies a short printable C string from `address` without faulting on bad pointers.
static int describe_string(uint64_t address, char* out, size_t size) {
    if (address < 0x10000 || size < 4) return 0;
    char bytes[64];
    vm_size_t copied = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)address, sizeof bytes, (vm_address_t)bytes, &copied) != KERN_SUCCESS) {
        // The range may cross into an unmapped page: retry up to the page end.
        const vm_size_t rest = 4096 - (address & 4095);
        if (rest >= sizeof bytes || vm_read_overwrite(mach_task_self(), (vm_address_t)address, rest, (vm_address_t)bytes, &copied) != KERN_SUCCESS) return 0;
    }
    size_t length = 0;
    while (length < copied && bytes[length] != '\0') {
        if (!isprint((unsigned char)bytes[length])) return 0;
        ++length;
    }
    if (length < 3) return 0;
    if (length > size - 3) length = size - 3;
    snprintf(out, size, "\"%.*s\"", (int)length, bytes);
    return 1;
}

static int g_hle_fd = -2;
static pthread_mutex_t g_hle_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_hle_sequence = 0;

static void hle_log_open(void) {
    if (g_hle_fd != -2) return;
    const char* path = getenv("ANYPS5_HLE_LOG");
    if (path == NULL || *path == '\0') { g_hle_fd = -1; return; }
    g_hle_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (g_hle_fd >= 0) {
        uint32_t header[2] = { 0x474F4C48u, 1u };
        (void)!write(g_hle_fd, header, sizeof header);
    }
}

static uint64_t hle_log_call(const char* name, const uint64_t* args) {
    hle_log_open();
    if (g_hle_fd < 0) return 0;
    uint64_t thread = 0;
    pthread_threadid_np(NULL, &thread);
    pthread_mutex_lock(&g_hle_lock);
    const uint64_t sequence = ++g_hle_sequence;
    pthread_mutex_unlock(&g_hle_lock);
    const uint32_t name_length = (uint32_t)strlen(name);
    const uint32_t flags = 0;
    uint64_t args_copy[6] = {0, 0, 0, 0, 0, 0};
    const uint64_t result = 0;
    if (args != NULL) memcpy(args_copy, args, sizeof args_copy);
    pthread_mutex_lock(&g_hle_lock);
    (void)!write(g_hle_fd, &sequence, 8);
    (void)!write(g_hle_fd, &thread, 8);
    (void)!write(g_hle_fd, &name_length, 4);
    (void)!write(g_hle_fd, &flags, 4);
    (void)!write(g_hle_fd, args_copy, sizeof args_copy);
    (void)!write(g_hle_fd, &result, 8);
    (void)!write(g_hle_fd, name, name_length);
    static const uint8_t zeros[8] = {0};
    (void)!write(g_hle_fd, zeros, (8 - (name_length % 8)) % 8);
    pthread_mutex_unlock(&g_hle_lock);
    return sequence;
}

static void hle_log_return(uint64_t sequence, uint64_t result) {
    hle_log_open();
    if (g_hle_fd < 0 || sequence == 0) return;
    uint64_t thread = 0;
    pthread_threadid_np(NULL, &thread);
    const uint32_t name_length = 0;
    const uint32_t flags = 1;
    uint64_t args_copy[6] = {0, 0, 0, 0, 0, 0};
    pthread_mutex_lock(&g_hle_lock);
    (void)!write(g_hle_fd, &sequence, 8);
    (void)!write(g_hle_fd, &thread, 8);
    (void)!write(g_hle_fd, &name_length, 4);
    (void)!write(g_hle_fd, &flags, 4);
    (void)!write(g_hle_fd, args_copy, sizeof args_copy);
    (void)!write(g_hle_fd, &result, 8);
    pthread_mutex_unlock(&g_hle_lock);
}

static _Atomic(void*)* g_targets;
static _Atomic unsigned* g_trace_calls;
static unsigned g_trace_limit = 16;
static double g_trace_after = 0.0;
static pthread_once_t g_trace_once = PTHREAD_ONCE_INIT;
static struct timespec g_trace_start;
static int g_trace_returns;

// Return addresses replaced by _aps5_trace_return, innermost last, per thread.
struct pending_return {
    uint64_t address;
    unsigned index;
    unsigned call;
    uint64_t hle_sequence;
    int logged;
};
enum { kPendingReturns = 64 };
static __thread struct pending_return t_pending[kPendingReturns];
static __thread unsigned t_pending_depth;
extern char aps5_trace_return[];

static double trace_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - g_trace_start.tv_sec) + (double)(now.tv_nsec - g_trace_start.tv_nsec) / 1e9;
}

static void trace_init(void) {
    g_targets = calloc(aps5_trace_count ? aps5_trace_count : 1, sizeof *g_targets);
    g_trace_calls = calloc(aps5_trace_count ? aps5_trace_count : 1, sizeof *g_trace_calls);
    const char* limit = getenv("ANYPS5_TRACE_LIMIT");
    if (limit != NULL && *limit != '\0') g_trace_limit = (unsigned)strtoul(limit, NULL, 0);
    const char* after = getenv("ANYPS5_TRACE_AFTER");
    if (after != NULL && *after != '\0') g_trace_after = strtod(after, NULL);
    const char* returns = getenv("ANYPS5_TRACE_RETURNS");
    g_trace_returns = returns != NULL && *returns != '\0' && *returns != '0';
    const char* hle = getenv("ANYPS5_HLE_LOG");
    if (hle != NULL && *hle != '\0') g_trace_returns = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_trace_start);
}

// The definition of `name` in a loaded image other than this one. dlsym
// RTLD_NEXT does not reach the x86_64 libraries from an inserted library, so
// each image is asked in load order.
static void* next_definition(const char* name) {
    // Skip this runtime and the stub dylib that holds the tables (and the tracing stubs).
    Dl_info self, stubs;
    if (dladdr((const void*)next_definition, &self) == 0 || dladdr((const void*)aps5_trace_names, &stubs) == 0) return NULL;
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const void* header = _dyld_get_image_header(i);
        if (header == self.dli_fbase || header == stubs.dli_fbase) continue;
        void* image = dlopen(_dyld_get_image_name(i), RTLD_LAZY | RTLD_NOLOAD);
        if (image == NULL) continue;
        void* symbol = dlsym(image, name);
        dlclose(image);
        Dl_info owner;
        if (symbol != NULL && dladdr(symbol, &owner) != 0 && owner.dli_fbase != self.dli_fbase && owner.dli_fbase != stubs.dli_fbase)
            return symbol;
    }
    return NULL;
}

void* aps5_trace_hit(unsigned index, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t* return_slot) {
    pthread_once(&g_trace_once, trace_init);
    void* target = atomic_load_explicit(&g_targets[index], memory_order_acquire);
    if (target == NULL) {
        // Skip the leading underscore of the Mach-O name.
        target = next_definition(aps5_trace_symbols[index] + 1);
        if (target == NULL) {
            char line[256];
            emit(line, snprintf(line, sizeof line, "[trace] %s has no next definition; aborting\n", aps5_trace_names[index]), sizeof line);
            abort();
        }
        atomic_store_explicit(&g_targets[index], target, memory_order_release);
    }
    const double seconds = trace_seconds();
    uint64_t hle_sequence = 0;
    hle_log_open();
    if (g_hle_fd >= 0) {
        const uint64_t hle_args[6] = {a0, a1, a2, 0, 0, 0};
        hle_sequence = hle_log_call(aps5_trace_names[index], hle_args);
    }
    // The HLE log needs every call's return, ANYPS5_TRACE_RETURNS only the traced window's.
    // Either way this is opt-in: a C++ exception cannot unwind through the trampoline.
    int pending = -1;
    if ((g_trace_returns || hle_sequence != 0) && t_pending_depth < kPendingReturns) {
        pending = (int)t_pending_depth++;
        t_pending[pending] = (struct pending_return){*return_slot, index, 0, hle_sequence, 0};
        *return_slot = (uint64_t)(uintptr_t)aps5_trace_return;
    }
    // Calls before ANYPS5_TRACE_AFTER seconds are neither logged nor counted, so the
    // per-import limit applies to the window of interest (a title's steady state).
    if (seconds < g_trace_after) return target;
    const unsigned call = atomic_fetch_add_explicit(&g_trace_calls[index], 1, memory_order_relaxed) + 1;
    if (call <= g_trace_limit) {
        uint64_t thread = 0;
        pthread_threadid_np(NULL, &thread);
        const uint64_t args[3] = {a0, a1, a2};
        char text[3][80];
        for (int i = 0; i < 3; ++i) {
            if (!describe_string(args[i], text[i], sizeof text[i])) snprintf(text[i], sizeof text[i], "0x%llx", (unsigned long long)args[i]);
        }
        char line[512];
        emit(line, snprintf(line, sizeof line, "[trace] %.3fs tid=%llu #%u %s(%s, %s, %s)%s\n", seconds, (unsigned long long)thread, call,
                            aps5_trace_names[index], text[0], text[1], text[2], call == g_trace_limit ? " (limit reached)" : ""),
             sizeof line);
        if (pending >= 0) {
            t_pending[pending].call = call;
            t_pending[pending].logged = 1;
        }
    }
    return target;
}

// Called by _aps5_trace_return with the callee's %rax; returns the caller's address.
uint64_t aps5_trace_returned(uint64_t value) {
    const struct pending_return entry = t_pending[--t_pending_depth];
    hle_log_return(entry.hle_sequence, value);
    if (!entry.logged) return entry.address;
    uint64_t thread = 0;
    pthread_threadid_np(NULL, &thread);
    char line[256];
    emit(line, snprintf(line, sizeof line, "[trace] %.3fs tid=%llu #%u %s -> 0x%llx\n", trace_seconds(), (unsigned long long)thread,
                        entry.call, aps5_trace_names[entry.index], (unsigned long long)value),
         sizeof line);
    return entry.address;
}

__asm__(".text\n"
        ".globl _aps5_stub_common\n"
        "_aps5_stub_common:\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    andq $-16, %rsp\n"
        "    movl %eax, %edi\n"
        "    call _aps5_stub_hit\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        // %r11d = trace index. Keeps every argument register (and %al for
        // variadic callees), then tail-jumps to the real definition.
        ".globl _aps5_trace_common\n"
        "_aps5_trace_common:\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %rdx\n"
        "    pushq %rcx\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %rax\n"
        "    pushq %r10\n"
        "    andq $-16, %rsp\n"
        "    subq $128, %rsp\n"
        "    movdqu %xmm0, 0(%rsp)\n"
        "    movdqu %xmm1, 16(%rsp)\n"
        "    movdqu %xmm2, 32(%rsp)\n"
        "    movdqu %xmm3, 48(%rsp)\n"
        "    movdqu %xmm4, 64(%rsp)\n"
        "    movdqu %xmm5, 80(%rsp)\n"
        "    movdqu %xmm6, 96(%rsp)\n"
        "    movdqu %xmm7, 112(%rsp)\n"
        "    leaq 8(%rbp), %r8\n"
        "    movq %rdx, %rcx\n"
        "    movq %rsi, %rdx\n"
        "    movq %rdi, %rsi\n"
        "    movl %r11d, %edi\n"
        "    call _aps5_trace_hit\n"
        "    movq %rax, %r11\n"
        "    movdqu 0(%rsp), %xmm0\n"
        "    movdqu 16(%rsp), %xmm1\n"
        "    movdqu 32(%rsp), %xmm2\n"
        "    movdqu 48(%rsp), %xmm3\n"
        "    movdqu 64(%rsp), %xmm4\n"
        "    movdqu 80(%rsp), %xmm5\n"
        "    movdqu 96(%rsp), %xmm6\n"
        "    movdqu 112(%rsp), %xmm7\n"
        "    leaq -64(%rbp), %rsp\n"
        "    popq %r10\n"
        "    popq %rax\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rcx\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rbp\n"
        "    jmp *%r11\n"
        // A hooked call returns here: log %rax, then return to the real caller
        // through a slot in place of the consumed return address.
        ".globl _aps5_trace_return\n"
        "_aps5_trace_return:\n"
        "    subq $8, %rsp\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    pushq %rax\n"
        "    pushq %rdx\n"
        "    andq $-16, %rsp\n"
        "    subq $32, %rsp\n"
        "    movdqu %xmm0, 0(%rsp)\n"
        "    movdqu %xmm1, 16(%rsp)\n"
        "    movq %rax, %rdi\n"
        "    call _aps5_trace_returned\n"
        "    movq %rax, 8(%rbp)\n"
        "    movdqu 0(%rsp), %xmm0\n"
        "    movdqu 16(%rsp), %xmm1\n"
        "    leaq -16(%rbp), %rsp\n"
        "    popq %rdx\n"
        "    popq %rax\n"
        "    popq %rbp\n"
        "    ret\n");
