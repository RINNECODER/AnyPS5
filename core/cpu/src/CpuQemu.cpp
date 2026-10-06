#include <cpu/Cpu.hpp>
#include <qemu/anyps5-cpu.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

namespace Cpu {
namespace {
AnyPS5QemuRegister registerId(Register reg) {
    switch (reg) {
    case Register::Rax: return ANYPS5_QEMU_RAX;
    case Register::Rbx: return ANYPS5_QEMU_RBX;
    case Register::Rcx: return ANYPS5_QEMU_RCX;
    case Register::Rdx: return ANYPS5_QEMU_RDX;
    case Register::Rsi: return ANYPS5_QEMU_RSI;
    case Register::Rdi: return ANYPS5_QEMU_RDI;
    case Register::Rbp: return ANYPS5_QEMU_RBP;
    case Register::Rsp: return ANYPS5_QEMU_RSP;
    case Register::R8: return ANYPS5_QEMU_R8;
    case Register::R9: return ANYPS5_QEMU_R9;
    case Register::R10: return ANYPS5_QEMU_R10;
    case Register::R11: return ANYPS5_QEMU_R11;
    case Register::R12: return ANYPS5_QEMU_R12;
    case Register::R13: return ANYPS5_QEMU_R13;
    case Register::R14: return ANYPS5_QEMU_R14;
    case Register::R15: return ANYPS5_QEMU_R15;
    case Register::Rip: return ANYPS5_QEMU_RIP;
    case Register::Rflags: return ANYPS5_QEMU_RFLAGS;
    case Register::FsBase: return ANYPS5_QEMU_FS_BASE;
    case Register::GsBase: return ANYPS5_QEMU_GS_BASE;
    }
    throw std::invalid_argument("Unknown guest register");
}
void checkRange(std::uint64_t address, std::size_t size) {
    if (!size || (address & 4095) || (size & 4095) || size > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Guest mapping must be a nonempty 4 KiB aligned range without overflow");
}
unsigned permissionBits(Permission permissions) {
    const auto bits = static_cast<unsigned>(permissions);
    if (bits & ~7u) throw std::invalid_argument("Invalid guest mapping permissions");
    return bits;
}
std::size_t hostPageSize() {
    const auto size = sysconf(_SC_PAGESIZE);
    if (size <= 0) throw std::runtime_error("Cannot determine the modern guest CPU host page size");
    return static_cast<std::size_t>(size);
}
struct Free { void operator()(std::byte* value) const { std::free(value); } };
}

struct Machine::Impl {
    struct Range {
        std::uint64_t address;
        std::size_t size;
        std::byte* backing;
        unsigned permissions;
        bool borrowed;
    };
    struct Backing {
        std::byte* pointer;
        std::size_t size;
        std::uint64_t id;
        std::unique_ptr<std::byte, Free> storage;
    };
    AnyPS5QemuCpu* engine = nullptr;
    const std::thread::id owner = std::this_thread::get_id();
    std::vector<Range> ranges;
    std::vector<Backing> backings;
    std::unordered_map<std::uint64_t, std::function<void(Machine&)>> calls;
    std::function<void(Machine&)> syscall;
    std::atomic<bool> requested{false};
    bool running = false;
    bool exited = false;
    int exitCode = 0;

    Impl() {
        std::array<char, 512> error{};
        engine = anyps5_qemu_cpu_create(error.data(), error.size());
        if (!engine) throw std::runtime_error(std::string("Create modern x86-64 translator: ") + error.data());
    }
    ~Impl() {
        if (anyps5_qemu_cpu_destroy(engine)) std::terminate();
    }
    void check(int result, const char* operation) const {
        if (result) throw std::runtime_error(std::string(operation) + ": " + anyps5_qemu_cpu_error(engine));
    }
    void checkOwner() const {
        if (std::this_thread::get_id() != owner) throw std::logic_error("Modern guest CPU requires its owner thread");
    }
    void checkMapped(std::uint64_t address, std::size_t size, unsigned required = 0) const {
        checkOwner();
        if (!size) return;
        if (size > std::numeric_limits<std::uint64_t>::max() - address)
            throw std::invalid_argument("Guest access range overflows");
        auto cursor = address;
        const auto end = address + size;
        while (cursor < end) {
            const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const Range& range) {
                return cursor >= range.address && cursor - range.address < range.size &&
                    (range.permissions & required) == required;
            });
            if (found == ranges.end()) {
                std::ostringstream message;
                message << "Guest access denied at 0x" << std::hex << cursor << " for permissions " << required;
                throw std::runtime_error(message.str());
            }
            cursor = std::min(end, found->address + found->size);
        }
    }
    void transfer(std::uint64_t address, std::byte* bytes, std::size_t size, bool write) const {
        while (size) {
            const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const Range& range) {
                return address >= range.address && address - range.address < range.size;
            });
            if (found == ranges.end()) throw std::logic_error("Guest memory preflight did not cover the transfer");
            const auto offset = address - found->address;
            const auto chunk = std::min<std::size_t>(size, found->size - offset);
            if (write) std::memcpy(found->backing + offset, bytes, chunk);
            else std::memcpy(bytes, found->backing + offset, chunk);
            address += chunk;
            bytes += chunk;
            size -= chunk;
        }
    }
    void map(std::uint64_t address, std::span<std::byte> memory, unsigned permissions,
             bool borrowed, std::size_t allocationSize, std::unique_ptr<std::byte, Free> storage = {},
             std::span<std::byte> fullBacking = {}) {
        checkOwner();
        const auto hostPage = hostPageSize();
        for (const auto& range : ranges)
            if (address < range.address + range.size && range.address < address + memory.size())
                throw std::invalid_argument("Map guest memory: guest mappings overlap");
        const auto pointer = reinterpret_cast<std::uintptr_t>(memory.data());
        const auto allocation = fullBacking.empty() ? memory.data() : fullBacking.data();
        const auto allocationPointer = reinterpret_cast<std::uintptr_t>(allocation);
        if (allocationSize > std::numeric_limits<std::uintptr_t>::max() - allocationPointer)
            throw std::invalid_argument("Guest backing memory range overflows");
        std::uint64_t backingId = 0;
        std::size_t offset = pointer - allocationPointer;
        const auto searchedPointer = fullBacking.empty() ? pointer : allocationPointer;
        const auto searchedSize = fullBacking.empty() ? memory.size() : allocationSize;
        for (const auto& backing : backings) {
            const auto begin = reinterpret_cast<std::uintptr_t>(backing.pointer);
            if (searchedPointer >= begin && searchedPointer - begin <= backing.size && searchedSize <= backing.size - (searchedPointer - begin)) {
                backingId = backing.id;
                offset = pointer - begin;
                break;
            }
            if (searchedPointer < begin + backing.size && begin < searchedPointer + searchedSize)
                throw std::runtime_error("Guest borrowed mapping crosses a registered backing allocation");
        }
        const auto newBacking = backingId == 0;
        if (newBacking && (allocationSize % hostPage || allocationPointer % hostPage))
            throw std::runtime_error("Modern guest CPU borrowed mapping requires a complete aligned host-page backing or a slice of a registered backing");
        if (offset & 4095) throw std::runtime_error("Modern guest CPU backing aliases require a 4 KiB aligned offset");
        ranges.reserve(ranges.size() + 1);
        if (newBacking) {
            backings.reserve(backings.size() + 1);
            check(anyps5_qemu_cpu_register_backing(engine, allocation, allocationSize, &backingId), "Register modern guest backing");
        }
        try {
            check(anyps5_qemu_cpu_map_alias(engine, address, backingId, offset, memory.size(), permissions), "Map modern guest backing alias");
        } catch (...) {
            if (newBacking && anyps5_qemu_cpu_release_backing(engine, backingId)) std::terminate();
            throw;
        }
        if (newBacking) backings.push_back({allocation, allocationSize, backingId, std::move(storage)});
        ranges.push_back({address, memory.size(), memory.data(), permissions, borrowed});
    }
    void refresh() const {
        check(anyps5_qemu_cpu_invalidate(engine), "Invalidate modern translated guest code");
    }
    std::vector<Range> withoutRange(std::uint64_t address, std::size_t size) const {
        std::vector<Range> result;
        result.reserve(ranges.size() + 2);
        const auto end = address + size;
        for (const auto& range : ranges) {
            const auto rangeEnd = range.address + range.size;
            if (range.address >= end || rangeEnd <= address) result.push_back(range);
            else {
                if (range.address < address)
                    result.push_back({range.address, static_cast<std::size_t>(address - range.address), range.backing, range.permissions, range.borrowed});
                if (rangeEnd > end)
                    result.push_back({end, static_cast<std::size_t>(rangeEnd - end), range.backing + (end - range.address), range.permissions, range.borrowed});
            }
        }
        return result;
    }
    void removeCalls(std::uint64_t address, std::size_t size) {
        const auto end = address + size;
        for (auto call = calls.begin(); call != calls.end(); ) {
            if (call->first >= address && call->first < end) call = calls.erase(call);
            else ++call;
        }
    }
    void releaseUnused() {
        for (auto backing = backings.begin(); backing != backings.end(); ) {
            const auto begin = reinterpret_cast<std::uintptr_t>(backing->pointer);
            const auto used = std::any_of(ranges.begin(), ranges.end(), [&](const Range& range) {
                const auto pointer = reinterpret_cast<std::uintptr_t>(range.backing);
                return pointer >= begin && pointer - begin < backing->size;
            });
            if (used) ++backing;
            else {
                check(anyps5_qemu_cpu_release_backing(engine, backing->id), "Release unused modern guest backing");
                backing = backings.erase(backing);
            }
        }
    }
    void rejectPrivileged(std::uint64_t address, std::uint64_t port) const {
        std::array<std::uint8_t, 15> bytes{};
        std::size_t size = 0;
        for (; size < bytes.size() && size <= std::numeric_limits<std::uint64_t>::max() - address; ++size) {
            const auto cursor = address + size;
            const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const Range& range) {
                return cursor >= range.address && cursor - range.address < range.size && (range.permissions & 4);
            });
            if (found == ranges.end()) break;
            bytes[size] = std::to_integer<std::uint8_t>(found->backing[cursor - found->address]);
        }
        std::size_t offset = 0;
        for (; offset < size; ++offset) {
            const auto byte = bytes[offset];
            if ((byte >= 0x40 && byte <= 0x4f) || byte == 0x66 || byte == 0x67 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
                byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 || byte == 0x65) continue;
            break;
        }
        if (offset == size) return;
        const auto opcode = bytes[offset];
        const auto input = opcode == 0xe4 || opcode == 0xe5 || opcode == 0xec || opcode == 0xed || opcode == 0x6c || opcode == 0x6d;
        const auto output = opcode == 0xe6 || opcode == 0xe7 || opcode == 0xee || opcode == 0xef || opcode == 0x6e || opcode == 0x6f;
        if (input || output) {
            if (opcode >= 0xe4 && opcode <= 0xe7) {
                if (offset + 1 >= size) return;
                port = bytes[offset + 1];
            } else port &= 0xffff;
            throw std::runtime_error(std::string(input ? "Unsupported guest port input " : "Unsupported guest port output ") + std::to_string(port));
        }
        bool unsupported = opcode == 0xf4 || opcode == 0xfa || opcode == 0xfb;
        if (opcode == 0x0f && offset + 1 < size) {
            if (bytes[offset + 1] == 0x34) throw std::runtime_error("Unsupported guest SYSENTER service");
            switch (bytes[offset + 1]) {
            case 0x06: case 0x07: case 0x08: case 0x09:
            case 0x20: case 0x21: case 0x22: case 0x23: case 0x30: case 0x32: case 0x35:
                unsupported = true;
                break;
            case 0x00:
                if (offset + 2 < size) {
                    const auto group = (bytes[offset + 2] >> 3) & 7;
                    unsupported = group == 2 || group == 3;
                }
                break;
            case 0x01:
                if (offset + 2 < size) {
                    const auto modrm = bytes[offset + 2];
                    const auto group = (modrm >> 3) & 7;
                    const auto memory = (modrm & 0xc0) != 0xc0;
                    unsupported = (memory && (group == 2 || group == 3 || group == 7)) || group == 6 ||
                        modrm == 0xf8 || modrm == 0xc8 || modrm == 0xc9;
                }
                break;
            default: break;
            }
        }
        if (unsupported) {
            std::ostringstream message;
            message << "Unsupported guest privileged service instruction at 0x" << std::hex << address;
            throw std::runtime_error(message.str());
        }
    }
};

Machine::Machine() : impl(std::make_unique<Impl>()) {}
Machine::~Machine() = default;
void Machine::Map(std::uint64_t address, std::size_t size, Permission permissions) {
    checkRange(address, size);
    const auto bits = permissionBits(permissions);
    const auto hostPage = hostPageSize();
    if (size > std::numeric_limits<std::size_t>::max() - (hostPage - 1)) throw std::invalid_argument("Guest backing allocation size overflows");
    const auto allocationSize = ((size + hostPage - 1) / hostPage) * hostPage;
    void* pointer = nullptr;
    if (posix_memalign(&pointer, hostPage, allocationSize)) throw std::bad_alloc();
    std::unique_ptr<std::byte, Free> storage(static_cast<std::byte*>(pointer));
    std::memset(pointer, 0, allocationSize);
    const auto memory = std::span(storage.get(), size);
    impl->map(address, memory, bits, false, allocationSize, std::move(storage));
}
void Machine::MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions) {
    checkRange(address, memory.size());
    if (!memory.data()) throw std::invalid_argument("Guest borrowed mapping requires backing memory");
    impl->map(address, memory, permissionBits(permissions), true, memory.size());
}
void Machine::MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions,
                          std::span<std::byte> fullBacking) {
    checkRange(address, memory.size());
    const auto bits = permissionBits(permissions);
    const auto pointer = reinterpret_cast<std::uintptr_t>(memory.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(fullBacking.data());
    const auto hostPage = hostPageSize();
    if (!memory.data() || !fullBacking.data() || fullBacking.empty() ||
        memory.size() > std::numeric_limits<std::uintptr_t>::max() - pointer ||
        fullBacking.size() > std::numeric_limits<std::uintptr_t>::max() - begin ||
        (pointer & 4095) || begin % hostPage || fullBacking.size() % hostPage ||
        pointer < begin || pointer - begin > fullBacking.size() ||
        memory.size() > fullBacking.size() - (pointer - begin))
        throw std::invalid_argument("Guest borrowed mapping requires an aligned complete backing containing the mapped span without overflow");
    impl->map(address, memory, bits, true, fullBacking.size(), {}, fullBacking);
}
void Machine::Unmap(std::uint64_t address, std::size_t size) {
    checkRange(address, size);
    impl->checkMapped(address, size);
    auto replacement = impl->withoutRange(address, size);
    impl->check(anyps5_qemu_cpu_unmap_range(impl->engine, address, size), "Unmap modern guest memory");
    impl->ranges.swap(replacement);
    impl->removeCalls(address, size);
    impl->releaseUnused();
}
void Machine::ReplaceBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions) {
    checkRange(address, memory.size());
    if (!memory.data()) throw std::invalid_argument("Guest borrowed replacement requires backing memory");
    const auto bits = permissionBits(permissions);
    impl->checkMapped(address, memory.size());
    const auto pointer = reinterpret_cast<std::uintptr_t>(memory.data());
    if (memory.size() > std::numeric_limits<std::uintptr_t>::max() - pointer)
        throw std::invalid_argument("Guest backing memory range overflows");
    auto replacement = impl->withoutRange(address, memory.size());
    replacement.push_back({address, memory.size(), memory.data(), bits, true});
    std::uint64_t backingId = 0;
    std::size_t offset = 0;
    for (const auto& backing : impl->backings) {
        const auto begin = reinterpret_cast<std::uintptr_t>(backing.pointer);
        if (pointer >= begin && pointer - begin <= backing.size && memory.size() <= backing.size - (pointer - begin)) {
            backingId = backing.id;
            offset = pointer - begin;
            break;
        }
        if (pointer < begin + backing.size && begin < pointer + memory.size())
            throw std::runtime_error("Guest borrowed replacement crosses a registered backing allocation");
    }
    const auto newBacking = backingId == 0;
    const auto hostPage = hostPageSize();
    if (newBacking && (memory.size() % hostPage || pointer % hostPage))
        throw std::runtime_error("Modern guest CPU borrowed replacement requires a complete aligned host-page backing or a slice of a registered backing");
    if (offset & 4095) throw std::runtime_error("Modern guest CPU backing aliases require a 4 KiB aligned offset");
    if (newBacking) {
        impl->backings.reserve(impl->backings.size() + 1);
        impl->check(anyps5_qemu_cpu_register_backing(impl->engine, memory.data(), memory.size(), &backingId), "Register modern replacement backing");
    }
    try {
        impl->check(anyps5_qemu_cpu_replace_alias(impl->engine, address, backingId, offset, memory.size(), bits), "Replace modern guest backing alias");
    } catch (...) {
        if (newBacking && anyps5_qemu_cpu_release_backing(impl->engine, backingId)) std::terminate();
        throw;
    }
    if (newBacking) impl->backings.push_back({memory.data(), memory.size(), backingId, {}});
    impl->ranges.swap(replacement);
    impl->removeCalls(address, memory.size());
    impl->releaseUnused();
}
void Machine::Protect(std::uint64_t address, std::size_t size, Permission permissions) {
    checkRange(address, size);
    const auto bits = permissionBits(permissions);
    impl->checkMapped(address, size);
    std::vector<Impl::Range> replacement;
    replacement.reserve(impl->ranges.size() + 2);
    const auto end = address + size;
    for (const auto& range : impl->ranges) {
        const auto rangeEnd = range.address + range.size;
        if (range.address >= end || rangeEnd <= address) { replacement.push_back(range); continue; }
        const auto begin = std::max(address, range.address);
        const auto overlapEnd = std::min(end, rangeEnd);
        if (range.address < begin)
            replacement.push_back({range.address, static_cast<std::size_t>(begin - range.address), range.backing, range.permissions, range.borrowed});
        replacement.push_back({begin, static_cast<std::size_t>(overlapEnd - begin), range.backing + (begin - range.address), bits, range.borrowed});
        if (overlapEnd < rangeEnd)
            replacement.push_back({overlapEnd, static_cast<std::size_t>(rangeEnd - overlapEnd), range.backing + (overlapEnd - range.address), range.permissions, range.borrowed});
    }
    impl->check(anyps5_qemu_cpu_protect_range(impl->engine, address, size, bits), "Protect modern guest memory");
    impl->ranges.swap(replacement);
}
std::vector<Mapping> Machine::Mappings() const {
    impl->checkOwner();
    std::vector<Mapping> result;
    result.reserve(impl->ranges.size());
    for (const auto& range : impl->ranges)
        result.push_back({range.address, range.size, static_cast<Permission>(range.permissions), range.borrowed});
    std::sort(result.begin(), result.end(), [](const Mapping& left, const Mapping& right) { return left.Address < right.Address; });
    return result;
}
void Machine::CheckAccess(std::uint64_t address, std::size_t size, Permission permissions) const {
    if (!size) return;
    const auto bits = permissionBits(permissions);
    if (!bits) throw std::invalid_argument("Invalid guest access permissions");
    impl->checkMapped(address, size, bits);
}
void Machine::Read(std::uint64_t address, std::span<std::byte> output) const {
    CheckAccess(address, output.size(), Permission::Read);
    impl->transfer(address, output.data(), output.size(), false);
}
void Machine::Write(std::uint64_t address, std::span<const std::byte> input) {
    if (input.empty()) return;
    impl->checkMapped(address, input.size());
    impl->transfer(address, const_cast<std::byte*>(input.data()), input.size(), true);
    impl->refresh();
}
std::uint64_t Machine::Get(Register reg) const {
    std::uint64_t value = 0;
    impl->check(anyps5_qemu_cpu_get(impl->engine, registerId(reg), &value), "Read modern guest register");
    return value;
}
void Machine::Set(Register reg, std::uint64_t value) {
    impl->check(anyps5_qemu_cpu_set(impl->engine, registerId(reg), value), "Write modern guest register");
}
void Machine::SetSyscallHandler(std::function<void(Machine&)> handler) {
    impl->checkOwner();
    impl->syscall = std::move(handler);
}
void Machine::AddHostCall(std::uint64_t address, std::function<void(Machine&)> handler) {
    if (!handler) throw std::invalid_argument("Guest host import requires a handler");
    impl->checkOwner();
    if (impl->calls.contains(address)) throw std::invalid_argument("Guest host import address is already registered");
    CheckAccess(address, 1, Permission::Execute);
    impl->calls.emplace(address, std::move(handler));
    try {
        impl->check(anyps5_qemu_cpu_add_gate(impl->engine, address, address), "Register modern guest host gate");
    } catch (...) {
        impl->calls.erase(address);
        throw;
    }
}
StopReason Machine::Run(std::uint64_t entry, std::uint64_t until, std::uint64_t instructionLimit) {
    impl->checkOwner();
    if (impl->running) throw std::logic_error("Guest execution is already running");
    if (!instructionLimit) throw std::invalid_argument("Guest execution requires a nonzero instruction limit");
    impl->check(anyps5_qemu_cpu_clear_stop(impl->engine), "Reset modern guest stop request");
    impl->running = true;
    impl->requested.store(false);
    impl->exited = false;
    struct Reset { Impl& value; ~Reset() { value.running = false; } } reset{*impl};
    Set(Register::Rip, entry);
    std::uint64_t executed = 0;
    for (;;) {
        if (impl->exited) return StopReason::Exit;
        if (impl->requested.load()) return StopReason::Requested;
        if (Get(Register::Rip) == until) return StopReason::Address;
        if (executed == instructionLimit) return StopReason::InstructionLimit;
        if (std::any_of(impl->ranges.begin(), impl->ranges.end(), [](const Impl::Range& range) { return range.borrowed; })) impl->refresh();
        AnyPS5QemuRunResult result{};
        impl->check(anyps5_qemu_cpu_run_until(impl->engine, instructionLimit - executed, until, &result), "Execute modern x86-64 guest");
        if (result.instructions > instructionLimit - executed) throw std::runtime_error("Modern guest CPU exceeded its instruction budget");
        executed += result.instructions;
        switch (result.reason) {
        case ANYPS5_QEMU_BUDGET:
            if (!result.instructions) throw std::runtime_error("Modern guest CPU budget stop made no progress");
            break;
        case ANYPS5_QEMU_HOST_GATE: {
            ++executed;
            const auto found = impl->calls.find(result.gate);
            if (found == impl->calls.end()) throw std::runtime_error("Modern guest CPU returned an unknown host gate");
            auto callback = found->second;
            callback(*this);
            if (!impl->exited && !impl->requested.load()) {
                const auto stack = Get(Register::Rsp);
                if (stack > std::numeric_limits<std::uint64_t>::max() - 8)
                    throw std::runtime_error("Guest host import return stack overflows");
                try { CheckAccess(stack, 8, Permission::Read); }
                catch (const std::exception&) { throw std::runtime_error("Guest host import return stack is not readable"); }
                std::uint64_t destination = 0;
                Read(stack, std::as_writable_bytes(std::span(&destination, 1)));
                Set(Register::Rsp, stack + sizeof(destination));
                Set(Register::Rip, destination);
            }
            break;
        }
        case ANYPS5_QEMU_SYSCALL: {
            if (!impl->syscall) throw std::runtime_error("Unsupported guest syscall " + std::to_string(Get(Register::Rax)));
            auto callback = impl->syscall;
            callback(*this);
            break;
        }
        case ANYPS5_QEMU_REQUESTED_STOP:
            impl->requested.store(true);
            return StopReason::Requested;
        case ANYPS5_QEMU_STOP_ADDRESS:
            if (result.rip != until) throw std::runtime_error("Modern guest CPU stopped at an unexpected address");
            return StopReason::Address;
        case ANYPS5_QEMU_FAULT: {
            std::ostringstream message;
            message << "Guest " << ((result.error_code & 1) ? "protected " : "unmapped ")
                << ((result.error_code & 16) ? "instruction fetch" : ((result.error_code & 2) ? "write" : "read"))
                << " at 0x" << std::hex << result.address << " (vector " << std::dec << result.vector << ")";
            throw std::runtime_error(message.str());
        }
        case ANYPS5_QEMU_UNSUPPORTED: {
            if (result.vector == 13) impl->rejectPrivileged(result.rip, Get(Register::Rdx));
            std::ostringstream message;
            if (result.vector == 6) message << "Unsupported guest instruction";
            else if (result.vector == 13) message << "Unsupported guest privileged service instruction";
            else message << "Unsupported guest interrupt " << result.vector;
            message << " at 0x" << std::hex << result.rip;
            throw std::runtime_error(message.str());
        }
        default:
            throw std::runtime_error("Modern guest CPU returned an unknown stop reason");
        }
    }
}
void Machine::Exit(int code) {
    impl->checkOwner();
    impl->exitCode = code;
    impl->exited = true;
}
void Machine::RequestStop() {
    impl->requested.store(true);
    anyps5_qemu_cpu_stop(impl->engine);
}
int Machine::ExitCode() const { return impl->exitCode; }
const char* Machine::Backend() { return "Modern QEMU TCG x86-64 dynamic translation"; }
}
