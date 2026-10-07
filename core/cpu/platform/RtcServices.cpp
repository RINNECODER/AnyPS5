#include "RtcServices.hpp"
#include <cpu/SceElf.hpp>
#include <bit>
#include <chrono>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace Cpu::Platform {
namespace {
constexpr std::uint64_t epoch = 62135596800000000ULL;
constexpr std::uint64_t maximum = 315537897599999999ULL;
constexpr std::int64_t minuteTicks = 60000000;
constexpr std::uint32_t pointerError = 0x80b50002u, valueError = 0x80b50003u;
constexpr std::uint32_t unsupportedError = 0x80b50005u, parseError = 0x80b50007u;
constexpr std::uint32_t yearError = 0x80b50008u, monthError = 0x80b50009u, dayError = 0x80b5000au;
constexpr std::uint32_t hourError = 0x80b5000bu, minuteError = 0x80b5000cu, secondError = 0x80b5000du;
std::uint64_t result(std::uint32_t code) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(code)));
}
std::array<std::byte, 8> encode(std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (unsigned i = 0; i < 8; ++i) bytes[i] = std::byte((value >> (i * 8)) & 255);
    return bytes;
}
bool adjust(std::uint64_t tick, int minutes, std::uint64_t& output) {
    if (tick > maximum) return false;
    const auto delta = static_cast<std::int64_t>(minutes) * minuteTicks;
    if (delta < 0 ? tick < static_cast<std::uint64_t>(-delta) : maximum - tick < static_cast<std::uint64_t>(delta)) return false;
    output = tick + static_cast<std::uint64_t>(delta);
    return true;
}
}
struct RtcServices::Impl {
    using Key = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::uint64_t base;
    std::map<Key, std::uint64_t> gates;
    Impl(Machine& guest, std::uint64_t address) : machine(guest), base(address) {
        if (!base || (base & 4095) || base > 0x7fffffffe000ULL)
            throw std::invalid_argument("RTC gate base must be a canonical aligned guest page");
        machine.Map(base, 4096, Permission::Read | Permission::Write);
        std::array<std::byte, 4096> code; code.fill(std::byte{0xcc});
        try {
            machine.Write(base, code);
            machine.Protect(base, 4096, Permission::Read | Permission::Execute);
        } catch (...) {
            machine.Unmap(base, 4096);
            throw;
        }
    }
    ~Impl() {
        // Unmap also removes the Machine host-call registrations for this owned page.
        try { machine.Unmap(base, 4096); } catch (...) { }
    }
    bool accessible(std::uint64_t address, std::size_t bytes, Permission mode) const {
        if (!address || bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
        try { machine.CheckAccess(address, bytes, mode); return true; }
        catch (const std::runtime_error&) { return false; }
    }
    std::uint32_t format(std::uint64_t destination, std::uint64_t source, std::uint64_t timezone) {
        if (!accessible(destination, 1, Permission::Write) || !accessible(source, 8, Permission::Read)) return pointerError;
        const int minutes = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(timezone));
        if (minutes < -1439 || minutes > 1439) return valueError;
        std::array<std::byte, 8> bytes{}; machine.Read(source, bytes);
        std::uint64_t tick = 0;
        for (unsigned i = 0; i < 8; ++i) tick |= std::uint64_t(std::to_integer<unsigned>(bytes[i])) << (i * 8);
        std::uint64_t local;
        if (!adjust(tick, minutes, local)) return valueError;
        using namespace std::chrono;
        const auto instant = sys_time<microseconds>{microseconds{static_cast<std::int64_t>(local) - static_cast<std::int64_t>(epoch)}};
        const auto days = floor<std::chrono::days>(instant);
        const year_month_day calendar{days};
        const hh_mm_ss<microseconds> clock{instant - days};
        std::array<char, 32> output{};
        const auto written = std::snprintf(output.data(), output.size(), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%02lld",
            int(calendar.year()), unsigned(calendar.month()), unsigned(calendar.day()),
            static_cast<long long>(clock.hours().count()), static_cast<long long>(clock.minutes().count()),
            static_cast<long long>(clock.seconds().count()), static_cast<long long>(clock.subseconds().count() / 10000));
        if (written != 22) throw std::runtime_error("RTC formatter internal range violation");
        const auto length = minutes == 0 ? 24u : 29u;
        if (minutes == 0) { output[22] = 'Z'; output[23] = 0; }
        else {
            const auto offset = minutes < 0 ? -minutes : minutes;
            std::snprintf(output.data() + 22, output.size() - 22, "%c%02d:%02d", minutes < 0 ? '-' : '+', offset / 60, offset % 60);
        }
        if (!accessible(destination, length, Permission::Write)) return pointerError;
        machine.Write(destination, std::as_bytes(std::span(output.data(), length)));
        return 0;
    }
    std::uint32_t parse(std::uint64_t destination, std::uint64_t source) {
        if (!accessible(destination, 8, Permission::Write) || !source) return pointerError;
        // Bound resource consumption explicitly; strings beyond this supported span remain unsupported.
        std::string text;
        for (unsigned i = 0; i < 128; ++i) {
            if (i > std::numeric_limits<std::uint64_t>::max() - source || !accessible(source + i, 1, Permission::Read)) return pointerError;
            std::byte byte{}; machine.Read(source + i, std::span(&byte, 1));
            if (byte == std::byte{0}) break;
            text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
            if (i == 127) return unsupportedError;
        }
        std::size_t cursor = 0;
        auto digits = [&](unsigned count, int& value) {
            value = 0;
            for (unsigned i = 0; i < count; ++i) {
                if (cursor == text.size() || text[cursor] < '0' || text[cursor] > '9') return false;
                value = value * 10 + text[cursor++] - '0';
            }
            return true;
        };
        auto separator = [&](char value) { return cursor < text.size() && text[cursor++] == value; };
        int year, month, day, hour, minute, second;
        if (!digits(4, year) || !separator('-') || !digits(2, month) || !separator('-') || !digits(2, day) ||
            cursor == text.size() || (text[cursor] != 'T' && text[cursor] != 't')) return parseError;
        ++cursor;
        if (!digits(2, hour) || !separator(':') || !digits(2, minute) || !separator(':') || !digits(2, second)) return parseError;
        int micros = 0;
        if (cursor < text.size() && text[cursor] == '.') {
            ++cursor;
            if (cursor == text.size() || text[cursor] < '0' || text[cursor] > '9') return parseError;
            int scale = 100000;
            while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
                micros += (text[cursor++] - '0') * scale;
                scale /= 10;
            }
        }
        int offset = 0;
        if (cursor < text.size() && (text[cursor] == 'Z' || text[cursor] == 'z')) ++cursor;
        else if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) {
            const bool negative = text[cursor++] == '-';
            int hours, minutes;
            if (!digits(2, hours) || !separator(':') || !digits(2, minutes) || hours > 23 || minutes > 59) return parseError;
            offset = (hours * 60 + minutes) * (negative ? -1 : 1);
            if (negative && offset == 0) return unsupportedError; // RFC3339 unknown local offset.
        } else return parseError;
        if (cursor != text.size()) return parseError;
        if (year < 1 || year > 9999) return yearError;
        if (month < 1 || month > 12) return monthError;
        using namespace std::chrono;
        const year_month_day calendar{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
        if (!calendar.ok()) return dayError;
        if (hour > 23) return hourError;
        if (minute > 59) return minuteError;
        if (second == 60) return unsupportedError; // No leap-second table is supplied.
        if (second > 59) return secondError;
        const auto seconds = duration_cast<std::chrono::seconds>(sys_days{calendar}.time_since_epoch()).count() + hour * 3600 + minute * 60 + second;
        const auto tick = static_cast<std::uint64_t>(seconds * 1000000 + micros + static_cast<std::int64_t>(epoch));
        std::uint64_t utc;
        if (!adjust(tick, -offset, utc)) return valueError;
        machine.Write(destination, encode(utc));
        return 0;
    }
    void invoke(Machine& guest, bool formatting) {
        const auto code = formatting ? format(guest.Get(Register::Rdi), guest.Get(Register::Rsi), guest.Get(Register::Rdx))
                                     : parse(guest.Get(Register::Rdi), guest.Get(Register::Rsi));
        guest.Set(Register::Rax, result(code));
    }
};
RtcServices::RtcServices(Machine& guest, std::uint64_t base) : impl(std::make_shared<Impl>(guest, base)) {}
RtcServices::~RtcServices() = default;
std::optional<std::uint64_t> RtcServices::Resolve(const SceImport& import, std::uint8_t type) {
    if (import.LibraryName != Module && import.ModuleName != Module) return std::nullopt;
    if (type != 2 || import.LibraryName != Module || import.ModuleName != Module || import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported RTC scope/type/version");
    if (import.Nid != Nids[0] && import.Nid != Nids[1]) throw std::runtime_error("Unsupported RTC NID");
    const Impl::Key key{import.Nid, import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() == 256) throw std::runtime_error("RTC gate page exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    constexpr std::array code{std::byte{0xc3}}; impl->machine.Write(gate, code);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), formatting = import.Nid == Nids[0]](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("RTC runtime expired");
        context->invoke(guest, formatting);
    });
    impl->gates.emplace(key, gate);
    return gate;
}
}
