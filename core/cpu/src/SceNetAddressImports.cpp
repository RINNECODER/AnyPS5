#include <cpu/SceNetAddressImports.hpp>
#include <cpu/SceElf.hpp>
#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

namespace Cpu {
namespace {

enum class Service { Htonl, Htons, InetNtop, InetPton };
constexpr std::size_t ipv4TextCapacity = 16;
constexpr std::size_t minimumIpv4TextCapacity = 8;
constexpr std::uint32_t maximumTextCapacity = 16 * 1024 * 1024;
static_assert(sizeof(in_addr) == 4);

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

void checkSpan(Machine& guest, std::uint64_t address, std::size_t size, Permission permission) {
    if (!address || size > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::runtime_error("Invalid SCE network address guest memory span");
    guest.CheckAccess(address, size, permission);
}

std::optional<std::array<char, ipv4TextCapacity>> copyText(Machine& guest, std::uint64_t address) {
    std::array<char, ipv4TextCapacity> text{};
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index > std::numeric_limits<std::uint64_t>::max() - address)
            throw std::runtime_error("Invalid SCE network address guest string span");
        const auto current = address + index;
        checkSpan(guest, current, 1, Permission::Read);
        std::array<std::byte, 1> byte;
        guest.Read(current, byte);
        text[index] = static_cast<char>(std::to_integer<unsigned char>(byte[0]));
        if (!text[index]) return text;
    }
    return std::nullopt;
}

void requireIpv4(std::uint64_t family) {
    if (static_cast<std::uint32_t>(family) != 2)
        throw std::runtime_error("Unsupported SCE network address family: " +
                                 std::to_string(static_cast<std::uint32_t>(family)));
}

}

struct SceNetAddressImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    const std::uint64_t base;
    std::size_t nextSlot = 0;
    const std::map<std::string, Service> services{
        {"9T2pDF2Ryqg", Service::Htonl}, {"iWQWrwiSt8A", Service::Htons},
        {"9vA2aW+CHuA", Service::InetNtop}, {"8Kcp5d-q1Uo", Service::InetPton}};
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::uint64_t gateBase) : machine(guest), base(gateBase) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE network address import gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }

    void invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        if (service == Service::Htonl) {
            const auto value = static_cast<std::uint32_t>(first);
            const auto swapped = ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
                                 ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
            guest.Set(Register::Rax, swapped);
            return;
        }
        if (service == Service::Htons) {
            const auto value = static_cast<std::uint16_t>(first);
            const auto swapped = static_cast<std::uint16_t>((value << 8) | (value >> 8));
            guest.Set(Register::Rax, swapped);
            return;
        }
        requireIpv4(first);
        const auto source = guest.Get(Register::Rsi);
        const auto destination = guest.Get(Register::Rdx);
        if (service == Service::InetPton) {
            if (!destination)
                throw std::runtime_error("Unsupported SCE network address null destination: guest errno is unavailable");
            const auto text = copyText(guest, source);
            if (!text) {
                guest.Set(Register::Rax, 0);
                return;
            }
            in_addr address{};
            const auto parsed = ::inet_pton(AF_INET, text->data(), &address);
            if (parsed == 0) {
                guest.Set(Register::Rax, 0);
                return;
            }
            if (parsed != 1)
                throw std::runtime_error("Unsupported SCE network address conversion: native IPv4 parsing failed");
            const auto bytes = std::as_bytes(std::span(&address, 1));
            checkSpan(guest, destination, bytes.size(), Permission::Write);
            guest.Write(destination, bytes);
            guest.Set(Register::Rax, 1);
            return;
        }
        const auto capacity = static_cast<std::uint32_t>(guest.Get(Register::Rcx));
        if (capacity < minimumIpv4TextCapacity || capacity > maximumTextCapacity)
            throw std::runtime_error("Unsupported SCE network address buffer capacity: " + std::to_string(capacity));
        in_addr address{};
        const auto bytes = std::as_writable_bytes(std::span(&address, 1));
        checkSpan(guest, source, bytes.size(), Permission::Read);
        guest.Read(source, bytes);
        checkSpan(guest, destination, capacity, Permission::Write);
        std::array<char, ipv4TextCapacity> text{};
        if (!::inet_ntop(AF_INET, &address, text.data(), static_cast<socklen_t>(text.size())))
            throw std::runtime_error("Unsupported SCE network address conversion: native IPv4 formatting failed");
        const auto length = std::strlen(text.data()) + 1;
        if (length > capacity)
            throw std::runtime_error("Unsupported SCE network address buffer capacity: IPv4 text does not fit; guest errno is unavailable");
        guest.Write(destination, std::as_bytes(std::span(text).first(length)));
        guest.Set(Register::Rax, destination);
    }
};

SceNetAddressImports::SceNetAddressImports(Machine& machine, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, gateBase)) {}
SceNetAddressImports::~SceNetAddressImports() = default;

std::optional<std::uint64_t> SceNetAddressImports::Resolve(const SceImport& import) {
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) return std::nullopt;
    if (import.LibraryName != "libSceNet" && import.ModuleName != "libSceNet") return std::nullopt;
    if (import.LibraryName != "libSceNet" || import.ModuleName != "libSceNet" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE network address import scope/version: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->nextSlot == 256) throw std::runtime_error("SCE network address import gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), operation = service->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE network address import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

}
