// SPDX-License-Identifier: GPL-2.0-or-later
// URI contract adapted to checked guest memory from shadPS4 http.cpp,
// commit 945dbc3cc3eee80ac3e053b438502ed936fa6bb2 (2026-10-07).
#include "NetworkServices.hpp"
#include <cpu/SceHostTrampolines.hpp>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace Cpu::Platform {
namespace {
constexpr std::uint32_t invalidValue = 0x804311fe;
constexpr std::uint32_t outOfMemory = 0x80431022;
constexpr std::size_t maximumInput = 16 * 1024;
void check(Machine& m, std::uint64_t ptr, std::size_t size, Permission permission) {
    if (!ptr || size > std::numeric_limits<std::uint64_t>::max() - ptr)
        throw std::runtime_error("NetworkServices invalid guest span");
    m.CheckAccess(ptr, size, permission);
}
bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}
}
struct NetworkServices::Impl {
    using Key = std::tuple<std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::optional<SceHostTrampolines> trampolines;
    std::map<Key, std::uint64_t> gates;
    Impl(Machine& m, std::uint64_t b) : machine(m) {
        if (!b || (b & 4095) || b >= 0x7ffffffff000ULL)
            throw std::invalid_argument("NetworkServices invalid gate page");
        trampolines.emplace(m, b, SceHostTrampolines::DefaultCapacity, "NetworkServices");
    }
    ~Impl() { trampolines->Release(); }
    void escape(Machine& m) {
        const auto output = m.Get(Register::Rdi);
        const auto required = m.Get(Register::Rsi);
        const auto capacity = m.Get(Register::Rdx);
        const auto input = m.Get(Register::Rcx);
        if (!input) { m.Set(Register::Rax, invalidValue); return; }
        std::vector<std::byte> encoded;
        bool terminated = false;
        constexpr char hex[] = "0123456789ABCDEF";
        for (std::size_t index = 0; index < maximumInput; ++index) {
            if (index > std::numeric_limits<std::uint64_t>::max() - input)
                throw std::runtime_error("NetworkServices invalid guest span");
            check(m, input + index, 1, Permission::Read);
            std::array<std::byte, 1> byte{};
            m.Read(input + index, byte);
            const auto value = std::to_integer<unsigned char>(byte[0]);
            if (!value) { terminated = true; break; }
            if (unreserved(value)) encoded.push_back(byte[0]);
            else {
                encoded.push_back(std::byte{'%'});
                encoded.push_back(static_cast<std::byte>(hex[value >> 4]));
                encoded.push_back(static_cast<std::byte>(hex[value & 15]));
            }
        }
        if (!terminated)
            throw std::runtime_error("NetworkServices unsupported URI input beyond 16383 bytes");
        encoded.push_back(std::byte{0});
        const std::uint64_t needed = encoded.size();
        if (required) check(m, required, sizeof(needed), Permission::Write);
        // A size query and a too-small buffer do not access the output pointer.
        const bool fits = output && capacity >= needed;
        if (fits) check(m, output, encoded.size(), Permission::Write);
        if (required) m.Write(required, std::as_bytes(std::span(&needed, 1)));
        if (fits) m.Write(output, encoded);
        m.Set(Register::Rax, output && !fits ? outOfMemory : 0);
    }
};
NetworkServices::NetworkServices(Machine& m, std::uint64_t b) : impl(std::make_shared<Impl>(m, b)) {}
NetworkServices::~NetworkServices() = default;
std::optional<std::uint64_t> NetworkServices::Resolve(const SceImport& i, std::uint8_t type) {
    if (i.Nid != UriEscapeNid) return std::nullopt;
    if (i.LibraryName != "libSceHttp" && i.ModuleName != "libSceHttp") return std::nullopt;
    if (i.LibraryName != "libSceHttp" || i.ModuleName != "libSceHttp" ||
        i.LibraryVersion != 1 || i.ModuleMajor != 1 || i.ModuleMinor != 1)
        throw std::runtime_error("NetworkServices unsupported import scope/version");
    if (type != 2) throw std::runtime_error("NetworkServices unsupported symbol type");
    const Impl::Key key{i.LibraryId, i.ModuleId};
    if (auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto address = impl->trampolines->Add([weak = std::weak_ptr<Impl>(impl)](Machine& m) {
        const auto state = weak.lock();
        if (!state) throw std::runtime_error("NetworkServices runtime expired");
        state->escape(m);
    });
    impl->gates.emplace(key, address);
    return address;
}
}
