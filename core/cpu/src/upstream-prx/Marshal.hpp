#pragma once

// Guest-to-host marshalling for upstream core/libs/prx exports.
//
// Upstream exports assume an x86-64 host where a guest pointer is a host pointer.
// Here the guest runs under TCG on arm64, so each bridged export carries a
// hand-checked descriptor: one argument kind per parameter, checked at compile time
// against the real upstream signature (the upstream Export.cpp is compiled into the
// same translation unit as its descriptor table).
//
//   I32 / I64       integer in a SysV argument register, truncated to the parameter type
//   Opaque          a pointer the upstream body never dereferences (it only ignores or
//                   null-checks it); the guest address is passed through unchanged
//   In              const T*: T is copied from guest memory into a host temporary
//   Out             T*: copy-in, call, write back only if the host changed it
//   OutArray<N>     T*: like Out, for parameter N's count of T elements
//   Ignored         any pointer the upstream body only discards or logs: the host receives nullptr
//   InBytes<N>      const void*: N guest bytes copied in (nullptr stays nullptr)
//   OutBytes<N>     void*: copy-in, call, write back N bytes only if the host changed them
//   InString<C>     const char*: NUL-terminated guest string of fewer than C bytes, copied in
//   OutString<C>    char*: the upstream body writes a NUL-terminated string of fewer than C
//                   bytes into a host buffer; only the string and its NUL are written back
//
// Not expressible, so such exports stay hand-written: nested pointers, function
// pointers, floating point, returned pointers and stack arguments (more than six).
// Pointee types other than integers need an audited GuestLayout specialisation.

#include <cpu/Cpu.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Cpu::UpstreamPrx {

struct I32 {};
struct I64 {};
struct Opaque {};
struct In {};
struct Out {};
template <std::size_t CountArgument> struct OutArray {};
struct Ignored {};
template <std::size_t Size> struct InBytes {};
template <std::size_t Size> struct OutBytes {};
template <std::size_t Capacity> struct InString {};
template <std::size_t Capacity> struct OutString {};

// Guest (x86-64 SysV) size of a pointee the bridge copies. Integers are implicit;
// specialise for a struct only after checking it holds no pointers and has the same
// layout on both ABIs.
template <class T> struct GuestLayout {
    static_assert(std::is_integral_v<T>, "Unaudited pointee type: add a GuestLayout specialisation");
    static constexpr std::size_t Size = sizeof(T);
};

using Thunk = void (*)(Machine&, std::string_view);
struct Export { std::string_view Name; Thunk Call; };
// Name is the importing module's name. LibraryName (empty: same as Name) is the import library,
// File (empty: Name + ".prx") the module file titles list in DT_NEEDED.
struct Library {
    std::string_view Name;
    std::span<const Export> Exports;
    std::string_view LibraryName{};
    std::string_view File{};
    std::uint16_t ModuleMajor = 1;
    std::uint16_t ModuleMinor = 1;
};

namespace Detail {

template <class F> struct Signature;
template <class R, class... P> struct Signature<R (*)(P...)> { using Return = R; using Parameters = std::tuple<P...>; };
template <class R, class... P> struct Signature<R (*)(P...) noexcept> : Signature<R (*)(P...)> {};

inline constexpr std::array ArgumentRegisters{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
using Raw = std::array<std::uint64_t, ArgumentRegisters.size()>;
// Upper bound for one OutArray copy; a larger guest count is a guest bug, not a request.
inline constexpr std::size_t MaximumArrayBytes = 16u << 20;

template <class T> inline constexpr bool Flat = !std::is_pointer_v<std::remove_cv_t<T>> &&
    !std::is_function_v<T> && !std::is_floating_point_v<std::remove_cv_t<T>> && !std::is_member_pointer_v<T>;

template <class R> constexpr bool BridgedResult() {
    if constexpr (std::is_void_v<R>) return true;
    else return std::is_integral_v<R> && sizeof(R) <= 4;
}

template <class T> inline constexpr bool Copyable = Flat<T> && !std::is_void_v<T> &&
    std::is_trivially_copyable_v<T> && GuestLayout<T>::Size == sizeof(T);

[[noreturn]] inline void Fault(std::string_view function, std::size_t index, std::string_view what) {
    throw std::runtime_error(std::string(function) + ": argument " + std::to_string(index + 1) + " " + std::string(what));
}

// Copies size guest bytes at address into host memory after checking the guest may
// access them with the given permissions. Faults name the export and argument.
inline void CopyIn(Machine& guest, std::uint64_t address, void* host, std::size_t size, Permission access,
                   std::string_view function, std::size_t index) {
    if (!size) return;
    if (size - 1 > std::numeric_limits<std::uint64_t>::max() - address) Fault(function, index, "overflows the guest address space");
    try { guest.CheckAccess(address, size, access); }
    catch (const std::exception& error) {
        Fault(function, index, "is not an accessible guest buffer of " + std::to_string(size) + " bytes: " + error.what());
    }
    guest.Read(address, std::span(static_cast<std::byte*>(host), size));
}

template <class Spec, class Parameters, std::size_t Index> struct Slot;

template <class Parameters, std::size_t Index> struct Slot<I32, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_integral_v<P> && sizeof(P) <= 4, "I32 needs an integer parameter of at most 32 bits");
    P value{};
    void Load(Machine&, const Raw& raw, std::string_view) { value = static_cast<P>(static_cast<std::uint32_t>(raw[Index])); }
    P Get() const { return value; }
    void Store(Machine&) const {}
};

template <class Parameters, std::size_t Index> struct Slot<I64, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_integral_v<P> && sizeof(P) == 8, "I64 needs a 64-bit integer parameter");
    P value{};
    void Load(Machine&, const Raw& raw, std::string_view) { value = static_cast<P>(raw[Index]); }
    P Get() const { return value; }
    void Store(Machine&) const {}
};

template <class Parameters, std::size_t Index> struct Slot<Opaque, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_pointer_v<P> && Flat<std::remove_pointer_t<P>>,
                  "Opaque needs a data pointer to a non-pointer type (no callbacks or nested pointers)");
    std::uint64_t address = 0;
    void Load(Machine&, const Raw& raw, std::string_view) { address = raw[Index]; }
    P Get() const { return reinterpret_cast<P>(static_cast<std::uintptr_t>(address)); }
    void Store(Machine&) const {}
};

template <class Parameters, std::size_t Index> struct Slot<In, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    using T = std::remove_const_t<std::remove_pointer_t<P>>;
    static_assert(std::is_pointer_v<P> && std::is_const_v<std::remove_pointer_t<P>> && Copyable<T>,
                  "In needs a const pointer to an audited flat type");
    T value{};
    bool present = false;
    void Load(Machine& guest, const Raw& raw, std::string_view function) {
        if (!raw[Index]) return;
        CopyIn(guest, raw[Index], &value, sizeof(T), Permission::Read, function, Index);
        present = true;
    }
    P Get() const { return present ? &value : nullptr; }
    void Store(Machine&) const {}
};

template <class Parameters, std::size_t Index> struct Slot<Out, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    using T = std::remove_pointer_t<P>;
    static_assert(std::is_pointer_v<P> && !std::is_const_v<T> && Copyable<T>,
                  "Out needs a mutable pointer to an audited flat type");
    T value{};
    std::array<std::byte, sizeof(T)> before{};
    std::uint64_t address = 0;
    void Load(Machine& guest, const Raw& raw, std::string_view function) {
        if (!raw[Index]) return;
        CopyIn(guest, raw[Index], &value, sizeof(T), Permission::Read | Permission::Write, function, Index);
        std::memcpy(before.data(), &value, sizeof(T));
        address = raw[Index];
    }
    P Get() { return address ? &value : nullptr; }
    void Store(Machine& guest) const {
        // Untouched outputs stay untouched: no write, so no translation-cache flush.
        if (address && std::memcmp(before.data(), &value, sizeof(T)))
            guest.Write(address, std::as_bytes(std::span(&value, 1)));
    }
};

template <std::size_t CountArgument, class Parameters, std::size_t Index> struct Slot<OutArray<CountArgument>, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    using T = std::remove_pointer_t<P>;
    using Count = std::tuple_element_t<CountArgument, Parameters>;
    static_assert(std::is_pointer_v<P> && !std::is_const_v<T> && Copyable<T>,
                  "OutArray needs a mutable pointer to an audited flat type");
    static_assert(CountArgument != Index && std::is_integral_v<Count>, "OutArray count must be another integer parameter");
    std::vector<T> values;
    std::vector<T> before;
    std::uint64_t address = 0;
    std::size_t count = 0;
    void Load(Machine& guest, const Raw& raw, std::string_view function) {
        if (!raw[Index]) return;
        // Same truncation as the count parameter's own I32/I64 slot, so host and copy agree.
        const auto typed = static_cast<Count>(raw[CountArgument]);
        // A negative signed count copies nothing; the upstream body sees the same negative value.
        std::uint64_t requested = static_cast<std::uint64_t>(typed);
        if constexpr (std::is_signed_v<Count>) if (typed < 0) requested = 0;
        if (requested > MaximumArrayBytes / sizeof(T)) Fault(function, Index, "array count exceeds the bridge copy limit");
        count = static_cast<std::size_t>(requested);
        // A non-null guest pointer stays non-null for the host even when the count is zero.
        values.resize(count ? count : 1);
        CopyIn(guest, raw[Index], values.data(), count * sizeof(T), Permission::Read | Permission::Write, function, Index);
        before.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
        address = raw[Index];
    }
    P Get() { return address ? values.data() : nullptr; }
    void Store(Machine& guest) const {
        if (address && count && std::memcmp(before.data(), values.data(), count * sizeof(T)))
            guest.Write(address, std::as_bytes(std::span(values.data(), count)));
    }
};

template <class Parameters, std::size_t Index> struct Slot<Ignored, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_pointer_v<P>, "Ignored needs a pointer parameter");
    void Load(Machine&, const Raw&, std::string_view) {}
    P Get() const { return nullptr; }
    void Store(Machine&) const {}
};
template <std::size_t Size, class Parameters, std::size_t Index> struct Slot<InBytes<Size>, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_same_v<P, const void*>, "InBytes needs a const void* parameter");
    std::array<std::byte, Size> value{};
    bool present = false;
    void Load(Machine& guest, const Raw& raw, std::string_view function) {
        if (!raw[Index]) return;
        CopyIn(guest, raw[Index], value.data(), Size, Permission::Read, function, Index);
        present = true;
    }
    P Get() const { return present ? value.data() : nullptr; }
    void Store(Machine&) const {}
};
template <std::size_t Size, class Parameters, std::size_t Index> struct Slot<OutBytes<Size>, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_same_v<P, void*>, "OutBytes needs a void* parameter");
    std::array<std::byte, Size> value{};
    std::array<std::byte, Size> before{};
    std::uint64_t address = 0;
    void Load(Machine& guest, const Raw& raw, std::string_view function) {
        if (!raw[Index]) return;
        CopyIn(guest, raw[Index], value.data(), Size, Permission::Read | Permission::Write, function, Index);
        before = value;
        address = raw[Index];
    }
    P Get() { return address ? value.data() : nullptr; }
    void Store(Machine& guest) const {
        if (address && value != before) guest.Write(address, std::span<const std::byte>(value));
    }
};
template <std::size_t Capacity, class Parameters, std::size_t Index> struct Slot<InString<Capacity>, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_same_v<P, const char*>, "InString needs a const char* parameter");
    std::array<char, Capacity> value{};
    bool present = false;
    void Load(Machine& guest, const Raw& raw, std::string_view function) {
        if (!raw[Index]) return;
        for (std::size_t offset = 0; offset < Capacity; ++offset) {
            CopyIn(guest, raw[Index] + offset, &value[offset], 1, Permission::Read, function, Index);
            if (!value[offset]) { present = true; return; }
        }
        Fault(function, Index, "is not a NUL-terminated string shorter than " + std::to_string(Capacity) + " bytes");
    }
    P Get() const { return present ? value.data() : nullptr; }
    void Store(Machine&) const {}
};
template <std::size_t Capacity, class Parameters, std::size_t Index> struct Slot<OutString<Capacity>, Parameters, Index> {
    using P = std::tuple_element_t<Index, Parameters>;
    static_assert(std::is_same_v<P, char*>, "OutString needs a char* parameter");
    std::array<char, Capacity> value{};
    std::uint64_t address = 0;
    std::string_view function;
    void Load(Machine&, const Raw& raw, std::string_view name) { address = raw[Index]; function = name; }
    P Get() { return address ? value.data() : nullptr; }
    void Store(Machine& guest) const {
        if (!address) return;
        const auto length = std::strlen(value.data()) + 1;
        if (length >= Capacity) Fault(function, Index, "string exceeds the audited capacity");
        if (length == 1) return;
        try { guest.CheckAccess(address, length, Permission::Write); }
        catch (const std::exception& error) { Fault(function, Index, std::string("is not a writable guest buffer: ") + error.what()); }
        guest.Write(address, std::as_bytes(std::span(value.data(), length)));
    }
};
template <auto Function, class R, class Parameters, class... Specs, std::size_t... Index>
void Invoke(Machine& guest, std::string_view name, std::index_sequence<Index...>) {
    Raw raw{};
    for (std::size_t index = 0; index < sizeof...(Specs); ++index) raw[index] = guest.Get(ArgumentRegisters[index]);
    std::tuple<Slot<Specs, Parameters, Index>...> slots;
    (std::get<Index>(slots).Load(guest, raw, name), ...);
    std::uint64_t result = 0;
    try {
        if constexpr (std::is_void_v<R>) Function(std::get<Index>(slots).Get()...);
        // SCE status codes are signed 32-bit values, sign-extended into RAX.
        else result = static_cast<std::uint64_t>(static_cast<std::int64_t>(Function(std::get<Index>(slots).Get()...)));
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(name) + ": " + error.what());
    }
    (std::get<Index>(slots).Store(guest), ...);
    guest.Set(Register::Rax, result);
}

}

template <auto Function, class... Specs>
void Call(Machine& guest, std::string_view name) {
    using Signature = Detail::Signature<decltype(Function)>;
    using R = typename Signature::Return;
    using Parameters = typename Signature::Parameters;
    static_assert(std::tuple_size_v<Parameters> == sizeof...(Specs), "Descriptor needs exactly one argument kind per parameter");
    static_assert(sizeof...(Specs) <= Detail::ArgumentRegisters.size(), "Stack arguments are not marshalled");
    static_assert(Detail::BridgedResult<R>(),
                  "Only void and 32-bit integer results are bridged (no pointers, floats or 64-bit results)");
    Detail::Invoke<Function, R, Parameters, Specs...>(guest, name, std::index_sequence_for<Specs...>{});
}

}

// One descriptor row: the export's own name (for its NID) and its argument kinds.
#define ANYPS5_UPSTREAM_EXPORT(function, ...) \
    ::Cpu::UpstreamPrx::Export{#function, &::Cpu::UpstreamPrx::Call<&::function __VA_OPT__(,) __VA_ARGS__>}
