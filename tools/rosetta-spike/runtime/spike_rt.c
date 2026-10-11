// Minimal macOS runtime for a title relinked with `relinker --macos
// --to-rosetta`, for measurement only. It provides the hooks the relinker's
// Mach-O output binds to (guest start, TLS registration/allocation, module
// arguments) and, through generated stubs, every imported NID: a stub logs
// the call and returns 0. That is enough to see how far guest code gets when
// it runs directly under Rosetta and how long that takes.
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern const char* const aps5_stub_names[];
extern const unsigned aps5_stub_count;

static double g_t0;
static unsigned long long* g_calls;
static volatile unsigned g_last = ~0u;
static unsigned long long g_total;
static unsigned g_logged;
static unsigned g_log_limit = 400;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void summary(const char* why) {
    fprintf(stderr, "[spike] %s at %.3fs: %llu import calls, %u distinct\n", why, now() - g_t0, g_total, g_logged);
    for (int round = 0; round < 15; round++) {
        unsigned best = ~0u;
        for (unsigned i = 0; i < aps5_stub_count; i++)
            if (g_calls[i] && (best == ~0u || g_calls[i] > g_calls[best])) best = i;
        if (best == ~0u) break;
        fprintf(stderr, "[spike]   %10llu %s\n", g_calls[best], aps5_stub_names[best]);
        g_calls[best] = 0;
    }
}

static void on_signal(int sig, siginfo_t* info, void* context) {
    ucontext_t* uc = context;
    fprintf(stderr, "[spike] signal %d addr=%p rip=%#llx last_import=%s\n", sig, info->si_addr,
            (unsigned long long)uc->uc_mcontext->__ss.__rip, g_last == ~0u ? "-" : aps5_stub_names[g_last]);
    summary(sig == SIGALRM ? "time limit" : "fault");
    _exit(sig == SIGALRM ? 0 : 128 + sig);
}

__attribute__((constructor)) static void spike_init(void) {
    g_t0 = now();
    g_calls = calloc(aps5_stub_count, sizeof *g_calls);
    const char* limit = getenv("SPIKE_SECONDS");
    const char* logs = getenv("SPIKE_LOG_LIMIT");
    if (logs) g_log_limit = (unsigned)atoi(logs);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    alarm(limit ? (unsigned)atoi(limit) : 60);
    fprintf(stderr, "[spike] runtime loaded at %.3fs, %u import stubs\n", 0.0, aps5_stub_count);
}

// Called by every generated stub with its index; the guest's argument
// registers are not needed here, and the stub returns 0 to the guest.
void aps5_stub_hit(unsigned index) {
    g_last = index;
    g_total++;
    if (g_calls[index]++ == 0 && g_logged++ < g_log_limit)
        fprintf(stderr, "[spike] %.4fs first call #%u %s\n", now() - g_t0, g_logged, aps5_stub_names[index]);
}

// --- hooks the relinker binds by name -------------------------------------

struct TlsDescriptor {
    uint64_t Present, Image, FileSize, BlockSize, Alignment, Key;
};

void Aps5GuestTlsRegister_nid_no_patch(struct TlsDescriptor* d) {
    pthread_key_t key;
    pthread_key_create(&key, NULL);
    d->Key = key;
    fprintf(stderr, "[spike] %.4fs TLS register block=%llu key=%lu\n", now() - g_t0, (unsigned long long)d->BlockSize,
            (unsigned long)key);
}

// Called from spike_tls.S with every guest register saved; returns the
// FreeBSD variant II thread pointer (TLS block below, self pointer at 0,
// stack guard at 0x28).
void* aps5_tls_allocate(struct TlsDescriptor* d) {
    const uint64_t align = d->Alignment < 64 ? 64 : d->Alignment;
    const uint64_t below = (d->BlockSize + align - 1) & ~(align - 1);
    uint8_t* raw = NULL;
    posix_memalign((void**)&raw, align, below + 0x100);
    memset(raw, 0, below + 0x100);
    uint8_t* tp = raw + below;
    if (d->FileSize) memcpy(tp - d->BlockSize, (const void*)d->Image, d->FileSize);
    ((uint64_t*)tp)[0] = (uint64_t)tp;
    ((uint64_t*)tp)[5] = 0x2f8a6b4c19d3e705ull;
    pthread_setspecific((pthread_key_t)d->Key, tp);
    return tp;
}

static const uint64_t g_module_args[2] = {0, 0};
const void* __aps5_get_pending_module_args_nid_no_patch(void) { return g_module_args; }
void __aps5_set_module_init_result_nid_no_patch(int result) {
    fprintf(stderr, "[spike] %.4fs guest module init returned %d\n", now() - g_t0, result);
}

static void guest_exit(void) {
    fprintf(stderr, "[spike] guest exit handler\n");
}

void Aps5StartGuest_nid_no_patch(void (*entry)(void*, void (*)(void)), void* frame) {
    fprintf(stderr, "[spike] %.4fs entering guest entry %p\n", now() - g_t0, (void*)entry);
    entry(frame, guest_exit);
    summary("guest entry returned");
    _exit(0);
}
