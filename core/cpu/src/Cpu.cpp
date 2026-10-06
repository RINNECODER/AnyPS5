#include <cpu/Cpu.hpp>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Cpu {
namespace {
void check(uc_err error, const char* operation) {
    if (error != UC_ERR_OK) throw std::runtime_error(std::string(operation) + ": " + uc_strerror(error));
}
int registerId(Register reg) {
    switch (reg) {
    case Register::Rax: return UC_X86_REG_RAX;
    case Register::Rbx: return UC_X86_REG_RBX;
    case Register::Rcx: return UC_X86_REG_RCX;
    case Register::Rdx: return UC_X86_REG_RDX;
    case Register::Rsi: return UC_X86_REG_RSI;
    case Register::Rdi: return UC_X86_REG_RDI;
    case Register::Rbp: return UC_X86_REG_RBP;
    case Register::Rsp: return UC_X86_REG_RSP;
    case Register::R8: return UC_X86_REG_R8;
    case Register::R9: return UC_X86_REG_R9;
    case Register::R10: return UC_X86_REG_R10;
    case Register::R11: return UC_X86_REG_R11;
    case Register::R12: return UC_X86_REG_R12;
    case Register::R13: return UC_X86_REG_R13;
    case Register::R14: return UC_X86_REG_R14;
    case Register::R15: return UC_X86_REG_R15;
    case Register::Rip: return UC_X86_REG_RIP;
    case Register::Rflags: return UC_X86_REG_RFLAGS;
    case Register::FsBase: return UC_X86_REG_FS_BASE;
    case Register::GsBase: return UC_X86_REG_GS_BASE;
    }
    throw std::invalid_argument("Unknown guest register");
}
void checkRange(std::uint64_t address, std::size_t size) {
    if (!size || (address & 4095) || (size & 4095) || size > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Guest mapping must be a nonempty 4 KiB aligned range without overflow");
}
const char* memoryAccess(uc_mem_type type) {
    switch (type) {
    case UC_MEM_READ_UNMAPPED: return "unmapped read";
    case UC_MEM_WRITE_UNMAPPED: return "unmapped write";
    case UC_MEM_FETCH_UNMAPPED: return "unmapped instruction fetch";
    case UC_MEM_READ_PROT: return "protected read";
    case UC_MEM_WRITE_PROT: return "protected write";
    case UC_MEM_FETCH_PROT: return "protected instruction fetch";
    default: return "invalid memory access";
    }
}
}

struct Machine::Impl {
    Machine& owner;
    uc_engine* engine = nullptr;
    std::function<void(Machine&)> syscall;
    std::unordered_map<std::uint64_t, std::function<void(Machine&)>> calls;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> borrowed;
    std::exception_ptr failure;
    std::atomic<bool> requested{false};
    bool running = false;
    bool exited = false;
    bool restart = false;
    bool limited = false;
    int exitCode = 0;
    std::uint64_t executed = 0;
    std::uint64_t limit = 0;
    std::uint64_t instructionAddress = 0;
    std::uint32_t instructionSize = 0;

    explicit Impl(Machine& machine) : owner(machine) {
        check(uc_open(UC_ARCH_X86, UC_MODE_64, &engine), "Create x86-64 translator");
        try {
            std::uint16_t fpcw = 0x037f;
            std::uint32_t mxcsr = 0x1f80;
            std::uint64_t flags = 2;
            check(uc_reg_write(engine, UC_X86_REG_FPCW, &fpcw), "Initialize guest x87 control word");
            check(uc_reg_write(engine, UC_X86_REG_MXCSR, &mxcsr), "Initialize guest SSE control word");
            check(uc_reg_write(engine, UC_X86_REG_RFLAGS, &flags), "Initialize guest flags");
            uc_hook hook;
            check(uc_hook_add(engine, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(onCode), this, 1, 0), "Install execution hook");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onSyscall), this, 1, 0, UC_X86_INS_SYSCALL), "Install syscall hook");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onSysenter), this, 1, 0, UC_X86_INS_SYSENTER), "Install sysenter rejection hook");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onInput), this, 1, 0, UC_X86_INS_IN), "Install port input rejection hook");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onOutput), this, 1, 0, UC_X86_INS_OUT), "Install port output rejection hook");
            check(uc_hook_add(engine, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(onInterrupt), this, 1, 0), "Install interrupt hook");
            check(uc_hook_add(engine, &hook, UC_HOOK_MEM_INVALID, reinterpret_cast<void*>(onMemory), this, 1, 0), "Install memory fault hook");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN_INVALID, reinterpret_cast<void*>(onInvalid), this, 1, 0), "Install invalid instruction hook");
        } catch (...) {
            uc_close(engine);
            throw;
        }
    }
    ~Impl() { uc_close(engine); }
    void rejectPrivileged(std::uint64_t address, std::uint32_t size) {
        std::array<std::uint8_t,15> bytes{};
        if (!size || size > bytes.size()) return;
        check(uc_mem_read(engine, address, bytes.data(), size), "Read translated instruction");
        std::uint32_t offset = 0;
        for (; offset < size; ++offset) {
            const auto byte = bytes[offset];
            if ((byte >= 0x40 && byte <= 0x4f) || byte == 0x66 || byte == 0x67 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
                byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 || byte == 0x65) continue;
            break;
        }
        if (offset == size) return;
        const auto opcode = bytes[offset];
        if (opcode == 0xc4 || opcode == 0xc5 || opcode == 0x62) {
            std::ostringstream message;
            message << "Unsupported guest VEX/EVEX instruction at 0x" << std::hex << address;
            throw std::runtime_error(message.str());
        }
        if (opcode == 0x8f && offset + 1 < size && (bytes[offset + 1] & 0x1f) >= 8) {
            std::ostringstream message;
            message << "Unsupported guest XOP instruction at 0x" << std::hex << address;
            throw std::runtime_error(message.str());
        }
        bool unsupported = opcode == 0xf4 || opcode == 0xfa || opcode == 0xfb;
        if (opcode == 0x0f && offset + 1 < size) {
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
                                  modrm == 0xf8 || modrm == 0xc8 || modrm == 0xc9 || modrm == 0xd1;
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
    void checkStack(std::uint64_t address) {
        if (address > std::numeric_limits<std::uint64_t>::max() - 8)
            throw std::runtime_error("Guest host import return stack overflows");
        try { owner.CheckAccess(address, 8, Permission::Read); }
        catch (const std::exception&) { throw std::runtime_error("Guest host import return stack is not readable"); }
    }
    template<class Function> void guard(Function&& function) noexcept {
        try { function(); }
        catch (...) { failure = std::current_exception(); uc_emu_stop(engine); }
    }
    static void onCode(uc_engine*, std::uint64_t address, std::uint32_t size, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] {
            if (self.requested.load()) { check(uc_emu_stop(self.engine), "Stop requested execution"); return; }
            if (self.executed == self.limit) { self.limited = true; check(uc_emu_stop(self.engine), "Stop instruction limit"); return; }
            ++self.executed;
            self.instructionAddress = address;
            self.instructionSize = size;
            auto found = self.calls.find(address);
            if (found == self.calls.end()) { self.rejectPrivileged(address, size); return; }
            auto callback = found->second;
            callback(self.owner);
            if (!self.exited && !self.requested.load()) {
                const auto stack = self.owner.Get(Register::Rsp);
                self.checkStack(stack);
                std::uint64_t destination = 0;
                self.owner.Read(stack, std::as_writable_bytes(std::span(&destination, 1)));
                self.owner.Set(Register::Rsp, stack + sizeof(destination));
                self.owner.Set(Register::Rip, destination);
                self.restart = true;
            }
            check(uc_emu_stop(self.engine), "Return from host import");
        });
    }
    static void onSyscall(uc_engine*, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] {
            if (!self.syscall) throw std::runtime_error("Unsupported guest syscall " + std::to_string(self.owner.Get(Register::Rax)));
            const auto address = self.owner.Get(Register::Rip);
            if (self.instructionAddress != address || self.instructionSize < 2 || self.instructionSize > 15)
                throw std::runtime_error("Guest SYSCALL instruction length is unavailable");
            self.owner.Set(Register::Rcx, address + self.instructionSize);
            self.owner.Set(Register::R11, self.owner.Get(Register::Rflags));
            auto callback = self.syscall;
            callback(self.owner);
        });
    }
    static void onSysenter(uc_engine*, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { throw std::runtime_error("Unsupported guest SYSENTER service"); });
    }
    static std::uint32_t onInput(uc_engine*, std::uint32_t port, int, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { throw std::runtime_error("Unsupported guest port input " + std::to_string(port)); });
        return 0;
    }
    static void onOutput(uc_engine*, std::uint32_t port, int, std::uint32_t, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { throw std::runtime_error("Unsupported guest port output " + std::to_string(port)); });
    }
    static void onInterrupt(uc_engine*, std::uint32_t number, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { throw std::runtime_error("Unsupported guest interrupt " + std::to_string(number)); });
    }
    static bool onMemory(uc_engine*, uc_mem_type type, std::uint64_t address, int size, std::int64_t, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] {
            std::ostringstream message;
            message << "Guest " << memoryAccess(type) << " at 0x" << std::hex << address << " (" << std::dec << size << " bytes)";
            throw std::runtime_error(message.str());
        });
        return false;
    }
    static bool onInvalid(uc_engine*, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] {
            std::ostringstream message;
            message << "Unsupported guest instruction at 0x" << std::hex << self.owner.Get(Register::Rip);
            throw std::runtime_error(message.str());
        });
        return false;
    }
};

Machine::Machine() : impl(std::make_unique<Impl>(*this)) {}
Machine::~Machine() = default;
void Machine::Map(std::uint64_t address, std::size_t size, Permission permissions) {
    checkRange(address, size);
    check(uc_mem_map(impl->engine, address, size, static_cast<unsigned>(permissions)), "Map guest memory");
}
void Machine::MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions) {
    checkRange(address, memory.size());
    check(uc_mem_map_ptr(impl->engine, address, memory.size(), static_cast<unsigned>(permissions), memory.data()), "Map shared guest memory");
    impl->borrowed.emplace_back(address, address + memory.size());
}
void Machine::Protect(std::uint64_t address, std::size_t size, Permission permissions) {
    checkRange(address, size);
    check(uc_mem_protect(impl->engine, address, size, static_cast<unsigned>(permissions)), "Protect guest memory");
}
void Machine::Read(std::uint64_t address, std::span<std::byte> output) const {
    CheckAccess(address, output.size(), Permission::Read);
    if (!output.empty()) check(uc_mem_read(impl->engine, address, output.data(), output.size()), "Read guest memory");
}
void Machine::CheckAccess(std::uint64_t address, std::size_t size, Permission permissions) const {
    if (!size) return;
    if (size > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Guest access range overflows");
    const auto required = static_cast<unsigned>(permissions);
    if (!required || (required & ~UC_PROT_ALL)) throw std::invalid_argument("Invalid guest access permissions");
    uc_mem_region* regions = nullptr;
    std::uint32_t count = 0;
    check(uc_mem_regions(impl->engine, &regions, &count), "Inspect guest access permissions");
    struct Release { uc_mem_region* value; ~Release() { uc_free(value); } } release{regions};
    auto cursor = address;
    const auto end = address + size;
    while (cursor < end) {
        bool found = false;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& region = regions[index];
            if (cursor >= region.begin && cursor <= region.end && (region.perms & required) == required) {
                cursor = region.end >= end - 1 ? end : region.end + 1;
                found = true;
                break;
            }
        }
        if (!found) {
            std::ostringstream message;
            message << "Guest access denied at 0x" << std::hex << cursor << " for permissions " << required;
            throw std::runtime_error(message.str());
        }
    }
}
void Machine::Write(std::uint64_t address, std::span<const std::byte> input) {
    if (input.empty()) return;
    if (input.size() > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Guest write range overflows");
    check(uc_mem_write(impl->engine, address, input.data(), input.size()), "Write guest memory");
    check(uc_ctl_remove_cache(impl->engine, address, address + input.size()), "Invalidate translated guest code");
}
std::uint64_t Machine::Get(Register reg) const {
    std::uint64_t value = 0;
    check(uc_reg_read(impl->engine, registerId(reg), &value), "Read guest register");
    return value;
}
void Machine::Set(Register reg, std::uint64_t value) { check(uc_reg_write(impl->engine, registerId(reg), &value), "Write guest register"); }
void Machine::SetSyscallHandler(std::function<void(Machine&)> handler) { impl->syscall = std::move(handler); }
void Machine::AddHostCall(std::uint64_t address, std::function<void(Machine&)> handler) {
    if (!handler) throw std::invalid_argument("Guest host import requires a handler");
    if (!impl->calls.emplace(address, std::move(handler)).second) throw std::invalid_argument("Guest host import address is already registered");
}
StopReason Machine::Run(std::uint64_t entry, std::uint64_t until, std::uint64_t instructionLimit) {
    if (impl->running) throw std::logic_error("Guest execution is already running");
    if (!instructionLimit) throw std::invalid_argument("Guest execution requires a nonzero instruction limit");
    impl->running = true;
    impl->requested.store(false);
    impl->failure = nullptr;
    impl->limited = false;
    impl->exited = false;
    impl->executed = 0;
    impl->limit = instructionLimit;
    struct Reset { Impl& value; ~Reset() { value.running = false; } } reset{*impl};
    Set(Register::Rip, entry);
    for (;;) {
        impl->restart = false;
        for (const auto& [begin, end] : impl->borrowed)
            check(uc_ctl_remove_cache(impl->engine, begin, end), "Refresh borrowed guest code");
        const auto error = uc_emu_start(impl->engine, Get(Register::Rip), until, 0, 0);
        if (impl->failure) std::rethrow_exception(impl->failure);
        check(error, "Execute x86-64 guest");
        if (impl->exited) return StopReason::Exit;
        if (impl->requested.load()) return StopReason::Requested;
        if (impl->limited) return StopReason::InstructionLimit;
        if (Get(Register::Rip) == until) return StopReason::Address;
        if (!impl->restart) throw std::runtime_error("Guest execution stopped without reaching its exit or stop address");
    }
}
void Machine::Exit(int code) {
    impl->exitCode = code;
    impl->exited = true;
    check(uc_emu_stop(impl->engine), "Exit guest");
}
void Machine::RequestStop() { impl->requested.store(true); check(uc_emu_stop(impl->engine), "Request guest stop"); }
int Machine::ExitCode() const { return impl->exitCode; }
const char* Machine::Backend() { return "Unicorn 2 x86-64 dynamic translation"; }
}
