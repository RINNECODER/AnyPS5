#include <cpu/SceImports.hpp>
#include <cpu/SceElf.hpp>
#include <nid/NidCompute.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Cpu {
namespace {
constexpr std::uint64_t TransferChunk = 16 * 1024 * 1024;
constexpr std::size_t MaximumString = 1024 * 1024;
enum class Service { Copy, Move, Set, Length, Compare, Exit };

struct ProcessExitState {
    std::function<void(int)> handler;
};

std::vector<char> guestString(Machine& machine, std::uint64_t address) {
    std::vector<char> result;
    std::array<char, 256> bytes{};
    while (result.size() < MaximumString) {
        const auto count = std::min<std::size_t>({bytes.size(), 4096 - (address & 4095), MaximumString - result.size()});
        machine.Read(address, std::as_writable_bytes(std::span(bytes).first(count)));
        const auto zero = std::find(bytes.begin(), bytes.begin() + count, '\0');
        result.insert(result.end(), bytes.begin(), zero);
        if (zero != bytes.begin() + count) { result.push_back('\0'); return result; }
        if (count > std::numeric_limits<std::uint64_t>::max() - address)
            throw std::runtime_error("SCE libc string address overflows guest memory");
        address += count;
    }
    throw std::runtime_error("SCE libc string exceeds the supported 1 MiB bound or has no terminator");
}

void invoke(Machine& machine, Service service, const std::function<void(int)>& processExit = {}) {
    const auto first = machine.Get(Register::Rdi);
    const auto second = machine.Get(Register::Rsi);
    if (service == Service::Exit) {
        const auto code = static_cast<int>(first & 255);
        if (processExit) processExit(code);
        else machine.Exit(code);
        return;
    }
    if (service == Service::Length || service == Service::Compare) {
        const auto left = guestString(machine, first);
        if (service == Service::Length) machine.Set(Register::Rax, std::strlen(left.data()));
        else {
            const auto right = guestString(machine, second);
            machine.Set(Register::Rax, static_cast<std::uint32_t>(std::strcmp(left.data(), right.data())));
        }
        return;
    }
    const auto length = machine.Get(Register::Rdx);
    if (length != 0) {
        // Both ranges are checked before the first byte moves, so a fault changes nothing.
        machine.CheckAccess(first, length, Permission::Write);
        if (service != Service::Set) machine.CheckAccess(second, length, Permission::Read);
        // Any length moves through one bounded host buffer. Each chunk is read before it is
        // written; copying backwards when the destination starts inside the source keeps
        // overlapping memmove (and memcpy, which shares it) exact.
        std::vector<std::byte> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(length, TransferChunk)));
        if (service == Service::Set) {
            std::fill(buffer.begin(), buffer.end(), static_cast<std::byte>(second & 255));
            for (std::uint64_t offset = 0; offset < length; offset += buffer.size())
                machine.Write(first + offset, std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - offset))));
        } else if (first > second && first - second < length) {
            for (std::uint64_t end = length; end;) {
                const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), end));
                end -= size;
                machine.Read(second + end, std::span(buffer).first(size));
                machine.Write(first + end, std::span(buffer).first(size));
            }
        } else {
            for (std::uint64_t offset = 0; offset < length;) {
                const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - offset));
                machine.Read(second + offset, std::span(buffer).first(size));
                machine.Write(first + offset, std::span(buffer).first(size));
                offset += size;
            }
        }
    }
    machine.Set(Register::Rax, first);
}

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}
}

struct SceImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    std::uint64_t base;
    std::uint64_t exitGate;
    std::size_t nextSlot = 1;
    std::map<std::string, Service> services;
    std::map<Key, std::uint64_t> gates;
    std::shared_ptr<ProcessExitState> processExit = std::make_shared<ProcessExitState>();

    Impl(Machine& value, std::uint64_t address) : machine(value), base(address), exitGate(address) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE import gates require a nonzero aligned low canonical guest page");
        for (const auto& [name, service] : std::array<std::pair<const char*, Service>, 6>{{
                 {"memcpy", Service::Copy}, {"memmove", Service::Move}, {"memset", Service::Set},
                 {"strlen", Service::Length}, {"strcmp", Service::Compare}, {"exit", Service::Exit}}})
            services.emplace(Nid::ComputeNid(name, "libc"), service);
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        bytes[0] = std::byte{0xc3};
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
        machine.AddHostCall(exitGate, [](Machine&) {
            throw std::runtime_error("Unsupported SCE entry termination callback: exact guest callback semantics are not implemented");
        });
    }

    std::uint64_t resolve(const SceImport& import) {
        if (import.LibraryName != "libc" || import.ModuleName != "libc" || import.LibraryVersion != 1 ||
            import.ModuleMajor != 1 || import.ModuleMinor != 1)
            throw std::runtime_error("Unsupported SCE import scope/version: " + identity(import));
        const auto service = services.find(import.Nid);
        if (service == services.end()) throw std::runtime_error("Unsupported SCE import service: " + identity(import));
        const Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                      import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
        if (const auto found = gates.find(key); found != gates.end()) return found->second;
        if (nextSlot == 256) throw std::runtime_error("SCE import gate page is exhausted");
        const auto gate = base + nextSlot * 16;
        const auto operation = service->second;
        const std::array ret{std::byte{0xc3}};
        machine.Write(gate, ret);
        if (operation == Service::Exit) {
            machine.AddHostCall(gate, [state = std::weak_ptr<ProcessExitState>(processExit)](Machine& cpu) {
                const auto context = state.lock();
                if (!context) throw std::runtime_error("SCE libc process exit import runtime has expired");
                const auto handler = context->handler;
                invoke(cpu, Service::Exit, handler);
            });
        } else machine.AddHostCall(gate, [operation](Machine& cpu) { invoke(cpu, operation); });
        gates.emplace(key, gate);
        ++nextSlot;
        return gate;
    }
};

SceImports::SceImports(Machine& machine, std::uint64_t gateBase) : impl(std::make_unique<Impl>(machine, gateBase)) {}
SceImports::~SceImports() = default;
std::uint64_t SceImports::Resolve(const SceImport& import) { return impl->resolve(import); }
std::uint64_t SceImports::ExitGate() const { return impl->exitGate; }
void SceImports::SetProcessExitHandler(std::function<void(int)> handler) { impl->processExit->handler = std::move(handler); }

}
