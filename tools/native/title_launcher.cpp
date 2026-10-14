// Minimal title launcher: builds the FreeBSD argc/argv frame the PS5 entry expects in rdi, the
// exit callback in rsi, the uniform register file's stack window, and calls the ELF entry.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/sysctl.h>
#include <csignal>
#include <initializer_list>
#include <exception>
#include <unistd.h>

struct RegFile {
    std::uint64_t gpr[16];
    unsigned __int128 xmm[16];
    unsigned __int128 ymm_hi[16];
    std::uint64_t flags;
    std::uint64_t flags_mask;
    double x87[8];
    std::uint64_t x87_top;
    std::uint64_t stack[32];
};

struct Frame {
    std::uint32_t argc;
    std::uint32_t reserved;
    const char* argv[64];
};

extern "C" void sub_80(void* rf);
extern "C" void NativeAotReportEntryCounts_nid_no_patch();
extern "C" const void* ApplicationProcessParameters_nid_no_patch();
extern "C" void ApplicationHeapInitialize_nid_no_patch(const void* processParameters);

static void OnFatal(int signal, siginfo_t* info, void*) {
    std::fprintf(stderr, "[aot] fatal signal %d at %p (code %d)\n", signal,
                 info != nullptr ? info->si_addr : nullptr, info != nullptr ? info->si_code : 0);
    NativeAotReportEntryCounts_nid_no_patch();
    _exit(128 + signal);
}

extern "C" [[noreturn]] void aot_guest_exit(int code) {
    std::fprintf(stderr, "[aot] guest exit(%d)\n", code);
    std::exit(code);
}

int main(int argc, char** argv) {
    int translated = 0;
    std::size_t bytes = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &bytes, nullptr, 0) != 0 && errno != ENOENT) return 90;
    if (translated != 0) return 91;
    std::fprintf(stderr, "architecture=arm64 translated=%d\n", translated);
    struct sigaction action{};
    action.sa_sigaction = OnFatal;
    action.sa_flags = SA_SIGINFO;
    for (int signal : {SIGTRAP, SIGSEGV, SIGBUS, SIGABRT, SIGILL}) sigaction(signal, &action, nullptr);
    static Frame frame{};
    frame.argc = static_cast<std::uint32_t>(argc);
    for (int index = 0; index < argc && index < 63; ++index) frame.argv[index] = argv[index];
    RegFile rf{};
    rf.gpr[7] = reinterpret_cast<std::uint64_t>(&frame);            // rdi: [0]=argc, [8]=argv[]
    rf.gpr[6] = reinterpret_cast<std::uint64_t>(&aot_guest_exit);   // rsi: exit callback
    rf.stack[0] = static_cast<std::uint64_t>(argc);
    for (int index = 0; index < argc && index + 1 < 32; ++index)
        rf.stack[1 + index] = reinterpret_cast<std::uint64_t>(argv[index]);
    // The image registered the guest process parameters from its constructor; initialise the
    // title's allocator here, once the runtime's own static initialisers have run.
    try { ApplicationHeapInitialize_nid_no_patch(ApplicationProcessParameters_nid_no_patch()); }
    catch (const std::exception& error) { std::fprintf(stderr, "[aot] heap initialization failed: %s\n", error.what()); }
    std::fprintf(stderr, "[aot] entering the title entry 0x80 (frame %p, argc %d)\n", (void*)&frame, argc);
    sub_80(&rf);
    std::fprintf(stderr, "[aot] the title entry returned\n");
    return 0;
}
