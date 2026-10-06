#include <cpu/SceLibcBootstrapImports.hpp>
#include <cpu/SceElf.hpp>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace Cpu {
namespace {

enum class Service { ProcessParameters, HeapRegistration, TraceInfo, StackGuard, ProgramName };
constexpr std::size_t pageSize = 4096;
constexpr std::size_t guardOffset = 0;
constexpr std::size_t programPointerOffset = 8;
constexpr std::size_t programStringOffset = 16;
constexpr std::size_t maskOffset = programStringOffset + 256;
constexpr std::size_t tableOffset = maskOffset + 8;
static_assert(tableOffset + 65 * 8 <= pageSize);

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

std::uint64_t readQword(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + index])) << (8 * index);
    return value;
}

void writeInteger(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index)
        bytes[offset + index] = std::byte((value >> (8 * index)) & 0xff);
}

bool validUtf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index++]);
        if (first < 0x80) continue;
        unsigned count;
        std::uint32_t value, minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
        else return false;
        if (count > text.size() - index) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[index++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

}

struct SceLibcBootstrapImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    const std::uint64_t base;
    const std::uint64_t dataBase;
    std::size_t nextSlot = 0;
    std::map<Key, std::uint64_t> gates;
    mutable std::mutex stateMutex;
    std::optional<std::pair<std::uint64_t, std::size_t>> processSpan;
    std::optional<std::array<std::uint64_t, 10>> heapCallbacks;
    const std::map<std::string, Service> services{
        {"959qrazPIrg", Service::ProcessParameters}, {"p5EcQeEeJAE", Service::HeapRegistration},
        {"NWtTN10cJzE", Service::TraceInfo}, {"f7uOxY9mM1U", Service::StackGuard},
        {"djxxOmW6-aw", Service::ProgramName}};

    Impl(Machine& guest, const std::string& name, std::uint64_t gateBase) :
        machine(guest), base(gateBase), dataBase(gateBase + pageSize) {
        if (!base || (base & (pageSize - 1)) || base >= 0x7fffffffe000)
            throw std::invalid_argument("SCE libc bootstrap requires two nonzero aligned low canonical guest pages");
        if (name.size() > 255 || name.find('\0') != std::string::npos || !validUtf8(name))
            throw std::invalid_argument("SCE libc bootstrap program name requires at most 255 UTF-8 bytes without NUL");
        std::array<std::byte, pageSize> data{};
        std::random_device entropy;
        auto guard = (std::uint64_t(entropy()) << 32) ^ std::uint64_t(entropy());
        if (!guard) guard = 1;
        writeInteger(data, guardOffset, guard, 8);
        writeInteger(data, programPointerOffset, dataBase + programStringOffset, 8);
        for (std::size_t index = 0; index < name.size(); ++index)
            data[programStringOffset + index] = std::byte(static_cast<unsigned char>(name[index]));
        std::array<std::byte, pageSize> code;
        code.fill(std::byte{0xcc});
        machine.Map(base, 2 * pageSize, Permission::Read | Permission::Write);
        machine.Write(base, code);
        machine.Write(dataBase, data);
        machine.Protect(base, pageSize, Permission::Read | Permission::Execute);
    }

    void access(std::uint64_t address, std::size_t size, Permission permission) const {
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address)
            throw std::runtime_error("SCE libc bootstrap guest span is invalid");
        machine.CheckAccess(address, size, permission);
    }

    void registerHeap(std::uint64_t address) {
        std::array<std::byte, 80> bytes;
        access(address, bytes.size(), Permission::Read);
        machine.Read(address, bytes);
        std::array<std::uint64_t, 10> callbacks;
        for (std::size_t index = 0; index < callbacks.size(); ++index)
            callbacks[index] = readQword(bytes, index * 8);
        if (!callbacks[0] || !callbacks[1])
            throw std::runtime_error("SCE libc bootstrap heap callbacks require malloc and free");
        for (const auto callback : callbacks)
            if (callback) access(callback, 1, Permission::Execute);
        std::lock_guard lock(stateMutex);
        if (heapCallbacks && *heapCallbacks != callbacks)
            throw std::runtime_error("SCE libc bootstrap heap callbacks cannot replace an active table");
        heapCallbacks = callbacks;
    }

    void traceInfo(std::uint64_t address) {
        std::array<std::byte, 32> bytes;
        access(address, bytes.size(), Permission::Read);
        machine.Read(address, bytes);
        const auto size = readQword(bytes, 0);
        if (size != bytes.size())
            throw std::runtime_error("Unsupported SCE libc bootstrap trace info size: " + std::to_string(size));
        access(address, bytes.size(), Permission::Write);
        writeInteger(bytes, 12, 0, 4);
        writeInteger(bytes, 16, dataBase + maskOffset, 8);
        writeInteger(bytes, 24, dataBase + tableOffset, 8);
        machine.Write(address, bytes);
    }

    void invoke(Machine& guest, Service service) {
        if (service == Service::ProcessParameters) {
            std::lock_guard lock(stateMutex);
            guest.Set(Register::Rax, processSpan ? processSpan->first : 0);
        } else if (service == Service::HeapRegistration) registerHeap(guest.Get(Register::Rdi));
        else if (service == Service::TraceInfo) traceInfo(guest.Get(Register::Rdi));
        else throw std::runtime_error("Unsupported SCE libc bootstrap gate operation");
    }
};

SceLibcBootstrapImports::SceLibcBootstrapImports(Machine& machine, std::string programName,
                                             std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, programName, gateBase)) {}
SceLibcBootstrapImports::~SceLibcBootstrapImports() = default;

void SceLibcBootstrapImports::SetProcessParameters(std::uint64_t address, std::size_t size) {
    if ((!address && size) || (address && size < 0x40))
        throw std::invalid_argument("SCE libc bootstrap process parameters require absence or a readable span of at least 64 bytes");
    if (address) impl->access(address, size, Permission::Read);
    std::lock_guard lock(impl->stateMutex);
    if (impl->processSpan && *impl->processSpan != std::pair{address, size})
        throw std::runtime_error("SCE libc bootstrap process parameters cannot replace an active span");
    if (address) impl->processSpan = std::pair{address, size};
}

std::optional<std::array<std::uint64_t, 10>> SceLibcBootstrapImports::HeapCallbacks() const {
    std::lock_guard lock(impl->stateMutex);
    return impl->heapCallbacks;
}

std::optional<std::uint64_t> SceLibcBootstrapImports::Resolve(const SceImport& import) {
    const auto found = impl->services.find(import.Nid);
    if (found == impl->services.end()) return std::nullopt;
    const auto service = found->second;
    const bool trace = service == Service::TraceInfo;
    const std::string_view library = trace ? "libSceLibcInternalExt" : "libkernel";
    const std::string_view module = trace ? "libSceLibcInternal" : "libkernel";
    if (import.LibraryName != library && import.ModuleName != module) return std::nullopt;
    if (import.LibraryName != library || import.ModuleName != module ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE libc bootstrap import scope/version: " + identity(import));
    if (service == Service::StackGuard) return impl->dataBase + guardOffset;
    if (service == Service::ProgramName) return impl->dataBase + programPointerOffset;
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto gate = impl->gates.find(key); gate != impl->gates.end()) return gate->second;
    if (impl->nextSlot == pageSize / 16) throw std::runtime_error("SCE libc bootstrap gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), service](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE libc bootstrap runtime has expired");
        context->invoke(guest, service);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

}
