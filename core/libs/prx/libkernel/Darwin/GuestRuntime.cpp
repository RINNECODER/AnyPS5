// Fork-only runtime for titles relinked with `relinker --macos --to-rosetta`
// (docs/design/rosetta-direct-execution.md, phase 2). Built into libkernel.prx
// only by the x86_64 Rosetta libs mode (cmake/RosettaLibs.cmake).
//
// The relinker's Mach-O output binds these hooks by name:
//   Aps5StartGuest        - called from LC_MAIN with the guest entry point and the
//                           argc/argv/envp frame the entry stub built.
//   Aps5GuestTlsRegister  - called by each image's __mod_init_func with its TLS
//                           descriptor; assigns the pthread key the %fs stubs read.
//   Aps5GuestTlsAllocate  - called by a %fs stub on a thread's first access to an
//                           image's TLS (descriptor in %rax, every other register
//                           live); returns the thread pointer in %rax.
//   Aps5GuestTlsGetAddr   - __tls_get_addr for guest modules: the module id the
//                           relinker writes is the image's TLS descriptor.
// Static TLS uses FreeBSD variant II: the image's block sits directly below the
// thread pointer, which holds a self pointer at 0 and the stack guard at 0x28.
// Every image has its own thread pointer, keyed per image, and blocks are
// allocated lazily, so threads from scePthreadCreate, pthread_create or host
// libraries all get guest TLS for every image, including modules dlopen'd later.
#if defined(__APPLE__) && defined(__x86_64__)

#include "prx/libkernel/Darwin/GuestRuntime.hpp"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using Aps5Darwin::GuestTlsDescriptor;

constexpr std::size_t ThreadControlBytes = 0x100;
constexpr std::size_t RawBlockSlot = 0x80 / 8;
constexpr std::uint64_t StackGuard = 0x2f8a6b4c19d3e705ull;

std::mutex registryLock;
std::vector<GuestTlsDescriptor*> registered;
std::atomic<std::uint64_t> allocatedBlocks{0};
const auto startTime = std::chrono::steady_clock::now();

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
}

void Write(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Write(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0) (void)!::write(STDERR_FILENO, line, std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(line) - 1));
}

[[noreturn]] void Fatal(const char* what) {
    Write("[rosetta] fatal: %s\n", what);
    std::_Exit(134);
}

void FreeBlock(void* threadPointer) {
    if (threadPointer == nullptr) return;
    std::free(static_cast<void**>(threadPointer)[RawBlockSlot]);
    allocatedBlocks.fetch_sub(1, std::memory_order_relaxed);
}

bool Registered(const GuestTlsDescriptor* descriptor) {
    return std::find(registered.begin(), registered.end(), descriptor) != registered.end();
}

}

namespace Aps5Darwin {

void* AllocateGuestTls(GuestTlsDescriptor* descriptor) {
    if (descriptor == nullptr || descriptor->Present == 0) Fatal("guest TLS access for an image without a TLS block");
    {
        std::lock_guard lock(registryLock);
        if (!Registered(descriptor)) Fatal("guest TLS access before the image registered its TLS descriptor");
    }
    const std::uint64_t alignment = std::max<std::uint64_t>(descriptor->Alignment, 64);
    const std::uint64_t below = (descriptor->BlockSize + alignment - 1) & ~(alignment - 1);
    void* raw = nullptr;
    if (::posix_memalign(&raw, alignment, below + ThreadControlBytes) != 0) Fatal("cannot allocate a guest TLS block");
    std::memset(raw, 0, below + ThreadControlBytes);
    auto* threadPointer = static_cast<std::uint8_t*>(raw) + below;
    if (descriptor->FileSize != 0)
        std::memcpy(threadPointer - descriptor->BlockSize, reinterpret_cast<const void*>(descriptor->Image), descriptor->FileSize);
    auto* control = reinterpret_cast<std::uint64_t*>(threadPointer);
    control[0] = reinterpret_cast<std::uint64_t>(threadPointer);
    control[5] = StackGuard;
    reinterpret_cast<void**>(threadPointer)[RawBlockSlot] = raw;
    if (::pthread_setspecific(static_cast<pthread_key_t>(descriptor->Key), threadPointer) != 0) Fatal("cannot publish a guest thread pointer");
    allocatedBlocks.fetch_add(1, std::memory_order_relaxed);
    return threadPointer;
}

void* GuestThreadPointer(GuestTlsDescriptor* descriptor) {
    void* threadPointer = ::pthread_getspecific(static_cast<pthread_key_t>(descriptor->Key));
    return threadPointer != nullptr ? threadPointer : AllocateGuestTls(descriptor);
}

std::size_t RegisteredTlsImages() {
    std::lock_guard lock(registryLock);
    return registered.size();
}

std::uint64_t LiveTlsBlocks() {
    return allocatedBlocks.load(std::memory_order_relaxed);
}

}

extern "C" {

void Aps5GuestTlsRegister_nid_no_patch(GuestTlsDescriptor* descriptor) {
    if (descriptor == nullptr || descriptor->Present == 0) return;
    std::lock_guard lock(registryLock);
    if (Registered(descriptor)) return;
    pthread_key_t key{};
    if (::pthread_key_create(&key, FreeBlock) != 0) Fatal("out of pthread keys for guest TLS images");
    descriptor->Key = key;
    registered.push_back(descriptor);
}

// Called from the assembly thunk below with every guest register saved.
void* Aps5GuestTlsAllocateSlow_nid_no_patch(GuestTlsDescriptor* descriptor) {
    return Aps5Darwin::AllocateGuestTls(descriptor);
}

void* Aps5GuestTlsGetAddr_nid_no_patch(const std::uint64_t* index) {
    auto* descriptor = reinterpret_cast<GuestTlsDescriptor*>(index[0]);
    if (descriptor == nullptr) Fatal("__tls_get_addr with a null module");
    auto* threadPointer = static_cast<std::uint8_t*>(Aps5Darwin::GuestThreadPointer(descriptor));
    return threadPointer - descriptor->BlockSize + index[1];
}

}

// The %fs stub has pushed flags (and %rax when it is live) and skipped the red
// zone; the descriptor is in %rax and every other register belongs to the guest.
asm(".text\n"
    ".globl _Aps5GuestTlsAllocate_nid_no_patch\n"
    "_Aps5GuestTlsAllocate_nid_no_patch:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %rdx\n"
    "    pushq %rcx\n"
    "    pushq %r8\n"
    "    pushq %r9\n"
    "    pushq %r10\n"
    "    pushq %r11\n"
    "    andq $-64, %rsp\n"
    "    subq $512, %rsp\n"
    "    fxsave64 (%rsp)\n"
    "    movq %rax, %rdi\n"
    "    call _Aps5GuestTlsAllocateSlow_nid_no_patch\n"
    "    fxrstor64 (%rsp)\n"
    "    leaq -64(%rbp), %rsp\n"
    "    popq %r11\n"
    "    popq %r10\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %rcx\n"
    "    popq %rdx\n"
    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbp\n"
    "    ret\n");

// ---- start-up -------------------------------------------------------------

namespace {

constexpr std::uint64_t AuxPageSize = 6;
constexpr std::uint64_t AuxNull = 0;
constexpr std::uint64_t GuestPageSize = 0x4000;
constexpr char GuestProgramPath[] = "/app0/eboot.bin";

std::vector<std::uint64_t> guestFrame;
std::vector<std::string> guestArguments;

void GuestExitCallback() {}

// ---- fault diagnostics ----------------------------------------------------

struct sigaction previousActions[32];
constexpr int DiagnosedSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};

const char* SignalName(int signal) {
    switch (signal) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    default: return "signal";
    }
}

bool ReadWord(std::uint64_t address, std::uint64_t* value) {
    mach_vm_size_t read = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, sizeof(*value), reinterpret_cast<mach_vm_address_t>(value), &read) == KERN_SUCCESS && read == sizeof(*value);
}

// "image+offset (guest vaddr) symbol" for a code address. Relinked images carry
// the guest load bias in __APS5DATA,__meta at offset 8.
void Describe(std::uint64_t address, char* out, std::size_t size) {
    Dl_info info{};
    if (address == 0 || !::dladdr(reinterpret_cast<void*>(address), &info) || info.dli_fbase == nullptr) {
        std::snprintf(out, size, "?");
        return;
    }
    const char* image = info.dli_fname != nullptr ? info.dli_fname : "?";
    if (const char* slash = std::strrchr(image, '/')) image = slash + 1;
    const auto base = reinterpret_cast<std::uint64_t>(info.dli_fbase);
    unsigned long metaSize = 0;
    const auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
    const auto* meta = reinterpret_cast<const std::uint64_t*>(getsectiondata(header, "__APS5DATA", "__meta", &metaSize));
    char guest[48] = "";
    // The load bias is rebased by dyld, so it already includes the image's slide.
    if (meta != nullptr && metaSize >= 16 && address >= meta[1])
        std::snprintf(guest, sizeof(guest), " guest=%#llx", static_cast<unsigned long long>(address - meta[1]));
    std::snprintf(out, size, "%s+%#llx%s%s%s", image, static_cast<unsigned long long>(address - base), guest,
        info.dli_sname != nullptr ? " " : "", info.dli_sname != nullptr ? info.dli_sname : "");
}

void Diagnose(int signal, siginfo_t* info, ucontext_t* context) {
    const auto& state = context->uc_mcontext->__ss;
    char where[256];
    Describe(state.__rip, where, sizeof(where));
    Write("[rosetta] guest fault: %s code=%d addr=%#llx rip=%#llx in %s at %.3fs\n", SignalName(signal), info->si_code,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info->si_addr)), static_cast<unsigned long long>(state.__rip), where, Seconds());
    Write("[rosetta]   rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx\n", state.__rax, state.__rbx, state.__rcx, state.__rdx, state.__rsi, state.__rdi);
    Write("[rosetta]   rbp=%#llx rsp=%#llx r8=%#llx r9=%#llx r10=%#llx r11=%#llx\n", state.__rbp, state.__rsp, state.__r8, state.__r9, state.__r10, state.__r11);
    Write("[rosetta]   r12=%#llx r13=%#llx r14=%#llx r15=%#llx rflags=%#llx\n", state.__r12, state.__r13, state.__r14, state.__r15, state.__rflags);
    std::uint64_t returnAddress = 0;
    if (ReadWord(state.__rsp, &returnAddress)) {
        Describe(returnAddress, where, sizeof(where));
        Write("[rosetta]   [rsp] %#llx %s\n", static_cast<unsigned long long>(returnAddress), where);
    }
    std::uint64_t frame = state.__rbp;
    for (int depth = 0; depth < 16 && frame != 0; ++depth) {
        std::uint64_t next = 0;
        if (!ReadWord(frame, &next) || !ReadWord(frame + 8, &returnAddress) || returnAddress == 0) break;
        Describe(returnAddress, where, sizeof(where));
        Write("[rosetta]   #%d %#llx %s\n", depth, static_cast<unsigned long long>(returnAddress), where);
        if (next <= frame) break;
        frame = next;
    }
}

void OnFault(int signal, siginfo_t* info, void* raw) {
    auto* context = static_cast<ucontext_t*>(raw);
    const auto& previous = previousActions[signal];
    // Chain first (the libc write watch resolves its own faults); a handler that
    // gives up restores SIG_DFL, which marks the fault as unhandled.
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal, info, raw);
        struct sigaction now {};
        if (::sigaction(signal, nullptr, &now) == 0 && now.sa_handler != SIG_DFL) return;
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    Diagnose(signal, info, context);
    std::_Exit(128 + signal);
}

void InstallFaultDiagnostics() {
    if (std::getenv("APS5_ROSETTA_NO_FAULT_REPORT") != nullptr) return;
    struct sigaction action {};
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int signal : DiagnosedSignals) {
        struct sigaction current {};
        if (::sigaction(signal, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == OnFault) continue;
        ::sigaction(signal, &action, &previousActions[signal]);
    }
    static thread_local bool stack = [] {
        stack_t alternate{};
        alternate.ss_size = 1 << 16;
        alternate.ss_sp = std::malloc(alternate.ss_size);
        return alternate.ss_sp != nullptr && ::sigaltstack(&alternate, nullptr) == 0;
    }();
    (void)stack;
}

}

// Installed when libkernel loads, so faults in guest module initialisers (run by
// dyld before Aps5StartGuest) are reported too; Aps5StartGuest re-checks.
__attribute__((constructor)) static void InstallFaultDiagnosticsAtLoad() {
    InstallFaultDiagnostics();
}

extern "C" {

int Aps5RosettaFaultDiagnosticsInstalled_nid_no_patch() {
    struct sigaction current {};
    return ::sigaction(SIGSEGV, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == OnFault;
}

[[noreturn]] void Aps5StartGuest_nid_no_patch(int (*entry)(void*, void (*)()), const std::uint64_t* hostFrame) {
    InstallFaultDiagnostics();
    // FreeBSD process frame: argc, argv[], NULL, envp[] (empty), NULL, auxv.
    const auto argc = hostFrame != nullptr ? hostFrame[0] : 0;
    guestArguments.emplace_back(GuestProgramPath);
    for (std::uint64_t index = 1; index < argc; ++index)
        guestArguments.emplace_back(reinterpret_cast<const char*>(hostFrame[1 + index]));
    guestFrame.push_back(guestArguments.size());
    for (const auto& argument : guestArguments) guestFrame.push_back(reinterpret_cast<std::uint64_t>(argument.c_str()));
    guestFrame.push_back(0);
    guestFrame.push_back(0);
    guestFrame.insert(guestFrame.end(), {AuxPageSize, GuestPageSize, AuxNull, 0});
    if (std::getenv("APS5_ROSETTA_QUIET") == nullptr)
        Write("[rosetta] guest entry %#llx after %.3fs of image start-up, %zu TLS images\n",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(entry)), Seconds(), Aps5Darwin::RegisteredTlsImages());
    const int result = entry(guestFrame.data(), GuestExitCallback);
    if (std::getenv("APS5_ROSETTA_QUIET") == nullptr) Write("[rosetta] guest entry returned %d at %.3fs\n", result, Seconds());
    std::exit(result);
}

}

#endif
