#include <cpu/SceKernelImports.hpp>
#include <cpu/GuestFiles.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <cpu/SceTls.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace Cpu {
namespace {

enum class Service { Open, Read, Pread, Lseek, Close, Write, Pwrite, Stat, Fstat, Getdents, Getdirentries,
                     Fsync, Ftruncate, Mkdir, Rmdir, Unlink, Rename, CheckReachability, Truncate, Utimes,
                     AioSubmitRead, AioWait, AioWaitMany, AioDelete, AioCancel,
                     TlsAddress, ReadTsc, TscFrequency, SanitizerDisabled, MallocReplace, NewReplace };

// sceKernel* functions return 0x8002xxxx error codes. Their POSIX aliases return -1 and store
// the FreeBSD errno in the calling thread's errno cell; libkernel also exports those aliases
// under the libScePosix library.
enum class Convention { Kernel, Posix };

struct Binding {
    Service service;
    Convention convention;
};

constexpr std::array<std::pair<const char*, Service>, 18> fileServices{{
    {"sceKernelOpen", Service::Open}, {"sceKernelRead", Service::Read}, {"sceKernelPread", Service::Pread},
    {"sceKernelLseek", Service::Lseek}, {"sceKernelClose", Service::Close}, {"sceKernelWrite", Service::Write},
    {"sceKernelPwrite", Service::Pwrite}, {"sceKernelStat", Service::Stat}, {"sceKernelFstat", Service::Fstat},
    {"sceKernelGetdents", Service::Getdents}, {"sceKernelGetdirentries", Service::Getdirentries},
    {"sceKernelFsync", Service::Fsync}, {"sceKernelFtruncate", Service::Ftruncate}, {"sceKernelMkdir", Service::Mkdir},
    {"sceKernelRmdir", Service::Rmdir}, {"sceKernelUnlink", Service::Unlink}, {"sceKernelRename", Service::Rename},
    {"sceKernelCheckReachability", Service::CheckReachability}}};

constexpr std::array<std::pair<const char*, Service>, 22> posixServices{{
    {"open", Service::Open}, {"_open", Service::Open}, {"read", Service::Read}, {"_read", Service::Read},
    {"pread", Service::Pread}, {"write", Service::Write}, {"_write", Service::Write}, {"pwrite", Service::Pwrite},
    {"lseek", Service::Lseek}, {"close", Service::Close}, {"_close", Service::Close}, {"stat", Service::Stat},
    {"fstat", Service::Fstat}, {"_fstat", Service::Fstat}, {"getdents", Service::Getdents},
    {"getdirentries", Service::Getdirentries}, {"fsync", Service::Fsync}, {"ftruncate", Service::Ftruncate},
    {"mkdir", Service::Mkdir}, {"rmdir", Service::Rmdir}, {"unlink", Service::Unlink}, {"rename", Service::Rename}}};

// Rows whose NIDs are not the plain hash of their name (upstream exports them with a
// _nid_postfix spelling), plus the asynchronous I/O rows, bound by NID.
constexpr std::array<std::pair<const char*, Service>, 7> nidServices{{
    {"WlyEA-sLDf0", Service::Truncate}, {"0Cq8ipKr9n0", Service::Utimes},
    {"HgX7+AORI58", Service::AioSubmitRead}, {"KOF-oJbQVvc", Service::AioWait}, {"lgK+oIWkJyA", Service::AioWaitMany},
    {"5TgME6AYty4", Service::AioDelete}, {"fR521KIGgb8", Service::AioCancel}}};

// Asynchronous I/O after upstream libkernel Aio/src/Aio.cpp: every request runs to completion
// inside the submit call, so a wait never blocks. Request ids are 1..511 and recycle.
constexpr std::int32_t AioProcessing = 2, AioCompleted = 3, AioAborted = 4, AioMaxQueues = 512, AioIdNumMax = 128;
constexpr std::uint32_t AioWaitAnd = 1, AioWaitOr = 2;
constexpr std::uint64_t AioRequestSize = 40;  // int64 offset, size_t nbyte, void* buf, result*, int32 fd
constexpr std::int64_t KernelEfault = std::bit_cast<std::int32_t>(0x8002000Eu);
constexpr std::int64_t KernelEinval = std::bit_cast<std::int32_t>(0x80020016u);

// Guest-visible sanitizer replacement tables, mirroring the prx ABI structures in
// core/libs/SceTypes.hpp (MallocReplace, NewReplace): one leading uint64 size field
// followed by the replacement entry points. All entry points stay null, which is how
// the prx side reports "no sanitizer installed"; libc then keeps its own allocator and
// operator new/delete. The storage is writable because the getter hands out the caller's
// registration table, exactly like the static object in libkernel System/src/Sanitizer.cpp.
// MallocReplace is uint64 size + 13 entry points, NewReplace is uint64 size + 12.
constexpr std::uint64_t MallocReplaceBytes = 112;
constexpr std::uint64_t NewReplaceBytes = 104;
constexpr std::uint64_t NewReplaceOffset = MallocReplaceBytes;   // 8-byte aligned after it
// Just past the 1 MiB the gate trampolines reserve.
constexpr std::uint64_t ReplacementOffset = (SceHostTrampolines::DefaultCapacity + 1) * 16;

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

std::int32_t signedInt(std::uint64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

}

struct SceKernelImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    GuestFiles files;
    std::shared_ptr<SceTls> tls;
    SceHostTrampolines trampolines;
    std::uint64_t replacements = 0;
    std::map<std::string, Binding> services;
    std::map<Key, std::uint64_t> gates;
    std::function<std::uint64_t()> errnoAddress;
    std::array<std::int32_t, AioMaxQueues> aioStates{};
    std::int32_t aioNext = 1;

    bool writable(std::uint64_t address, std::uint64_t size) {
        if (!address || address > std::numeric_limits<std::uint64_t>::max() - size) return false;
        try { machine.CheckAccess(address, size, Permission::Read | Permission::Write); }
        catch (const std::runtime_error&) { return false; }
        return true;
    }
    template<class T> T load(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    template<class T> void store(std::uint64_t address, T value) {
        machine.Write(address, std::as_bytes(std::span(&value, 1)));
    }
    static bool aioValid(std::int32_t id) { return id > 0 && id < AioMaxQueues; }

    std::int64_t aioSubmitRead(std::uint64_t requests, std::int32_t size, std::uint64_t id) {
        if (!requests || !id) return KernelEfault;
        if (size <= 0) return KernelEinval;
        if (static_cast<std::uint64_t>(size) > std::numeric_limits<std::uint64_t>::max() / AioRequestSize ||
            !writable(requests, AioRequestSize * static_cast<std::uint64_t>(size)) || !writable(id, 4))
            return KernelEfault;
        for (std::int32_t index = 0; index < size; ++index)
            if (!writable(load<std::uint64_t>(requests + AioRequestSize * index + 24), 16)) return KernelEfault;
        const std::int32_t queue = aioNext;
        aioNext = aioNext + 1 == AioMaxQueues ? 1 : aioNext + 1;
        bool aborted = false;
        for (std::int32_t index = 0; index < size; ++index) {
            const auto request = requests + AioRequestSize * index;
            const auto offset = load<std::int64_t>(request);
            const auto count = load<std::uint64_t>(request + 8);
            const auto buffer = load<std::uint64_t>(request + 16);
            const auto result = load<std::uint64_t>(request + 24);
            const auto fd = load<std::int32_t>(request + 32);
            const auto done = files.Pread(fd, buffer, count, offset);
            store<std::int64_t>(result, done);
            store<std::uint32_t>(result + 8, done < 0 ? AioAborted : AioCompleted);
            aborted = aborted || done < 0;
        }
        aioStates[queue] = aborted ? AioAborted : AioCompleted;
        store<std::int32_t>(id, queue);
        return 0;
    }

    std::int64_t aioWait(std::int32_t id, std::uint64_t state) {
        if (!writable(state, 4)) return KernelEfault;
        if (!aioValid(id)) return KernelEinval;
        store<std::int32_t>(state, aioStates[id]);
        return 0;
    }

    std::int64_t aioWaitMany(std::uint64_t ids, std::int32_t count, std::uint64_t states, std::uint32_t mode) {
        if (!ids || !states) return KernelEfault;
        if (count < 0) return KernelEinval;
        if (count > AioIdNumMax) throw std::runtime_error("sceKernelAioWaitRequests: more than 128 ids per batch is not modelled");
        if (count && (!writable(ids, 4ULL * count) || !writable(states, 4ULL * count))) return KernelEfault;
        for (std::int32_t index = 0; index < count; ++index)
            if (!aioValid(load<std::int32_t>(ids + 4ULL * index))) return KernelEinval;
        if (mode != AioWaitAnd && mode != AioWaitOr) throw std::runtime_error("sceKernelAioWaitRequests: unsupported wait mode");
        for (std::int32_t index = 0; index < count; ++index)
            store<std::int32_t>(states + 4ULL * index, aioStates[load<std::int32_t>(ids + 4ULL * index)]);
        return 0;
    }

    std::int64_t aioDelete(std::int32_t id, std::uint64_t output) {
        if (!writable(output, 4)) return KernelEfault;
        if (!aioValid(id)) return KernelEinval;
        aioStates[id] = AioAborted;
        store<std::int32_t>(output, 0);
        return 0;
    }

    std::int64_t aioCancel(std::int32_t id, std::uint64_t state) {
        if (!writable(state, 4)) return KernelEfault;
        if (id == 0) { store<std::int32_t>(state, AioProcessing); return 0; }
        if (!aioValid(id)) return KernelEinval;
        aioStates[id] = AioAborted;
        store<std::int32_t>(state, AioAborted);
        return 0;
    }

    Impl(Machine& guest, const std::filesystem::path& resourceRoot, GuestFilesOptions options, std::uint64_t base) :
        machine(guest), files(guest, resourceRoot, std::move(options)),
        trampolines(guest, base, SceHostTrampolines::DefaultCapacity, "SCE kernel import") {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE kernel import gates require a nonzero aligned low canonical guest page");
        for (const auto& [name, service] : fileServices)
            services.emplace(Nid::ComputeNid(name, "libkernel"), Binding{service, Convention::Kernel});
        for (const auto& [name, service] : posixServices)
            services.emplace(Nid::ComputeNid(name, "libkernel"), Binding{service, Convention::Posix});
        for (const auto& [nid, service] : nidServices) services.emplace(nid, Binding{service, Convention::Kernel});
        for (const auto& [name, service] : std::array<std::pair<const char*, Service>, 4>{{
                 {"__tls_get_addr", Service::TlsAddress},
                 {"sceKernelIsAddressSanitizerEnabled", Service::SanitizerDisabled},
                 {"sceKernelGetSanitizerMallocReplaceExternal", Service::MallocReplace},
                 {"sceKernelGetSanitizerNewReplaceExternal", Service::NewReplace}}})
            services.emplace(Nid::ComputeNid(name, "libkernel"), Binding{service, Convention::Kernel});
        // Both read the clock behind guest RDTSC, so guest-visible time agrees.
        if (Machine::TscFrequency()) {
            services.emplace(Nid::ComputeNid("sceKernelReadTsc", "libkernel"), Binding{Service::ReadTsc, Convention::Kernel});
            services.emplace(Nid::ComputeNid("sceKernelGetTscFrequency", "libkernel"), Binding{Service::TscFrequency, Convention::Kernel});
        }
        std::array<std::byte, 4096> tables{};
        tables.fill(std::byte{});
        const auto putSize = [&](std::uint64_t offset, std::uint64_t value) {
            for (unsigned index = 0; index < 8; ++index)
                tables[offset + index] = static_cast<std::byte>(value >> (8 * index));
        };
        putSize(0, MallocReplaceBytes);
        putSize(NewReplaceOffset, NewReplaceBytes);
        replacements = base + ReplacementOffset;
        machine.Map(replacements, tables.size(), Permission::Read | Permission::Write);
        machine.Write(replacements, std::as_bytes(std::span(tables)));
    }

    // POSIX convention: a failed call stores errno in the active thread's errno cell and returns -1.
    std::int64_t posixResult(Machine& guest, std::int64_t result) {
        if (result >= 0) return result;
        const auto error = static_cast<std::int32_t>(static_cast<std::uint32_t>(result) & 0xffff);
        // A zero address means the caller has no errno cell; the -1 result still reports failure.
        if (const auto address = errnoAddress ? errnoAddress() : 0) {
            if (address > std::numeric_limits<std::uint64_t>::max() - 4)
                throw std::runtime_error("SCE POSIX file call guest errno address overflows");
            guest.CheckAccess(address, 4, Permission::Read | Permission::Write);
            guest.Write(address, std::as_bytes(std::span(&error, 1)));
        }
        return -1;
    }

    void invoke(Machine& guest, Binding binding) {
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        const auto third = guest.Get(Register::Rdx);
        const auto fourth = guest.Get(Register::Rcx);
        std::int64_t result;
        switch (binding.service) {
        case Service::Open: result = files.Open(first, static_cast<std::uint32_t>(second), static_cast<std::uint16_t>(third)); break;
        case Service::Read: result = files.Read(signedInt(first), second, third); break;
        case Service::Pread: result = files.Pread(signedInt(first), second, third, std::bit_cast<std::int64_t>(fourth)); break;
        case Service::Lseek: result = files.Lseek(signedInt(first), std::bit_cast<std::int64_t>(second), signedInt(third)); break;
        case Service::Close: result = files.Close(signedInt(first)); break;
        case Service::Write: result = files.Write(signedInt(first), second, third); break;
        case Service::Pwrite: result = files.Pwrite(signedInt(first), second, third, std::bit_cast<std::int64_t>(fourth)); break;
        case Service::Stat: result = files.Stat(first, second); break;
        case Service::Fstat: result = files.Fstat(signedInt(first), second); break;
        case Service::Getdents: result = files.Getdents(signedInt(first), second, third); break;
        case Service::Getdirentries: result = files.Getdirentries(signedInt(first), second, third, fourth); break;
        case Service::Fsync: result = files.Fsync(signedInt(first)); break;
        case Service::Ftruncate: result = files.Ftruncate(signedInt(first), std::bit_cast<std::int64_t>(second)); break;
        case Service::Mkdir: result = files.Mkdir(first, static_cast<std::uint16_t>(second)); break;
        case Service::Rmdir: result = files.Rmdir(first); break;
        case Service::Unlink: result = files.Unlink(first); break;
        case Service::Rename: result = files.Rename(first, second); break;
        case Service::CheckReachability: result = files.CheckReachability(first); break;
        case Service::Truncate: result = files.Truncate(first, std::bit_cast<std::int64_t>(second)); break;
        case Service::Utimes: result = files.Utimes(first, second); break;
        case Service::AioSubmitRead: result = aioSubmitRead(first, signedInt(second), fourth); break;
        case Service::AioWait: result = aioWait(signedInt(first), second); break;
        case Service::AioWaitMany: result = aioWaitMany(first, signedInt(second), third, static_cast<std::uint32_t>(fourth)); break;
        case Service::AioDelete: result = aioDelete(signedInt(first), second); break;
        case Service::AioCancel: result = aioCancel(signedInt(first), second); break;
        case Service::TlsAddress:
            if (!tls) throw std::runtime_error("SCE __tls_get_addr called before main-module TLS is configured");
            result = static_cast<std::int64_t>(tls->ResolveIndex(first));
            break;
        case Service::ReadTsc: result = std::bit_cast<std::int64_t>(Machine::ReadTsc()); break;
        case Service::TscFrequency: result = std::bit_cast<std::int64_t>(Machine::TscFrequency()); break;
        case Service::SanitizerDisabled: result = 0; break;
        case Service::MallocReplace: result = static_cast<std::int64_t>(replacements); break;
        case Service::NewReplace: result = static_cast<std::int64_t>(replacements + NewReplaceOffset); break;
        default: throw std::runtime_error("Unsupported SCE kernel import operation");
        }
        if (binding.convention == Convention::Posix) result = posixResult(guest, result);
        guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
    }
};

SceKernelImports::SceKernelImports(Machine& machine, const std::filesystem::path& resourceRoot, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, resourceRoot, GuestFilesOptions{}, gateBase)) {}
SceKernelImports::SceKernelImports(Machine& machine, const std::filesystem::path& resourceRoot, GuestFilesOptions files,
                                   std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, resourceRoot, std::move(files), gateBase)) {}
SceKernelImports::~SceKernelImports() = default;

std::uint64_t SceKernelImports::Resolve(const SceImport& import) {
    const auto service = impl->services.find(import.Nid);
    const bool posixLibrary = import.LibraryName == "libScePosix" && service != impl->services.end() &&
                              service->second.convention == Convention::Posix;
    if ((import.LibraryName != "libkernel" && !posixLibrary) || import.ModuleName != "libkernel" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE kernel import scope/version: " + identity(import));
    if (service == impl->services.end()) throw std::runtime_error("Unsupported SCE kernel import service: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto operation = service->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl), operation](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE kernel import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    return gate;
}

GuestFiles& SceKernelImports::Files() { return impl->files; }
void SceKernelImports::SetTls(std::shared_ptr<SceTls> tls) { impl->tls = std::move(tls); }
void SceKernelImports::SetErrnoAddress(std::function<std::uint64_t()> provider) { impl->errnoAddress = std::move(provider); }

}
