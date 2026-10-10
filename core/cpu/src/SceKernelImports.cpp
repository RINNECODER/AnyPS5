#include <cpu/SceKernelImports.hpp>
#include <cpu/GuestFiles.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceTls.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <bit>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace Cpu {
namespace {

enum class Service { Open, Read, Pread, Lseek, Close, TlsAddress, ReadTsc, TscFrequency,
                     SanitizerDisabled, MallocReplace, NewReplace };

// Guest-visible sanitizer replacement tables, mirroring the prx ABI structures in
// core/libs/SceTypes.hpp (MallocReplace, NewReplace): one leading uint64 size field
// followed by the replacement entry points. All entry points stay null, which is how
// the prx side reports "no sanitizer installed"; libc then keeps its own allocator and
// operator new/delete. The storage is writable because the getter hands out the caller's
// registration table, exactly like the static object in libkernel System/src/Sanitizer.cpp.
constexpr std::uint64_t MallocReplaceBytes = 120;  // uint64 size + 14 entry points
constexpr std::uint64_t NewReplaceBytes = 104;     // uint64 size + 12 entry points
constexpr std::uint64_t NewReplaceOffset = 0x80;   // 8-byte aligned after MallocReplace
constexpr std::uint64_t ReplacementOffset = 0x1000; // one page past the gate page

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
    std::map<std::string, Service> services;
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, const std::filesystem::path& resourceRoot, std::uint64_t gateBase) :
        machine(guest), files(guest, resourceRoot), base(gateBase) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE kernel import gates require a nonzero aligned low canonical guest page");
        for (const auto& [name, service] : std::array<std::pair<const char*, Service>, 9>{{
                 {"sceKernelOpen", Service::Open}, {"sceKernelRead", Service::Read},
                 {"sceKernelPread", Service::Pread}, {"sceKernelLseek", Service::Lseek},
                 {"sceKernelClose", Service::Close}, {"__tls_get_addr", Service::TlsAddress},
                 {"sceKernelIsAddressSanitizerEnabled", Service::SanitizerDisabled},
                 {"sceKernelGetSanitizerMallocReplaceExternal", Service::MallocReplace},
                 {"sceKernelGetSanitizerNewReplaceExternal", Service::NewReplace}}})
            services.emplace(Nid::ComputeNid(name, "libkernel"), service);
        // Both read the clock behind guest RDTSC, so guest-visible time agrees.
        if (Machine::TscFrequency()) {
            services.emplace(Nid::ComputeNid("sceKernelReadTsc", "libkernel"), Service::ReadTsc);
            services.emplace(Nid::ComputeNid("sceKernelGetTscFrequency", "libkernel"), Service::TscFrequency);
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

    void invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        const auto third = guest.Get(Register::Rdx);
        std::int64_t result;
        switch (service) {
        case Service::Open: result = files.Open(first, static_cast<std::uint32_t>(second), static_cast<std::uint16_t>(third)); break;
        case Service::Read: result = files.Read(signedInt(first), second, third); break;
        case Service::Pread: result = files.Pread(signedInt(first), second, third, std::bit_cast<std::int64_t>(guest.Get(Register::Rcx))); break;
        case Service::Lseek: result = files.Lseek(signedInt(first), std::bit_cast<std::int64_t>(second), signedInt(third)); break;
        case Service::Close: result = files.Close(signedInt(first)); break;
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
        guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
    }
};

SceKernelImports::SceKernelImports(Machine& machine, const std::filesystem::path& resourceRoot, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, resourceRoot, gateBase)) {}
SceKernelImports::~SceKernelImports() = default;

std::uint64_t SceKernelImports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE kernel import scope/version: " + identity(import));
    const auto service = impl->services.find(import.Nid);
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

}
