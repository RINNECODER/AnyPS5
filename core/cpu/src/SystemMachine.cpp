#include <cpu/SystemMachine.hpp>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <array>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Cpu {
namespace {
void check(uc_err error, const char* operation) {
    if (error != UC_ERR_OK) throw std::runtime_error(std::string(operation) + ": " + uc_strerror(error));
}
}

struct SystemMachine::Impl {
    uc_engine* engine = nullptr;
    std::size_t ramBytes;
    std::exception_ptr failure;
    bool running = false;
    bool halted = false;
    bool limited = false;
    std::uint64_t executed = 0;
    std::uint64_t limit = 0;

    explicit Impl(std::size_t bytes) : ramBytes(bytes) {
        if (!bytes || (bytes & 4095) || bytes > 1024ULL * 1024 * 1024)
            throw std::invalid_argument("System RAM must be a nonzero 4 KiB aligned size no larger than 1 GiB");
        check(uc_open(UC_ARCH_X86, UC_MODE_64, &engine), "Create ring0 x86-64 translator");
        try {
            check(uc_ctl_tlb_mode(engine, UC_TLB_CPU), "Enable architectural x86 MMU");
            check(uc_mem_map(engine, 0, bytes, UC_PROT_ALL), "Map system physical RAM");
            std::uint64_t flags = 2;
            check(uc_reg_write(engine, UC_X86_REG_RFLAGS, &flags), "Initialize ring0 flags");
            uc_hook hook;
            check(uc_hook_add(engine, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(onCode), this, 1, 0), "Install system execution guard");
            check(uc_hook_add(engine, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(onException), this, 1, 0), "Install explicit system exception failure");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN_INVALID, reinterpret_cast<void*>(onInvalid), this, 1, 0), "Install unsupported system instruction failure");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onSyscall), this, 1, 0, UC_X86_INS_SYSCALL), "Install unsupported system syscall guard");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onSysenter), this, 1, 0, UC_X86_INS_SYSENTER), "Install unsupported system sysenter guard");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onInput), this, 1, 0, UC_X86_INS_IN), "Install unsupported system port input guard");
            check(uc_hook_add(engine, &hook, UC_HOOK_INSN, reinterpret_cast<void*>(onOutput), this, 1, 0, UC_X86_INS_OUT), "Install unsupported system port output guard");
        } catch (...) {
            uc_close(engine);
            throw;
        }
    }
    ~Impl() { uc_close(engine); }

    std::uint64_t reg(int id) const {
        std::uint64_t value = 0;
        check(uc_reg_read(engine, id, &value), "Read system CPU state");
        return value;
    }
    void range(std::uint64_t address, std::size_t bytes) const {
        if (address > ramBytes || bytes > ramBytes - address)
            throw std::invalid_argument("System physical access exceeds RAM");
    }
    [[noreturn]] void unsupported(const char* operation) const {
        std::ostringstream message;
        message << "Unsupported system " << operation << " at RIP=0x" << std::hex << reg(UC_X86_REG_RIP);
        throw std::runtime_error(message.str());
    }
    template<class Function> void guard(Function&& function) noexcept {
        try { function(); }
        catch (...) { failure = std::current_exception(); uc_emu_stop(engine); }
    }
    static void onCode(uc_engine*, std::uint64_t address, std::uint32_t size, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] {
            if (self.executed == self.limit) {
                self.limited = true;
                check(uc_emu_stop(self.engine), "Stop system instruction limit");
                return;
            }
            ++self.executed;
            std::array<std::uint8_t, 15> bytes{};
            if (!size || size > bytes.size()) self.unsupported("instruction decode");
            check(uc_vmem_read(self.engine, address, UC_PROT_EXEC, bytes.data(), size), "Read system instruction through MMU");
            std::uint32_t offset = 0;
            std::uint8_t rex = 0;
            bool locked = false;
            for (; offset < size; ++offset) {
                const auto byte = bytes[offset];
                if (byte >= 0x40 && byte <= 0x4f) { rex = byte; continue; }
                if (byte == 0xf0) { locked = true; continue; }
                if (byte == 0x66 || byte == 0x67 || byte == 0xf2 || byte == 0xf3 || byte == 0x26 ||
                    byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 || byte == 0x65) continue;
                break;
            }
            if (offset == size) self.unsupported("instruction decode");
            const auto opcode = bytes[offset];
            if (opcode == 0xf4) self.halted = true;
            if (opcode >= 0x6c && opcode <= 0x6f) self.unsupported("string port I/O");
            if (opcode != 0x0f || offset + 1 >= size) return;
            const auto second = bytes[offset + 1];
            if (second == 0x30 || second == 0x32) {
                if (static_cast<std::uint32_t>(self.reg(UC_X86_REG_RCX)) != 0xc0000080)
                    self.unsupported("MSR operation (only EFER is supported)");
                if (second == 0x30) {
                    const auto value = static_cast<std::uint32_t>(self.reg(UC_X86_REG_RAX)) |
                        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(self.reg(UC_X86_REG_RDX))) << 32);
                    if (value & ~std::uint64_t{0xd01}) self.unsupported("EFER reserved or unsupported feature bits");
                }
            }
            if (second == 0x07 || second == 0x35) self.unsupported("privilege return service");
            if (second == 0x31 || (second == 0x01 && offset + 2 < size && bytes[offset + 2] == 0xf9))
                self.unsupported("timestamp service");
            if ((second == 0x20 || second == 0x22) && offset + 2 < size) {
                const auto control = ((bytes[offset + 2] >> 3) & 7) + ((rex & 4) ? 8 : 0);
                if (locked || (control != 0 && control != 2 && control != 3 && control != 4))
                    self.unsupported("control register/APIC operation");
                if (second == 0x22 && (control == 0 || control == 4)) {
                    constexpr std::array registers{
                        UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RBX,
                        UC_X86_REG_RSP, UC_X86_REG_RBP, UC_X86_REG_RSI, UC_X86_REG_RDI,
                        UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11,
                        UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
                    };
                    const auto source = (bytes[offset + 2] & 7) + ((rex & 1) ? 8 : 0);
                    const auto value = self.reg(registers[source]);
                    constexpr std::uint64_t paging = 1ULL << 31;
                    constexpr std::uint64_t pae = 1ULL << 5;
                    if (control == 0) {
                        constexpr std::uint64_t supported = 0xe005003f;
                        if ((value & ~supported) || ((value & paging) && !(value & 1)) ||
                            ((value & (1ULL << 29)) && !(value & (1ULL << 30))))
                            self.unsupported("CR0 value (reserved bits or invalid paging/cache configuration)");
                        if ((value & paging) && !(self.reg(UC_X86_REG_CR4) & pae))
                            self.unsupported("CR0 value (64-bit paging requires CR4.PAE)");
                    } else {
                        constexpr std::uint64_t supported = (1ULL << 4) | pae | (1ULL << 7) |
                            (1ULL << 9) | (1ULL << 10) | (1ULL << 16) | (1ULL << 18) | (1ULL << 20);
                        if ((value & ~supported) || ((self.reg(UC_X86_REG_CR0) & paging) && !(value & pae)))
                            self.unsupported("CR4 value (reserved bits or unsupported system profile)");
                    }
                }
            }
        });
    }
    static void onException(uc_engine*, std::uint32_t vector, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] {
            std::ostringstream message;
            message << "System guest exception vector=" << vector << " RIP=0x" << std::hex
                    << self.reg(UC_X86_REG_RIP) << " CR2=0x" << self.reg(UC_X86_REG_CR2)
                    << "; guest IDT exception delivery is unsupported";
            throw std::runtime_error(message.str());
        });
    }
    static bool onInvalid(uc_engine*, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { self.unsupported("instruction"); });
        return false;
    }
    static void onSyscall(uc_engine*, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { self.unsupported("SYSCALL entry"); });
    }
    static void onSysenter(uc_engine*, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { self.unsupported("SYSENTER entry"); });
    }
    static std::uint32_t onInput(uc_engine*, std::uint32_t, int, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { self.unsupported("port input"); });
        return 0;
    }
    static void onOutput(uc_engine*, std::uint32_t, int, std::uint32_t, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guard([&] { self.unsupported("port output"); });
    }
};

SystemMachine::SystemMachine(std::size_t bytes) : impl(std::make_unique<Impl>(bytes)) {}
SystemMachine::~SystemMachine() = default;
void SystemMachine::WritePhysical(std::uint64_t address, std::span<const std::byte> input) {
    impl->range(address, input.size());
    if (input.empty()) return;
    check(uc_mem_write(impl->engine, address, input.data(), input.size()), "Write system physical RAM");
    check(uc_ctl_flush_tb(impl->engine), "Invalidate system translation blocks");
    check(uc_ctl_flush_tlb(impl->engine), "Invalidate system translations");
}
void SystemMachine::ReadPhysical(std::uint64_t address, std::span<std::byte> output) const {
    impl->range(address, output.size());
    if (!output.empty()) check(uc_mem_read(impl->engine, address, output.data(), output.size()), "Read system physical RAM");
}
SystemStop SystemMachine::Run(std::uint64_t entry, std::uint64_t stack, std::uint64_t limit) {
    if (impl->running) throw std::logic_error("System guest is already running");
    if (!limit) throw std::invalid_argument("System execution requires a nonzero instruction limit");
    impl->running = true;
    struct Reset { Impl& value; ~Reset() { value.running = false; } } reset{*impl};
    impl->failure = nullptr;
    impl->halted = false;
    impl->limited = false;
    impl->executed = 0;
    impl->limit = limit;
    check(uc_reg_write(impl->engine, UC_X86_REG_RSP, &stack), "Set ring0 stack");
    const auto error = uc_emu_start(impl->engine, entry, 0, 0, 0);
    if (impl->failure) std::rethrow_exception(impl->failure);
    check(error, "Execute ring0 guest");
    if (impl->limited) return SystemStop::InstructionLimit;
    if (impl->halted) return SystemStop::Halted;
    throw std::runtime_error("System guest stopped without HLT or an instruction limit");
}
std::uint64_t SystemMachine::GetCR0() const { return impl->reg(UC_X86_REG_CR0); }
std::uint64_t SystemMachine::GetCR2() const { return impl->reg(UC_X86_REG_CR2); }
std::uint64_t SystemMachine::GetCR3() const { return impl->reg(UC_X86_REG_CR3); }
std::uint64_t SystemMachine::GetCR4() const { return impl->reg(UC_X86_REG_CR4); }
std::uint64_t SystemMachine::GetRip() const { return impl->reg(UC_X86_REG_RIP); }

}
