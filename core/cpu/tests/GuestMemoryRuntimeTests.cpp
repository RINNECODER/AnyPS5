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
    failure([&] { runtime.MapDirect(whole, page, 0x400, 0x90, physical, page); }, "Unsupported guest memory protection");
    failure([&] { runtime.MapDirect(whole, page, 2, 2, physical, page); }, "Unsupported guest memory mapping flags");
    kernelError([&] { runtime.MapDirect(whole, page, 4, 0x90, physical, page); }, 22);
    kernelError([&] { runtime.MapDirect(whole, page, 2, 0x90, 7 * page, page); }, 22);
    kernelError([&] { runtime.MapDirect(whole, page, 2, 0x90, physical, page, 0x8000); }, 14);
    kernelError([&] { runtime.Protect(first - page, 2 * page, 1); }, 13);
    kernelError([&] { runtime.Unmap(first + 3 * page, 2 * page); }, 13);
    kernelError([&] { runtime.AllocateDirect(0, 8 * page, 6 * page, page, 0); }, 35);
    kernelError([&] { runtime.AllocateDirect(-1, 8 * page, page, page, 0); }, 22);
    kernelError([&] { runtime.AllocateDirect(8 * page, 8 * page, page, page, 0); }, 22);
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
    require(available.Address == 3 * page && available.Size == 5 * page, "Partial physical free did not report the largest free run");
    require(runtime.AllocateDirect(0, 8 * page, page, page, 0) == page, "Released physical gap was not reused");
    runtime.MapDirect(whole, page, 2, 0x90, page, page);
    require(read(machine, whole) == std::byte{0}, "Reallocated physical pages retained stale bytes");
    const auto views = runtime.Snapshot().Views;
    require(views.front().PhysicalId != views.back().PhysicalId, "Reused physical address retained a stale backing identity");
    const auto reserved = runtime.Reserve(first + 0x40000, 2 * page, 0x90, page);
    const auto reservation = runtime.Query(reserved);
    require(reservation.Start == reserved && reservation.End == reserved + 2 * page && reservation.Flags == 0,
            "Reserved address space was reported committed");
    require(runtime.MapDirect(reserved, page, 2, 0x90, 0, page) == reserved && runtime.Query(reserved).Flags == 0x12 &&
            runtime.Query(reserved + page).Flags == 0, "Fixed no-overwrite mapping did not split a real reservation");
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

void boundedSearchEnds() {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime runtime(machine, 4 * page);
    constexpr std::array<std::int64_t, 4> bounds{4 * page, 5 * page, 0x7fffffffff,
        std::numeric_limits<std::int64_t>::max()};
    const auto generation = runtime.Snapshot().Generation;
    for (const auto bound : bounds) {
        const auto available = runtime.AvailableDirect(0, bound, page);
        require(available.Address == 0 && available.Size == 4 * page,
                "Oversized physical search end did not bound available bytes to configured capacity");
    }
    require(runtime.Snapshot().Generation == generation, "Available search mutated the physical allocation registry");
    require(runtime.AllocateDirect(0, 0x7fffffffff, page, page, 0) == 0 &&
            runtime.AllocateDirect(0, std::numeric_limits<std::int64_t>::max(), 3 * page, page, 12) == page,
            "Sentinel search end rejected a valid allocation inside configured capacity");
    kernelError([&] { runtime.AvailableDirect(0, 0x7fffffffff, page); }, 35);
    kernelError([&] { runtime.AllocateDirect(0, 0x7fffffffff, page, page, 0); }, 35);
    kernelError([&] { runtime.AvailableDirect(4 * page, std::numeric_limits<std::int64_t>::max(), page); }, 35);
    kernelError([&] { runtime.AllocateDirect(4 * page, std::numeric_limits<std::int64_t>::max(), page, page, 0); }, 35);
    runtime.ReleaseDirect(page, 3 * page);
    const auto available = runtime.AvailableDirect(0, 0x7fffffffff, 2 * page);
    require(available.Address == 2 * page && available.Size == 2 * page,
            "Bounded sentinel search ignored alignment or reported bytes outside the physical pool");
}

void roundedProtectionLengths() {
    Cpu::Machine machine;
    machine.Map(0x1000, 4096, rx);
    struct Case { std::uint64_t Offset, Length, Begin, End; };
    constexpr std::array<Case, 4> cases{{{0, 1, 0, page}, {23, 1, 0, page},
        {page - 1, 2, 0, 2 * page}, {page + 19, page, page, 3 * page}}};
    for (const auto& value : cases) {
        Cpu::GuestMemoryRuntime runtime(machine, 4 * page);
        runtime.MapFlexible(first, 3 * page, 2, 0x90);
        runtime.Protect(first + value.Offset, value.Length, 1);
        const auto query = runtime.Query(first + value.Offset);
        require(query.Start == first + value.Begin && query.End == first + value.End && query.Protection == 1,
                "Sub-page protection did not round the complete guest-page interval");
        if (value.Begin) require(runtime.Query(first).Protection == 2, "Rounded protection changed an earlier untouched page");
        if (value.End < 3 * page) require(runtime.Query(first + value.End).Protection == 2,
                                        "Rounded protection changed a later untouched page");
        for (const auto target : {first + value.Begin, first + value.End - 1}) {
            runtime.Protect(first + value.Offset, value.Length, 1);
            std::vector<std::uint8_t> code{0x48, 0xb8}; append64(code, target);
            code.insert(code.end(), {0xc6, 0x00, 0x5a});
            machine.Write(0x1000, std::as_bytes(std::span(code)));
            machine.Set(Register::Rflags, 2);
            bool denied = false;
            try { machine.Run(0x1000, 0x100d, 32); }
            catch (const std::exception&) { denied = true; }
            require(denied && machine.Get(Register::Rip) == 0x100a && read(machine, target) == std::byte{0},
                    "Actual x86 store did not fault at a protected rounded-range endpoint");
            runtime.Protect(first + value.Offset, value.Length, 2);
            require(machine.Run(0x100a, 0x100d, 10) == Cpu::StopReason::Address && read(machine, target) == std::byte{0x5a},
                    "Actual x86 faulting store did not resume after restoring rounded-page write permission");
        }
    }
    Cpu::GuestMemoryRuntime runtime(machine, 4 * page);
    runtime.MapFlexible(first, 3 * page, 2, 0x90);
    runtime.Unmap(first + 2 * page, page);
    machine.Map(first + 2 * page, 4096, rw);
    const auto generation = runtime.Snapshot().Generation;
    kernelError([&] { runtime.Protect(first + 2 * page - 1, 2, 1); }, 13);
    kernelError([&] { runtime.Protect(first, 0, 1); }, 22);
    kernelError([&] { runtime.Protect(0, 1, 1); }, 22);
    kernelError([&] { runtime.Protect(std::numeric_limits<std::uint64_t>::max(), 2, 1); }, 22);
    kernelError([&] { runtime.Protect(std::numeric_limits<std::uint64_t>::max() - page + 2, 1, 1); }, 22);
    require(runtime.Snapshot().Generation == generation && runtime.Query(first).Protection == 2 &&
            runtime.Query(first + page).Protection == 2 && runtime.Query(first + 2 * page).Protection == 3,
            "Rejected rounded protection changed an owned or pinned mapping");
    std::vector<std::uint8_t> code{0x48, 0xb8}; append64(code, first + page + 17);
    code.insert(code.end(), {0xc6, 0x00, 0x5a}); machine.Write(0x1000, std::as_bytes(std::span(code)));
    require(machine.Run(0x1000, 0x100d, 32) == Cpu::StopReason::Address && read(machine, first + page + 17) == std::byte{0x5a},
            "Failed full-page ownership validation partially protected the preceding owned page");
}

bool accessible(Cpu::Machine& machine, std::uint64_t address, Permission permission) {
    try { machine.CheckAccess(address, 1, permission); return true; }
    catch (const std::exception&) { return false; }
}

// MEM-10: MAP_FIXED into a reservation splits it, and MAP_FIXED without NO_OVERWRITE replaces.
void fixedPlacementIntoReservations() {
    Cpu::Machine machine;
    machine.Map(first + 0x80000, 4096, rw);
    Cpu::GuestMemoryRuntime runtime(machine, 8 * page);
    const auto physical = runtime.AllocateDirect(0, 8 * page, 2 * page, page, 0);
    require(runtime.Reserve(first, 4 * page, 0x10, page) == first, "Fixed reservation was not placed");
    require(runtime.MapDirect(first + page, page, 3, 0x90, physical, page) == first + page,
            "Fixed no-overwrite direct mapping into a reservation was refused");
    auto before = runtime.Query(first), inside = runtime.Query(first + page), after = runtime.Query(first + 2 * page);
    require(before.Start == first && before.End == first + page && before.Flags == 0 &&
            inside.Start == first + page && inside.End == first + 2 * page && inside.Flags == 0x12 &&
            after.Start == first + 2 * page && after.End == first + 4 * page && after.Flags == 0,
            "Mapping into a reservation did not split it around the committed pages");
    require(runtime.MapFlexible(first + 2 * page, page, 3, 0x10) == first + 2 * page,
            "Fixed flexible mapping into a reservation was refused");
    const std::byte marker{0x6b};
    machine.Write(first + 2 * page + 9, std::span(&marker, 1));
    kernelError([&] { runtime.MapFlexible(first + 2 * page, page, 3, 0x90); }, 17);
    require(read(machine, first + 2 * page + 9) == marker, "Refused no-overwrite mapping changed the committed bytes");
    require(runtime.MapFlexible(first + 2 * page, page, 3, 0x10) == first + 2 * page &&
            read(machine, first + 2 * page + 9) == std::byte{0},
            "MAP_FIXED without NO_OVERWRITE did not replace the committed mapping with fresh memory");
    require(runtime.MapFlexible(first + page, page, 1, 0x10) == first + page && runtime.Query(first + page).Flags == 0x11 &&
            !accessible(machine, first + page, Permission::Write) && accessible(machine, first + page, Permission::Read),
            "MAP_FIXED did not replace a direct mapping with a flexible one");
    require(runtime.Reserve(first, 2 * page, 0x10, page) == first && runtime.Query(first + page).Flags == 0 &&
            !accessible(machine, first + page, Permission::Read),
            "Fixed reservation did not replace and unmap a committed mapping");
    kernelError([&] { runtime.MapFlexible(first + 0x80000, page, 3, 0x10); }, 17);
    require(runtime.Snapshot().Views.size() == 1, "Replaced mappings remained published to GPU consumers");
    runtime.Shutdown();
}

// MEM-11: releasing physical memory removes every virtual alias of it in the same commit.
void releaseUnmapsAliases() {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime runtime(machine, 8 * page);
    const auto physical = runtime.AllocateDirect(0, 8 * page, 2 * page, page, 0);
    runtime.MapDirect(first, 2 * page, 3, 0x90, physical, page);
    runtime.MapDirect(whole, page, 3, 0x90, physical + page, page);
    runtime.ReleaseDirect(physical + page, page);
    require(runtime.Query(first).End == first + page && accessible(machine, first, Permission::Write),
            "Partial release unmapped the surviving physical page");
    kernelError([&] { runtime.Query(first + page); }, 13);
    kernelError([&] { runtime.Query(whole); }, 13);
    require(!accessible(machine, first + page, Permission::Read) && !accessible(machine, whole, Permission::Read),
            "Released physical page stayed reachable through a guest alias");
    runtime.ReleaseDirect(physical, page);
    kernelError([&] { runtime.Query(first); }, 13);
    require(runtime.Snapshot().Views.empty() && runtime.AvailableDirect(0, 8 * page, page).Size == 8 * page,
            "Release with live aliases did not return the whole pool");
    // Holes are skipped by the plain release and reported by the checked one.
    const auto again = runtime.AllocateDirect(0, 8 * page, page, page, 0);
    kernelError([&] { runtime.ReleaseDirect(again, 2 * page, true); }, 2);
    runtime.ReleaseDirect(again, 2 * page);
    require(runtime.AvailableDirect(0, 8 * page, page).Size == 8 * page, "Release over a hole did not free the allocated part");
}

// MEM-14: the available-size query answers the largest aligned gap, not the first one.
void largestAvailableGap() {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime runtime(machine, 8 * page);
    runtime.AllocateDirect(0, 8 * page, 8 * page, page, 0);
    runtime.ReleaseDirect(page, page);
    runtime.ReleaseDirect(4 * page, 3 * page);
    auto largest = runtime.AvailableDirect(0, 8 * page, page);
    require(largest.Address == 4 * page && largest.Size == 3 * page, "Available size reported the first gap instead of the largest");
    largest = runtime.AvailableDirect(0, 8 * page, 4 * page);
    require(largest.Address == 4 * page && largest.Size == 3 * page, "Available size ignored the requested alignment");
    largest = runtime.AvailableDirect(0, 3 * page, page);
    require(largest.Address == page && largest.Size == page, "Available size ignored the search end");
    kernelError([&] { runtime.AvailableDirect(0, 8 * page, 8 * page); }, 35);
}

// MEM-09: every memory type is an attribute, every documented protection/mapping bit is accepted.
void memoryTypesProtectionsAndFlags() {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime runtime(machine, 8 * page);
    const auto typed = runtime.AllocateDirect(0, 8 * page, page, page, 3);
    const auto other = runtime.AllocateDirect(0, 8 * page, page, page, 10);
    kernelError([&] { runtime.AllocateDirect(0, 8 * page, page, page, -1); }, 22);
    const auto direct = runtime.QueryDirect(static_cast<std::int64_t>(typed), false);
    require(direct && direct->Start == typed && direct->End == typed + page && direct->MemoryType == 3,
            "Direct memory query lost the allocation type");
    const auto next = runtime.QueryDirect(static_cast<std::int64_t>(other + page), true);
    require(!next && !runtime.QueryDirect(static_cast<std::int64_t>(4 * page), false) &&
            runtime.QueryDirect(static_cast<std::int64_t>(typed), true)->Start == typed,
            "Direct memory query invented or skipped an allocation");
    require(runtime.MapFlexible(first, page, 0xf2, 0) == first && runtime.Query(first).Protection == 0xf2,
            "The title's 0xF2 protection request was not accepted with its raw bits");
    require(accessible(machine, first, Permission::Write), "0xF2 lost CPU read/write access");
    runtime.Protect(first, page, 0x3f1);
    require(runtime.Query(first).Protection == 0x3f1 && accessible(machine, first, Permission::Write),
            "Extended 0x100/0x200 protection bits did not grant CPU access");
    kernelError([&] { runtime.Protect(first, page, 0x400); }, 22);
    require(runtime.MapDirect(whole, page, 0x32, 0x80, typed, page) != 0, "NO_OVERWRITE without FIXED was not a plain hint");
    require(runtime.MapDirect(0, page, 0x32, 0x400000, other, page) % page == 0, "NO_COALESCE was rejected");
    const auto super = runtime.MapFlexible(0, page, 3, 1u << 24);
    require(super % 0x200000 == 0, "MAP_ALIGNED_SUPER did not align to 2 MiB");
    require(runtime.MapFlexible(0, page, 3, 17u << 24) % 0x20000 == 0, "MAP_ALIGNED(17) did not align to 128 KiB");
    kernelError([&] { runtime.MapFlexible(0, page, 3, 3u << 24); }, 22);
    runtime.Protect(whole, page, 0x32, 12);
    require(runtime.Query(whole).MemoryType == 12 && runtime.QueryDirect(static_cast<std::int64_t>(typed), false)->MemoryType == 12,
            "Type protection did not retype the mapping and its physical allocation");
    const auto retyped = runtime.MapDirect(0, page, 3, 0, other, page, 0, 5);
    require(runtime.Query(retyped).MemoryType == 5, "Typed direct mapping lost its memory type");
}

// SEC-05: flexible memory has a budget, and size queries report it.
void flexibleBudget() {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime runtime(machine, 4 * page, {}, 4 * page);
    require(runtime.FlexibleMemorySize() == 4 * page && runtime.AvailableFlexible() == 4 * page,
            "Flexible budget is not reported");
    runtime.MapFlexible(first, 3 * page, 3, 0x10);
    require(runtime.AvailableFlexible() == page, "Flexible mapping did not consume the budget");
    const auto generation = runtime.Snapshot().Generation;
    kernelError([&] { runtime.MapFlexible(whole, 2 * page, 3, 0x10); }, 12);
    require(runtime.Snapshot().Generation == generation, "Over-budget flexible mapping mutated the registry");
    runtime.Unmap(first, page);
    require(runtime.AvailableFlexible() == 2 * page, "Unmap did not return flexible budget");
    runtime.MapFlexible(whole, 2 * page, 3, 0x10);
    require(runtime.AvailableFlexible() == 0, "Flexible budget is not exact");
    runtime.MapFlexible(first + 2 * page, page, 3, 0x10);
    require(runtime.AvailableFlexible() == 0, "Fixed flexible replacement double-counted the budget");
}

// MEM-29: munmap rounds lengths and skips holes; mprotect reaches pinned Machine mappings.
void unmapRoundingAndHoles() {
    Cpu::Machine machine;
    machine.Map(first + 0x40000, page, rw);
    Cpu::GuestMemoryRuntime runtime(machine, 4 * page);
    runtime.MapFlexible(first, page, 3, 0x10);
    runtime.MapFlexible(first + 2 * page, page, 3, 0x10);
    runtime.Unmap(first, 3 * page);
    kernelError([&] { runtime.Query(first); }, 13);
    kernelError([&] { runtime.Query(first + 2 * page); }, 13);
    runtime.MapFlexible(first, 2 * page, 3, 0x10);
    runtime.Unmap(first, 1);
    require(runtime.Query(first + page).Start == first + page && !accessible(machine, first, Permission::Read),
            "Unmap did not round its length up to a whole guest page");
    runtime.Unmap(whole, page);
    kernelError([&] { runtime.Unmap(first + 0x40000, page); }, 13);
    runtime.Protect(first + 0x40000, page, 1);
    require(accessible(machine, first + 0x40000, Permission::Read) && !accessible(machine, first + 0x40000, Permission::Write),
            "Mprotect did not reach a pinned Machine mapping");
}

// Names, pools.
void namesAndPools() {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime runtime(machine, 64 * page);
    runtime.MapFlexible(first, 4 * page, 3, 0x10);
    runtime.SetName(first + page, 2 * page, "heap");
    auto named = runtime.Query(first + page);
    require(named.Start == first + page && named.End == first + 3 * page && std::string(named.Name.data()) == "heap",
            "Virtual range name was not reported with its own extent");
    require(runtime.Query(first).Name[0] == 0 && runtime.Query(first).End == first + page, "Name leaked outside its range");
    require(runtime.Query(first + page, false, false).End == first + 4 * page, "Unsplit query was clipped to a range name");
    runtime.Unmap(first, 4 * page);
    runtime.MapFlexible(first, page, 3, 0x10);
    require(runtime.Query(first).Name[0] == 0, "Unmap kept a stale range name");
    runtime.SetName(first + 1, 1, "x");
    named = runtime.Query(first + 1);
    require(named.Start == first && named.End == first + page && std::string(named.Name.data()) == "x",
            "Unaligned range name was not widened to whole pages");
    kernelError([&] { runtime.PoolCommit(whole, 4 * page, 0, 3); }, 22);
    const auto pool = runtime.Reserve(whole, 16 * page, 0x10, 4 * page, 0, true);
    require(runtime.Query(pool).Flags == 8, "Pool reservation was not reported as pooled and uncommitted");
    require(runtime.PoolStats().AvailableFlushedBlocks == 0, "Empty pool reported blocks");
    kernelError([&] { runtime.PoolCommit(pool, 4 * page, 0, 3); }, 12);
    kernelError([&] { runtime.PoolExpand(0, 64 * page, page, 0); }, 22);
    const auto expanded = runtime.PoolExpand(0, 64 * page, 8 * page, 0);
    kernelError([&] { runtime.PoolCommit(pool, page, 0, 3); }, 22);
    runtime.PoolCommit(pool, 4 * page, 0, 3);
    auto committed = runtime.Query(pool);
    require(committed.Flags == 0x18 && committed.End == pool + 4 * page && accessible(machine, pool, Permission::Write),
            "Pool commit did not back the reservation");
    const auto stats = runtime.PoolStats();
    require(stats.AllocatedFlushedBlocks == 1 && stats.AvailableFlushedBlocks == 1,
            "Pool block stats do not reflect the 64 KiB committed blocks");
    kernelError([&] { runtime.PoolCommit(pool + 4 * page, 8 * page, 0, 3); }, 12);
    runtime.PoolDecommit(pool, 4 * page);
    require(runtime.Query(pool).Flags == 8 && !accessible(machine, pool, Permission::Read) &&
            runtime.PoolStats().AllocatedFlushedBlocks == 0, "Pool decommit did not return pages to the reservation");
    kernelError([&] { runtime.PoolDecommit(first, page); }, 22);
    runtime.ReleaseDirect(static_cast<std::int64_t>(expanded), 8 * page);
    require(runtime.PoolStats().AvailableFlushedBlocks == 0, "Released pool pages still back the pool");
    kernelError([&] { runtime.PoolCommit(pool, 4 * page, 0, 3); }, 12);
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "search-bounds") boundedSearchEnds();
        else if (argc == 2 && std::string(argv[1]) == "protect-lengths") roundedProtectionLengths();
        else {
            require(argc == 1, "Unknown guest memory test group");
            secondPageFirstAliases(); truthfulIntervalsAndFailures(); gpuTransactionLifetime(); transactionReentrancy();
            boundedSearchEnds(); roundedProtectionLengths();
            fixedPlacementIntoReservations(); releaseUnmapsAliases(); largestAvailableGap();
            memoryTypesProtectionsAndFlags(); flexibleBudget(); unmapRoundingAndHoles(); namesAndPools();
        }
        std::cout << "PASS guest memory physical aliases, query ABI, atomic failures, and GPU lease retirement\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
