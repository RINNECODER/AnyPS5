#include <cpu/GuestMemoryRuntime.hpp>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t page = 0x4000;
constexpr std::uint64_t first = 0x1000000000;
constexpr std::uint64_t whole = first + 0x20000;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Function> void kernelError(Function&& function, unsigned code) {
    try { function(); }
    catch (const Cpu::GuestMemoryError& error) {
        const auto expected = static_cast<std::int64_t>(static_cast<std::int32_t>(0x80020000u | code));
        require(error.Result() == expected, "Guest memory returned the wrong signed kernel error"); return;
    }
    throw std::runtime_error("Guest memory operation did not return an expected kernel error");
}
template<class Function> void failure(Function&& function, const char* text) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(text) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error("Missing guest memory rejection");
}
std::byte read(Cpu::Machine& machine, std::uint64_t address) {
    std::byte byte{}; machine.Read(address, std::span(&byte, 1)); return byte;
}
void append64(std::vector<std::uint8_t>& code, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) code.push_back((value >> (i * 8)) & 255);
}
void secondPageFirstAliases() {
    Cpu::Machine machine;
    machine.Map(0x1000, 4096, rx);
    Cpu::GuestMemoryRuntime runtime(machine, 8 * page);
    const auto physical = runtime.AllocateDirect(0, 8 * page, 2 * page, page, 0);
    require(physical == 0, "Fresh direct allocation did not begin at the independently expected physical offset");
    require(runtime.MapDirect(first, page, 2, 0x90, page, page) == first, "Second-page-first exact VA mapping failed");
    require(runtime.MapDirect(whole, 2 * page, 2, 0x90, physical, page) == whole, "Later full physical alias did not map");
    require(read(machine, first + 0x38) == std::byte{0} && read(machine, whole + 0x38) == std::byte{0},
            "Physical memory is not demand-zero before guest execution");
    std::vector<std::uint8_t> code{0x48, 0xb8}; append64(code, first + 0x38);
    code.insert(code.end(), {0xc6, 0x00, 0x5a, 0x48, 0xba}); append64(code, whole + page + 0x38);
    code.insert(code.end(), {0x0f, 0xb6, 0x0a, 0xc6, 0x42, 0x01, 0xa7, 0x0f, 0xb6, 0x70, 0x01,
        0x83, 0xf9, 0x5a, 0x75, 0x07, 0xbb, 0x78, 0x56, 0x34, 0x12, 0xeb, 0x02, 0x31, 0xdb});
    machine.Write(0x1000, std::as_bytes(std::span(code)));
    machine.Set(Register::Rflags, 2);
    require(machine.Run(0x1000, 0x1000 + code.size(), 100) == Cpu::StopReason::Address,
            "Physical alias proof did not finish through actual x86 control flow");
    require(machine.Get(Register::Rcx) == 0x5a && machine.Get(Register::Rsi) == 0xa7 &&
            machine.Get(Register::Rbx) == 0x12345678, "Actual x86 did not observe bidirectional shared aliases");
    require((machine.Get(Register::Rflags) & 0x8d5) == 0x44, "Alias comparison produced incorrect independently expected x86 flags");
    require(read(machine, first + 0x39) == std::byte{0xa7} && read(machine, whole + page + 0x38) == std::byte{0x5a},
            "Guest stores did not update common physical bytes");
    const auto query = runtime.Query(first + 0x38);
    require(query.Start == first && query.End == first + page && query.Offset == page && query.Protection == 2 && query.Flags == 0x12,
            "Direct query lost raw protection or actual physical offset");
    const auto snapshot = runtime.Snapshot();
    require(snapshot.Views.size() == 2 && snapshot.Views[0].PhysicalId == snapshot.Views[1].PhysicalId &&
            snapshot.Views[0].PhysicalOffset == page && snapshot.Views[1].PhysicalOffset == 0 &&
            snapshot.Views[0].Bytes.data() == snapshot.Views[1].Bytes.data() + page,
            "Published GPU views do not preserve actual physical identity and byte aliasing");
    runtime.Unmap(first, page);
    runtime.Unmap(whole, 2 * page);
    runtime.ReleaseDirect(physical, 2 * page);
    require(runtime.AvailableDirect(0, 8 * page, page).Size == 8 * page,
            "Released physical allocation did not restore the configured pool");
}

void truthfulIntervalsAndFailures() {
    Cpu::Machine machine;
    machine.Map(0x8000, 4096, rx);
    machine.Map(first + 0x10000, 4096, rw);
    Cpu::GuestMemoryRuntime runtime(machine, 8 * page);
    require(runtime.DirectMemorySize() == 8 * page, "Direct capacity was invented from host resources");
    const auto physical = runtime.AllocateDirect(0, 8 * page, 3 * page, page, 12);
    runtime.MapDirect(first, 3 * page, 0x32, 0x90, physical, page);
    const auto stableGeneration = runtime.Snapshot().Generation;
    kernelError([&] { runtime.MapDirect(first + 0x10000, page, 2, 0x90, physical, page); }, 17);
    failure([&] { runtime.MapDirect(whole, page, 0xf2, 0x90, physical, page); }, "Unsupported guest memory protection");
    failure([&] { runtime.MapDirect(whole, page, 2, 2, physical, page); }, "Unsupported guest memory mapping flags");
    kernelError([&] { runtime.MapDirect(whole, page, 4, 0x90, physical, page); }, 22);
    kernelError([&] { runtime.MapDirect(whole, page, 2, 0x90, 7 * page, page); }, 22);
    kernelError([&] { runtime.MapDirect(whole, page, 2, 0x90, physical, page, 0x8000); }, 14);
    kernelError([&] { runtime.Protect(first - page, 2 * page, 1); }, 13);
    kernelError([&] { runtime.Unmap(first + 2 * page, 2 * page); }, 13);
    failure([&] { runtime.ReleaseDirect(physical, page); }, "Unsupported guest memory release of mapped");
    kernelError([&] { runtime.AllocateDirect(0, 8 * page, 6 * page, page, 0); }, 35);
    kernelError([&] { runtime.AllocateDirect(-1, 8 * page, page, page, 0); }, 22);
    kernelError([&] { runtime.AllocateDirect(0, 9 * page, page, page, 0); }, 22);
    kernelError([&] { runtime.AllocateDirect(0, 8 * page, page, 3 * page, 0); }, 22);
    kernelError([&] { runtime.Reserve(std::numeric_limits<std::uint64_t>::max() - page + 1, page, 0x90, page); }, 22);
    require(runtime.Snapshot().Generation == stableGeneration && runtime.AvailableDirect(0, 8 * page, page).Address == 3 * page,
            "Rejected memory operations changed allocator or published generation");
    runtime.Protect(first + page, page, 0x10);
    const auto protectedMiddle = runtime.Query(first + page + 1);
    require(protectedMiddle.Start == first + page && protectedMiddle.End == first + 2 * page &&
            protectedMiddle.Offset == page && protectedMiddle.Protection == 0x10 && protectedMiddle.MemoryType == 12,
            "Protection query is not split at the actual permission boundary");
    failure([&] { machine.CheckAccess(first + page, 1, Permission::Read); }, "Guest access denied");
    runtime.Unmap(first + page, page);
    kernelError([&] { runtime.Query(first + page); }, 13);
    require(runtime.Query(first).End == first + page && runtime.Query(first + 2 * page).Offset == 2 * page,
            "Partial unmap clipped or lost surviving physical offsets");
    runtime.ReleaseDirect(page, page);
    const auto available = runtime.AvailableDirect(0, 8 * page, page);
    require(available.Address == page && available.Size == page, "Partial physical free reported a fabricated available run");
    require(runtime.AllocateDirect(0, 8 * page, page, page, 0) == page, "Released physical gap was not reused");
    runtime.MapDirect(whole, page, 2, 0x90, page, page);
    require(read(machine, whole) == std::byte{0}, "Reallocated physical pages retained stale bytes");
    const auto views = runtime.Snapshot().Views;
    require(views.front().PhysicalId != views.back().PhysicalId, "Reused physical address retained a stale backing identity");
    const auto reserved = runtime.Reserve(first + 0x40000, 2 * page, 0x90, page);
    const auto reservation = runtime.Query(reserved);
    require(reservation.Start == reserved && reservation.End == reserved + 2 * page && reservation.Flags == 0,
            "Reserved address space was reported committed");
    failure([&] { runtime.MapDirect(reserved, page, 2, 0x10, 0, page); }, "Unsupported guest memory fixed replacement");
    kernelError([&] { runtime.MapDirect(reserved, page, 2, 0x90, 0, page); }, 17);
    require(runtime.Query(0, true).Start == 0x8000, "Find-next query omitted a lower pinned Machine mapping");
    runtime.Protect(whole, page, 4);
    constexpr std::array<std::uint8_t, 6> executed{0xb8, 0x2a, 0, 0, 0, 0x90};
    machine.Write(whole, std::as_bytes(std::span(executed)));
    require(machine.Run(whole, whole + executed.size(), 10) == Cpu::StopReason::Address && machine.Get(Register::Rax) == 42,
            "Executable protection did not execute actual translated direct-memory bytes");
    const auto serialized = runtime.Query(first).Serialize();
    constexpr std::array<std::uint8_t, 36> expected{
        0,0,0,0,0x10,0,0,0, 0,0x40,0,0,0x10,0,0,0, 0,0,0,0,0,0,0,0,
        0x32,0,0,0, 12,0,0,0, 0x12,0,0,0};
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(serialized[i] == std::byte(expected[i]), "Virtual-query wire bytes disagree with independently encoded ABI fields");
    for (std::size_t i = expected.size(); i < serialized.size(); ++i)
        require(serialized[i] == std::byte{0}, "Virtual-query reserved/name/padding bytes were not zeroed");
    runtime.Shutdown();
    require(machine.Mappings().size() == 2 && machine.Mappings()[0].Address == 0x8000 &&
            machine.Mappings()[1].Address == first + 0x10000, "Shutdown removed pinned memory or left runtime aliases live");
}

void gpuTransactionLifetime() {
    Cpu::Machine machine;
    enum class Mode { Normal, Before, After, Omit } mode = Mode::Normal;
    std::vector<std::shared_ptr<void>> consumerLeases;
    std::weak_ptr<void> retired;
    std::size_t calls = 0;
    Cpu::GuestMemoryRuntime runtime(machine, 4 * page,
        [&](const auto& previous, const auto& next, const auto& mutation) {
            ++calls;
            require(next.Generation > previous.Generation, "GPU candidate generation did not increase");
            if (mode == Mode::Before) throw std::runtime_error("before mutation");
            if (mode == Mode::Omit) return;
            if (mode == Mode::After) {
                consumerLeases = previous.Owners;
                consumerLeases.insert(consumerLeases.end(), next.Owners.begin(), next.Owners.end());
                retired = next.Owners.back();
                mutation(); throw std::runtime_error("after mutation");
            }
            mutation();
        });
    runtime.MapFlexible(first, page, 2, 0x90);
    const auto generation = runtime.Snapshot().Generation;
    const auto before = calls;
    mode = Mode::Before;
    failure([&] { runtime.MapFlexible(whole, page, 2, 0x90); }, "before mutation");
    require(calls == before + 1 && runtime.Snapshot().Generation == generation && machine.Mappings().size() == 1,
            "GPU rejection changed CPU mappings or published registry");
    mode = Mode::Omit;
    failure([&] { runtime.MapFlexible(whole, page, 2, 0x90); }, "did not execute CPU mutation");
    require(machine.Mappings().size() == 1 && runtime.Snapshot().Generation == generation,
            "Missing synchronous CPU mutation published fabricated memory success");
    mode = Mode::After;
    failure([&] { runtime.MapFlexible(whole, page, 2, 0x90); }, "after mutation");
    require(!retired.expired() && machine.Mappings().size() == 2, "Post-mutation failure freed backing while CPU/GPU could access it");
    failure([&] { runtime.Query(first); }, "shutdown required");
    runtime.Shutdown();
    require(machine.Mappings().empty() && !retired.expired(), "Terminal cleanup freed GPU-leased bytes before consumer shutdown");
    consumerLeases.clear();
    require(retired.expired(), "Retired physical owner did not release after consumer shutdown");
}

void transactionReentrancy() {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime* active = nullptr;
    enum class Mode { Normal, NestedMutation, NestedShutdown, BeforeFailure } mode = Mode::Normal;
    std::weak_ptr<void> outerOwner;
    std::size_t mutations = 0;
    Cpu::GuestMemoryRuntime runtime(machine, 4 * page,
        [&](const auto&, const auto& next, const auto& mutation) {
            const auto attempt = mode;
            mode = Mode::Normal;
            if (attempt == Mode::NestedMutation) {
                require(active->Query(first).Start == first && !active->Snapshot().Owners.empty(),
                        "GPU transaction cannot inspect the published read-only memory inventory");
                failure([&] { active->MapFlexible(first + 0x60000, page, 2, 0x90); }, "transaction is already in progress");
                failure([&] { active->AllocateDirect(0, 4 * page, page, page, 0); }, "transaction is already in progress");
            }
            if (attempt == Mode::NestedShutdown)
                failure([&] { active->Shutdown(); }, "transaction is already in progress");
            if (attempt == Mode::BeforeFailure) throw std::runtime_error("reset guard after failure");
            if (!next.Owners.empty()) outerOwner = next.Owners.back();
            mutation(); ++mutations;
        });
    active = &runtime;
    runtime.MapFlexible(first, page, 2, 0x90);
    const std::byte sentinel{0xa3}; machine.Write(first + 17, std::span(&sentinel, 1));
    mode = Mode::NestedMutation;
    runtime.MapFlexible(whole, page, 2, 0x90);
    require(mutations == 2 && machine.Mappings().size() == 2 && runtime.Snapshot().Views.size() == 2 &&
            !outerOwner.expired() && read(machine, first + 17) == sentinel,
            "Nested mutation corrupted outer publication, retained bytes, or backing lifetime");
    kernelError([&] { runtime.Query(first + 0x60000); }, 13);
    require(runtime.AvailableDirect(0, 4 * page, page).Size == 4 * page,
            "Nested physical allocation changed the pool before reentrancy rejection");
    mode = Mode::NestedShutdown;
    runtime.MapFlexible(first + 0x40000, page, 2, 0x90);
    require(mutations == 3 && machine.Mappings().size() == 3 && runtime.Snapshot().Views.size() == 3 &&
            !outerOwner.expired() && read(machine, first + 17) == sentinel,
            "Nested shutdown retired live mappings or discarded outer owner leases");
    mode = Mode::BeforeFailure;
    failure([&] { runtime.Reserve(first + 0x80000, page, 0x90, page); }, "reset guard after failure");
    require(runtime.Reserve(first + 0x80000, page, 0x90, page) == first + 0x80000,
            "Transaction guard was not reset after a callback exception");
    runtime.Shutdown();
    require(machine.Mappings().empty() && outerOwner.expired(), "Completed shutdown retained unleased runtime backing");
}
}

int main() {
    try {
        secondPageFirstAliases(); truthfulIntervalsAndFailures(); gpuTransactionLifetime(); transactionReentrancy();
        std::cout << "PASS guest memory physical aliases, query ABI, atomic failures, and GPU lease retirement\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
