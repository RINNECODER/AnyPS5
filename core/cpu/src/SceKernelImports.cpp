#include <cpu/SceKernelImports.hpp>
#include <cpu/GuestFiles.hpp>
#include <cpu/SceElf.hpp>
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
                     Fsync, Ftruncate, Mkdir, Rmdir, Unlink, Rename, CheckReachability,
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
constexpr std::uint64_t ReplacementOffset = 0x1000;              // one page past the gate page

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
    std::uint64_t base;
    std::uint64_t replacements = 0;
    std::size_t nextSlot = 0;
    std::map<std::string, Binding> services;
    std::map<Key, std::uint64_t> gates;
    std::function<std::uint64_t()> errnoAddress;

    Impl(Machine& guest, const std::filesystem::path& resourceRoot, GuestFilesOptions options, std::uint64_t gateBase) :
        machine(guest), files(guest, resourceRoot, std::move(options)), base(gateBase) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE kernel import gates require a nonzero aligned low canonical guest page");
        for (const auto& [name, service] : fileServices)
            services.emplace(Nid::ComputeNid(name, "libkernel"), Binding{service, Convention::Kernel});
        for (const auto& [name, service] : posixServices)
            services.emplace(Nid::ComputeNid(name, "libkernel"), Binding{service, Convention::Posix});
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
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
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
    if (impl->nextSlot == 256) throw std::runtime_error("SCE kernel import gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const auto operation = service->second;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), operation](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE kernel import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

void SceKernelImports::SetTls(std::shared_ptr<SceTls> tls) { impl->tls = std::move(tls); }
void SceKernelImports::SetErrnoAddress(std::function<std::uint64_t()> provider) { impl->errnoAddress = std::move(provider); }

}
