// Fork-only runtime for titles relinked with `relinker --macos --to-rosetta`
// (docs/design/rosetta-direct-execution.md, phase 2). Built into libkernel.prx
// only by the x86_64 Rosetta libs mode (cmake/RosettaLibs.cmake).
//
// The relinker's Mach-O output binds these hooks by name:
//   Aps5StartGuest        - called from LC_MAIN with the guest entry point and the
//                           argc/argv/envp frame the entry stub built. When the title
//                           loads libSceVideoOut, the guest runs on its own thread (with
//                           the main thread's stack size) and the main thread becomes
//                           the AppKit application (Aps5RunHostApplication), so the game
//                           window and its input run on the main queue.
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
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
int (*guestEntry)(void*, void (*)()) = nullptr;
std::size_t guestStackBytes = 0;

void GuestExitCallback() {}

// ---- fault diagnostics ----------------------------------------------------

struct sigaction previousActions[32];
// SIGABRT gives uncaught host exceptions (std::terminate) a backtrace to the throw site.
constexpr int DiagnosedSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};

const char* SignalName(int signal) {
    switch (signal) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
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

// ---- indirect branch tracing (ANYPS5_TRACE_INDIRECT, Phase 1) --------------
//
// The opt-in relinker rewrites each computed `call r/m64` / `jmp r/m64` opcode
// byte to `int3` and writes the site list to <ANYPS5_TRACE_INDIRECT>.sites. The
// SIGTRAP handler below emulates the branch from the saved context and appends a
// fixed 24-byte record (from, to, kind, pad), kind 0=call 1=jmp, for targets that
// land in the main image's executable range. It never allocates or logs from the
// hot path beyond the record write.

struct IndirectTraceSite {
    std::uint64_t OpcodeAddress = 0;
    std::uint32_t PrefixLength = 0;
    std::uint32_t Length = 0;
    std::uint8_t Bytes[16] = {};
};

struct IndirectTraceState {
    std::atomic<bool> enabled{false};
    int fd = -1;
    int entryFd = -1;
    int pageFd = -1;
    std::uint64_t pagesWritten = 0;
    std::uint64_t seenEntries[4096] = {};
    std::size_t seenEntryCount = 0;
    std::uint64_t limit = 2000000;
    double afterSeconds = 0.0;
    double startSeconds = 0.0;
    double stopSeconds = 0.0;
    std::string triggerPath;
    std::size_t pageSize = 0;
    std::uint64_t written = 0;
    std::uint64_t guestBase = 0;
    std::uint64_t codeMin = 0;
    std::uint64_t codeMax = 0;
    std::vector<IndirectTraceSite> sites;
    std::vector<std::uint64_t> pages;
};

IndirectTraceState indirectTrace;

const IndirectTraceSite* FindIndirectSite(const std::uint64_t guestOpcode) {
    const auto found = std::lower_bound(indirectTrace.sites.begin(), indirectTrace.sites.end(), guestOpcode,
        [](const IndirectTraceSite& site, const std::uint64_t value) { return site.OpcodeAddress < value; });
    return found != indirectTrace.sites.end() && found->OpcodeAddress == guestOpcode ? &*found : nullptr;
}

// Restores the original instruction bytes (patched=false) or writes `int3` plus
// one-byte NOPs (patched=true). Only the pages that hold sites are made writable,
// and they are put back to read/execute. Used to run the load untraced and patch
// only at the trace window.
void ApplySitePatch(const bool patched) {
    if (indirectTrace.sites.empty() || indirectTrace.pages.empty()) return;
    for (const std::uint64_t page : indirectTrace.pages) {
        if (::mprotect(reinterpret_cast<void*>(page), indirectTrace.pageSize, PROT_READ | PROT_WRITE) != 0) {
            Write("[rosetta] indirect trace: mprotect(rw) failed for %#llx: %d %s\n", static_cast<unsigned long long>(page), errno, std::strerror(errno));
            return;
        }
    }
    for (const auto& site : indirectTrace.sites) {
        auto* code = reinterpret_cast<std::uint8_t*>(indirectTrace.guestBase + site.OpcodeAddress);
        const std::uint32_t count = site.Length > site.PrefixLength ? site.Length - site.PrefixLength : 0;
        if (patched) {
            code[0] = 0xCC;
            for (std::uint32_t index = 1; index < count; ++index) code[index] = 0x90;
        } else {
            for (std::uint32_t index = 0; index < count; ++index) code[index] = site.Bytes[site.PrefixLength + index];
        }
    }
    for (const std::uint64_t page : indirectTrace.pages)
        (void)::mprotect(reinterpret_cast<void*>(page), indirectTrace.pageSize, PROT_READ | PROT_EXEC);
}

void* TraceWindowThread(void*) {
    const auto base = std::chrono::steady_clock::now();
    const auto elapsed = [&base] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - base).count(); };
    if (indirectTrace.startSeconds > 0.0) {
        while (elapsed() < indirectTrace.startSeconds) ::usleep(50000);
    } else {
        while (::access(indirectTrace.triggerPath.c_str(), F_OK) != 0) ::usleep(100000);
    }
    ApplySitePatch(true);
    indirectTrace.enabled = true;
    Write("[rosetta] indirect trace: tracing started after %.3fs\n", elapsed());
    if (indirectTrace.stopSeconds > 0.0) {
        const auto stopBase = std::chrono::steady_clock::now();
        const auto stopElapsed = [&stopBase] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - stopBase).count(); };
        while (stopElapsed() < indirectTrace.stopSeconds) ::usleep(50000);
        ApplySitePatch(false);
        indirectTrace.enabled = false;
        Write("[rosetta] indirect trace: tracing stopped after %.3fs\n", stopElapsed());
    }
    return nullptr;
}

bool ReadMemory(std::uint64_t address, void* buffer, std::size_t size) {
    mach_vm_size_t read = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, reinterpret_cast<mach_vm_address_t>(buffer), &read) == KERN_SUCCESS && read == size;
}

bool WriteMemory(std::uint64_t address, const void* buffer, std::size_t size) {
    return mach_vm_write(mach_task_self(), address, reinterpret_cast<vm_offset_t>(const_cast<void*>(buffer)), static_cast<mach_msg_type_number_t>(size)) == KERN_SUCCESS;
}

std::uint64_t MainImageGuestBase() {
    const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    if (header == nullptr) return 0;
    unsigned long size = 0;
    const auto* meta = reinterpret_cast<const std::uint64_t*>(getsectiondata(header, "__APS5DATA", "__meta", &size));
    return meta != nullptr && size >= 16 ? meta[1] : 0;
}

void MainImageCodeRange(std::uint64_t* begin, std::uint64_t* end) {
    *begin = 0;
    *end = 0;
    const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    if (header == nullptr) return;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(header);
    const auto* cursor = reinterpret_cast<const std::uint8_t*>(header) + sizeof(mach_header_64);
    for (std::uint32_t index = 0; index < header->ncmds; ++index) {
        const auto* command = reinterpret_cast<const load_command*>(cursor);
        if (command->cmd == LC_SEGMENT_64) {
            const auto* segment = reinterpret_cast<const segment_command_64*>(command);
            if (std::strncmp(segment->segname, "__TEXT", sizeof(segment->segname)) == 0) {
                *begin = base;
                *end = base + segment->vmsize;
                return;
            }
        }
        cursor += command->cmdsize;
    }
}

void InitializeIndirectTrace() {
    const char* path = std::getenv("ANYPS5_TRACE_INDIRECT");
    if (path == nullptr || *path == '\0') return;
    indirectTrace.guestBase = MainImageGuestBase();
    std::uint64_t hostBegin = 0;
    std::uint64_t hostEnd = 0;
    MainImageCodeRange(&hostBegin, &hostEnd);
    indirectTrace.codeMin = hostBegin - indirectTrace.guestBase;
    indirectTrace.codeMax = hostEnd - indirectTrace.guestBase;
    if (const char* limit = std::getenv("ANYPS5_TRACE_INDIRECT_LIMIT")) indirectTrace.limit = std::strtoull(limit, nullptr, 0);
    if (const char* after = std::getenv("ANYPS5_TRACE_INDIRECT_AFTER")) indirectTrace.afterSeconds = std::strtod(after, nullptr);

    const std::string sitesPath = std::string(path) + ".sites";
    std::ifstream stream(sitesPath, std::ios::binary);
    std::uint32_t magic = 0;
    std::uint32_t count = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    if (stream.read(reinterpret_cast<char*>(&magic), sizeof(magic)) && magic == 0x49355041
        && stream.read(reinterpret_cast<char*>(&count), sizeof(count)) && count <= (1u << 24)
        && stream.read(reinterpret_cast<char*>(&begin), sizeof(begin))
        && stream.read(reinterpret_cast<char*>(&end), sizeof(end))) {
        constexpr std::size_t RecordBytes = 32;
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(count) * RecordBytes);
        if (stream.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
            indirectTrace.sites.resize(count);
            for (std::size_t index = 0; index < count; ++index) {
                const std::uint8_t* record = raw.data() + index * RecordBytes;
                std::memcpy(&indirectTrace.sites[index].OpcodeAddress, record, 8);
                std::memcpy(&indirectTrace.sites[index].PrefixLength, record + 8, 4);
                std::memcpy(&indirectTrace.sites[index].Length, record + 12, 4);
                std::memcpy(indirectTrace.sites[index].Bytes, record + 16, 16);
            }
            std::sort(indirectTrace.sites.begin(), indirectTrace.sites.end(),
                [](const IndirectTraceSite& left, const IndirectTraceSite& right) { return left.OpcodeAddress < right.OpcodeAddress; });
            indirectTrace.codeMin = begin;
            indirectTrace.codeMax = end;
        }
    } else {
        Write("[rosetta] indirect trace: no usable site list at %s; tracing without validation\n", sitesPath.c_str());
        std::uint64_t hostBegin = 0;
        std::uint64_t hostEnd = 0;
        MainImageCodeRange(&hostBegin, &hostEnd);
        indirectTrace.codeMin = 0;
        indirectTrace.codeMax = hostEnd > indirectTrace.guestBase ? hostEnd - indirectTrace.guestBase : 0;
    }
    indirectTrace.fd = ::open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    indirectTrace.entryFd = ::open((std::string(path) + ".entries").c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    indirectTrace.pageFd = ::open((std::string(path) + ".pages").c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (indirectTrace.fd < 0) {
        Write("[rosetta] indirect trace: cannot open %s\n", path);
        return;
    }
    if (const char* start = std::getenv("ANYPS5_TRACE_INDIRECT_START")) indirectTrace.startSeconds = std::strtod(start, nullptr);
    if (const char* stop = std::getenv("ANYPS5_TRACE_INDIRECT_STOP")) indirectTrace.stopSeconds = std::strtod(stop, nullptr);
    if (const char* trigger = std::getenv("ANYPS5_TRACE_INDIRECT_TRIGGER")) indirectTrace.triggerPath = trigger;
    Write("[rosetta] indirect trace: window start=%.1f stop=%.1f trigger=%s\n", indirectTrace.startSeconds, indirectTrace.stopSeconds, indirectTrace.triggerPath.c_str());
    const bool window = indirectTrace.startSeconds > 0.0 || !indirectTrace.triggerPath.empty();
    if (window) {
        indirectTrace.pageSize = static_cast<std::size_t>(::getpagesize());
        for (const auto& site : indirectTrace.sites) {
            const std::uint64_t address = indirectTrace.guestBase + site.OpcodeAddress;
            indirectTrace.pages.push_back(address & ~(static_cast<std::uint64_t>(indirectTrace.pageSize) - 1));
        }
        std::sort(indirectTrace.pages.begin(), indirectTrace.pages.end());
        indirectTrace.pages.erase(std::unique(indirectTrace.pages.begin(), indirectTrace.pages.end()), indirectTrace.pages.end());
        // The relinker patched the image; put the original bytes back so the load runs untraced.
        ApplySitePatch(false);
        indirectTrace.enabled = false;
        pthread_t thread{};
        if (::pthread_create(&thread, nullptr, TraceWindowThread, nullptr) != 0) {
            Write("[rosetta] indirect trace: cannot start the trace-window thread; tracing from the start\n");
            ApplySitePatch(true);
            indirectTrace.enabled = true;
        } else {
            ::pthread_detach(thread);
        }
    } else {
        indirectTrace.enabled = true;
    }
    Write("[rosetta] indirect trace: %zu trapped branches, limit %llu, guest base %#llx%s -> %s\n",
        indirectTrace.sites.size(), static_cast<unsigned long long>(indirectTrace.limit),
        static_cast<unsigned long long>(indirectTrace.guestBase), window ? " (trace window)" : "", path);
}

std::uint64_t RegisterValue(const ucontext_t* context, const unsigned index) {
    const auto& state = context->uc_mcontext->__ss;
    switch (index) {
    case 0: return state.__rax;
    case 1: return state.__rcx;
    case 2: return state.__rdx;
    case 3: return state.__rbx;
    case 4: return state.__rsp;
    case 5: return state.__rbp;
    case 6: return state.__rsi;
    case 7: return state.__rdi;
    case 8: return state.__r8;
    case 9: return state.__r9;
    case 10: return state.__r10;
    case 11: return state.__r11;
    case 12: return state.__r12;
    case 13: return state.__r13;
    case 14: return state.__r14;
    default: return state.__r15;
    }
}

void AppendIndirectRecord(const std::uint64_t from, const std::uint64_t to, const std::uint32_t kind) {
    if (indirectTrace.written >= indirectTrace.limit) return;
    struct Record {
        std::uint64_t From;
        std::uint64_t To;
        std::uint32_t Kind;
        std::uint32_t Pad;
    } record{from, to, kind, 0};
    (void)!::write(indirectTrace.fd, &record, sizeof(record));
    ++indirectTrace.written;
}

// One function-entry record: the entry address and the 16 general-purpose
// registers at entry, in the harness order (rax rbx rcx rdx rsi rdi rbp rsp
// r8..r15).
// ---- whole-process writable snapshot (ANYPS5_TRACE_SNAPSHOT) -----------------
//
// At ANYPS5_TRACE_SNAPSHOT_AT seconds the runtime dumps every writable region of
// the process (guest heap, stacks, data, TLS and the runtime's own) to
// <path>.snapshot as flat (base, size, data) records, then records hot-function
// entries for ANYPS5_TRACE_SNAPSHOT_WINDOW seconds so the replay can use the same
// memory for entries hit just after the snapshot.

struct SnapshotState {
    bool enabled = false;
    std::atomic<bool> active{false};
    double atSeconds = 60.0;
    double windowSeconds = 1.0;
    std::string path;
    std::uint64_t bytes = 0;
};

SnapshotState snapshotState;

void DumpSnapshot() {
    const int fd = ::open(snapshotState.path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return;
    const std::uint32_t magic = 0x534e4150;  // 'PANS'
    (void)!::write(fd, &magic, sizeof(magic));
    constexpr std::uint64_t Limit = 2ull << 30;
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_port_t object = MACH_PORT_NULL;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    while (snapshotState.bytes < Limit && mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                                          reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS) {
        if (object != MACH_PORT_NULL) { mach_port_deallocate(mach_task_self(), object); object = MACH_PORT_NULL; }
        count = VM_REGION_BASIC_INFO_COUNT_64;
        if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) && size != 0 && size <= (64ull << 20)) {
            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
            mach_vm_size_t read = 0;
            if (mach_vm_read_overwrite(mach_task_self(), address, size, reinterpret_cast<mach_vm_address_t>(buffer.data()), &read) == KERN_SUCCESS && read == size) {
                const std::uint64_t base = address;
                const std::uint64_t bytes = size;
                (void)!::write(fd, &base, sizeof(base));
                (void)!::write(fd, &bytes, sizeof(bytes));
                (void)!::write(fd, buffer.data(), buffer.size());
                snapshotState.bytes += size;
            }
        }
        address += size;
    }
    ::close(fd);
}

void* SnapshotThread(void*) {
    const auto base = std::chrono::steady_clock::now();
    const auto elapsed = [&base] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - base).count(); };
    while (elapsed() < snapshotState.atSeconds) ::usleep(50000);
    DumpSnapshot();
    snapshotState.active = true;
    const double windowEnd = elapsed() + snapshotState.windowSeconds;
    Write("[rosetta] snapshot: %llu bytes -> %s\n", static_cast<unsigned long long>(snapshotState.bytes), snapshotState.path.c_str());
    while (elapsed() < windowEnd) ::usleep(20000);
    snapshotState.active = false;
    Write("[rosetta] snapshot: window closed at %.3fs\n", elapsed());
    return nullptr;
}

void InitializeSnapshot() {
    const char* path = std::getenv("ANYPS5_TRACE_SNAPSHOT");
    if (path == nullptr || *path == '\0') return;
    snapshotState.enabled = true;
    snapshotState.path = path;
    if (const char* at = std::getenv("ANYPS5_TRACE_SNAPSHOT_AT")) snapshotState.atSeconds = std::strtod(at, nullptr);
    if (const char* window = std::getenv("ANYPS5_TRACE_SNAPSHOT_WINDOW")) snapshotState.windowSeconds = std::strtod(window, nullptr);
    pthread_t thread{};
    if (::pthread_create(&thread, nullptr, SnapshotThread, nullptr) != 0) {
        Write("[rosetta] snapshot: cannot start the snapshot thread\n");
        return;
    }
    ::pthread_detach(thread);
}

void AppendEntryRecord(const std::uint64_t entry, const ucontext_t* context) {
    if (snapshotState.enabled && !snapshotState.active) return;
    if (indirectTrace.entryFd < 0) return;
    const auto& state = context->uc_mcontext->__ss;
    struct Entry {
        std::uint64_t Entry;
        std::uint64_t Registers[16];
    } record{};
    record.Entry = entry;
    const std::uint64_t values[16] = {state.__rax, state.__rbx, state.__rcx, state.__rdx, state.__rsi, state.__rdi,
                                      state.__rbp, state.__rsp, state.__r8, state.__r9, state.__r10, state.__r11,
                                      state.__r12, state.__r13, state.__r14, state.__r15};
    std::memcpy(record.Registers, values, sizeof(values));
    (void)!::write(indirectTrace.entryFd, &record, sizeof(record));

    // Snapshot the pages reachable from the pointer registers and the stack,
    // page-granular and bounded, so the replay can map the same memory.
    if (indirectTrace.pageFd < 0 || indirectTrace.pagesWritten >= 30000) return;
    // One page set per entry: repeated entries would otherwise fill the file.
    for (std::size_t index = 0; index < indirectTrace.seenEntryCount; ++index)
        if (indirectTrace.seenEntries[index] == entry) return;
    if (indirectTrace.seenEntryCount < 4096) indirectTrace.seenEntries[indirectTrace.seenEntryCount++] = entry;
    constexpr std::uint64_t PageSize = 0x4000;
    constexpr std::size_t MaxPages = 64;
    std::uint64_t pages[MaxPages];
    std::size_t count = 0;
    for (const std::uint64_t value : values) {
        const std::uint64_t base = value & ~(PageSize - 1);
        if (base < indirectTrace.guestBase) continue;
        // A window around each pointer, not just the page it names: the body
        // dereferences [reg + offset] across struct fields and stack frames.
        for (std::int64_t step = -1; step <= 14; ++step) {
            const std::uint64_t page = static_cast<std::uint64_t>(static_cast<std::int64_t>(base) + step * static_cast<std::int64_t>(PageSize));
            if (page < indirectTrace.guestBase) continue;
            bool seen = false;
            for (std::size_t index = 0; index < count; ++index) if (pages[index] == page) { seen = true; break; }
            if (seen) continue;
            pages[count++] = page;
            if (count == MaxPages) break;
        }
        if (count == MaxPages) break;
    }
    for (std::size_t index = 0; index < count; ++index) {
        std::uint8_t buffer[PageSize];
        if (!ReadMemory(pages[index], buffer, sizeof(buffer))) continue;
        const std::uint64_t guestPage = pages[index] - indirectTrace.guestBase;
        struct PageRecord {
            std::uint64_t Entry;
            std::uint64_t Page;
        } record{entry, guestPage};
        (void)!::write(indirectTrace.pageFd, &record, sizeof(record));
        (void)!::write(indirectTrace.pageFd, buffer, sizeof(buffer));
        ++indirectTrace.pagesWritten;
    }
}

// Returns true when the faulting instruction is one of our trapped indirect
// branches and the context has been advanced past it.
bool HandleIndirectTrap(ucontext_t* context) {
    if (!indirectTrace.enabled) return false;
    auto& state = context->uc_mcontext->__ss;
    // int3 saves the address after the opcode; accept both layouts.
    const IndirectTraceSite* site = nullptr;
    std::uint64_t opcodeAddress = 0;
    if (state.__rip > 0) {
        site = FindIndirectSite(state.__rip - 1 - indirectTrace.guestBase);
        if (site != nullptr) opcodeAddress = state.__rip - 1;
    }
    if (site == nullptr) {
        site = FindIndirectSite(state.__rip - indirectTrace.guestBase);
        if (site != nullptr) opcodeAddress = state.__rip;
    }
    if (site == nullptr) return false;

    const std::uint8_t* bytes = site->Bytes;
    const bool isEntry = (site->PrefixLength & 0x80000000u) != 0;
    const std::uint32_t prefix = site->PrefixLength & 0x7fffffffu;
    const std::uint32_t length = site->Length;
    if (isEntry) {
        // A trapped `push r64` prologue: record the entry registers and emulate it.
        AppendEntryRecord(opcodeAddress - indirectTrace.guestBase, context);
        const std::uint8_t opcode = bytes[prefix];
        std::uint8_t rex = 0;
        for (std::uint32_t index = 0; index < prefix; ++index)
            if (bytes[index] >= 0x40 && bytes[index] <= 0x4F) rex = bytes[index];
        const unsigned index = (opcode & 7) | ((rex & 1) << 3);
        const std::uint64_t stack = state.__rsp - sizeof(std::uint64_t);
        *reinterpret_cast<volatile std::uint64_t*>(stack) = RegisterValue(context, index);
        state.__rsp = stack;
        state.__rip = opcodeAddress - prefix + length;
        return true;
    }
    if (prefix + 2 > length) return false;
    const std::uint8_t modrm = bytes[prefix + 1];
    const std::uint8_t mod = modrm >> 6;
    const std::uint8_t reg = (modrm >> 3) & 7;
    if (reg != 2 && reg != 4) return false;
    std::uint8_t rex = 0;
    for (std::uint32_t index = 0; index < prefix; ++index)
        if (bytes[index] >= 0x40 && bytes[index] <= 0x4F) rex = bytes[index];

    const std::uint8_t rm = modrm & 7;
    std::size_t position = prefix + 2;
    std::uint64_t target = 0;
    if (mod == 3) {
        target = RegisterValue(context, rm | ((rex & 1) << 3));
    } else {
        std::uint64_t address = 0;
        if (rm == 4) {
            const std::uint8_t sib = bytes[position++];
            const unsigned indexRegister = ((sib >> 3) & 7) | (((rex >> 1) & 1) << 3);
            const unsigned baseRegister = (sib & 7) | ((rex & 1) << 3);
            std::uint64_t index = 0;
            if (indexRegister != 4) index = RegisterValue(context, indexRegister) << (sib >> 6);
            if ((sib & 7) == 5 && mod == 0) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, bytes + position, sizeof(displacement));
                position += 4;
                address = static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement)) + index;
            } else {
                address = RegisterValue(context, baseRegister) + index;
            }
        } else if (mod == 0 && rm == 5) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + position, sizeof(displacement));
            position += 4;
            address = (opcodeAddress - prefix) + position + static_cast<std::int64_t>(displacement);
        } else {
            address = RegisterValue(context, rm | ((rex & 1) << 3));
        }
        if (mod == 1) {
            address += static_cast<std::int8_t>(bytes[position++]);
        } else if (mod == 2) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + position, sizeof(displacement));
            position += 4;
            address += static_cast<std::int64_t>(displacement);
        }
        target = *reinterpret_cast<const volatile std::uint64_t*>(address);
    }

    const bool isCall = reg == 2;
    if (isCall) {
        const std::uint64_t stack = state.__rsp - sizeof(std::uint64_t);
        const std::uint64_t returnAddress = opcodeAddress - prefix + length;
        *reinterpret_cast<volatile std::uint64_t*>(stack) = returnAddress;
        state.__rsp = stack;
    }
    state.__rip = target;
    if (Seconds() >= indirectTrace.afterSeconds && target >= indirectTrace.guestBase) {
        const std::uint64_t guestTarget = target - indirectTrace.guestBase;
        if (guestTarget >= indirectTrace.codeMin && guestTarget < indirectTrace.codeMax)
            AppendIndirectRecord(opcodeAddress - indirectTrace.guestBase, guestTarget, isCall ? 0u : 1u);
    }
    return true;
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
    for (int depth = 0; depth < 32 && frame != 0; ++depth) {
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
    if (signal == SIGTRAP && HandleIndirectTrap(context)) return;
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
    static const bool traceInitialized = [] { InitializeIndirectTrace(); InitializeSnapshot(); return true; }();
    static_cast<void>(traceInitialized);
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

}

namespace {

[[noreturn]] void RunGuest() {
    const int result = guestEntry(guestFrame.data(), GuestExitCallback);
    if (std::getenv("APS5_ROSETTA_QUIET") == nullptr) Write("[rosetta] guest entry returned %d at %.3fs\n", result, Seconds());
    std::exit(result);
}

void* GuestThread(void*) {
    InstallFaultDiagnostics();
    RunGuest();
}

// Called by the host application once AppKit is set up, on the main thread.
void StartGuestThread(void*) {
    pthread_attr_t attributes;
    if (::pthread_attr_init(&attributes) != 0) Fatal("cannot initialise the guest thread attributes");
    ::pthread_attr_setstacksize(&attributes, guestStackBytes);
    ::pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    // The guest's first thread keeps the main thread's priority.
    ::pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t thread{};
    if (::pthread_create(&thread, &attributes, GuestThread, nullptr) != 0) Fatal("cannot start the guest thread");
    ::pthread_attr_destroy(&attributes);
}

// The guest's main thread gets what it had as the process main thread: the stack size limit
// (8 MiB by default), never less.
std::size_t MainThreadStackBytes() {
    constexpr std::size_t Minimum = 8u << 20;
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    const std::size_t size = std::max(Minimum, ::pthread_get_stacksize_np(::pthread_self()));
    return (size + page - 1) / page * page;
}

}

extern "C" {

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
    if (const char* script = std::getenv("ANYPS5_INPUT_SCRIPT"); script != nullptr && *script != '\0')
        Write("[rosetta] input script env: present (%zu bytes)\n", std::strlen(script));
    // The engine launcher (anyps5_rosetta_run --diagnostics-json) asks for the startup event.
    if (const char* json = std::getenv("ANYPS5_DIAGNOSTICS_JSON"); json != nullptr && json[0] == '1')
        Write("{\"schema_version\":1,\"event\":\"startup\",\"entry\":1,\"seconds\":%.3f}\n", Seconds());
    guestEntry = entry;
    // libSceVideoOut's AppKit application owns the main thread when the title has a display;
    // APS5_ROSETTA_GUEST_ON_MAIN_THREAD=1 keeps the guest on it (no window events).
    using HostApplication = void (*)(void (*)(void*), void*);
    const char* onMain = std::getenv("APS5_ROSETTA_GUEST_ON_MAIN_THREAD");
    const auto host = reinterpret_cast<HostApplication>(::dlsym(RTLD_DEFAULT, "Aps5RunHostApplication_nid_no_patch"));
    if (host != nullptr && (onMain == nullptr || std::strcmp(onMain, "1") != 0)) {
        guestStackBytes = MainThreadStackBytes();
        host(StartGuestThread, nullptr);
        Fatal("the host application returned");
    }
    RunGuest();
}

}

#endif
