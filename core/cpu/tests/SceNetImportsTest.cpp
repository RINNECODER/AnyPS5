#include <cpu/SceNetImports.hpp>
#include <cpu/SceElf.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Synthetic guest fixtures only: loopback sockets, no game files. Every call goes through a real
// x86 CALL into the gate, so the guest ABI (registers, stack, callee-saved state) is exercised.
namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t data = 0x10000;       // 64 KiB of scratch guest memory
constexpr std::uint32_t netBase = 0x80410100u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

enum class Lib { Net, Ssl, Http2, Posix };

Cpu::SceImport qualified(const std::string& nid, Lib lib) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    switch (lib) {
        case Lib::Net: value.LibraryName = value.ModuleName = "libSceNet"; break;
        case Lib::Ssl: value.LibraryName = value.ModuleName = "libSceSsl"; value.ModuleMajor = 2; break;
        case Lib::Http2: value.LibraryName = value.ModuleName = "libSceHttp2"; break;
        case Lib::Posix: value.LibraryName = "libScePosix"; value.ModuleName = "libkernel"; break;
    }
    return value;
}

struct Entry { const char* nid; Lib lib; };
// Every NID the ported lists name: the libSceNet, libSceSsl and libSceHttp2 rows of the import list
// and the socket rows of the libkernel list.
constexpr Entry required[] = {
    {"dgJBaeJnGpo", Lib::Net}, {"C4UgDHHPvdw", Lib::Net}, {"kJlYH5uMAWI", Lib::Net}, {"K7RlrTkI-mw", Lib::Net},
    {"Nd91WaWmG2w", Lib::Net}, {"RCCY01Xd+58", Lib::Net}, {"TSM6whtekok", Lib::Net}, {"45ggEzakPJQ", Lib::Net},
    {"Q4qBuN-c0ZM", Lib::Net}, {"2mKX2Spso7I", Lib::Net}, {"OXXX4mUk3uk", Lib::Net}, {"HQOwnfMGipQ", Lib::Net},
    {"beRjXBn-z+o", Lib::Net}, {"9wO9XrMsNhc", Lib::Net}, {"zJGf8xjFnQE", Lib::Net}, {"bErx49PgxyY", Lib::Net},
    {"kOj1HiAGE54", Lib::Net}, {"PIWqhn9oSxc", Lib::Net},
    {"hdpVEUDFW3s", Lib::Ssl}, {"0K1yQ6Lv-Yc", Lib::Ssl},
    {"3JCe3lCbQ8A", Lib::Http2}, {"+wCt7fCijgk", Lib::Http2}, {"A+NVAFu4eCg", Lib::Http2}, {"IZ-qjhRqvjk", Lib::Http2},
    {"MOp-AUhdfi8", Lib::Http2}, {"mmyOCxQMVYQ", Lib::Http2}, {"nrPfOE8TQu0", Lib::Http2}, {"izvHhqgDt44", Lib::Http2},
    {"-rdXUi2XW90", Lib::Http2}, {"pDom5-078DA", Lib::Http2}, {"o0DBQpFE13o", Lib::Http2}, {"9XYJwCf3lEA", Lib::Http2},
    {"bGN-6zbo7ms", Lib::Http2}, {"c8D9qIjo8EY", Lib::Http2}, {"YiBUtz-pGkc", Lib::Http2}, {"-0wUiGX74GQ", Lib::Http2},
    {"FSAFOzi0FpM", Lib::Http2}, {"YfaXPnc8PJE", Lib::Http2}, {"-MU9L6S3Fj4", Lib::Http2}, {"VYMxTcBqSE0", Lib::Http2},
    {"rbqZig38AT8", Lib::Http2}, {"D1IAo-CNswA", Lib::Http2},
    {"T8fER+tIGgk", Lib::Posix}, {"lUk6wrGXyMw", Lib::Posix}, {"oBr313PppNE", Lib::Posix}, {"TU-d9PfIHPM", Lib::Posix},
    {"KuOmgKoqCdY", Lib::Posix}, {"fFxGkxF2bVo", Lib::Posix}, {"6O8EwYOgH9Y", Lib::Posix}};

// Exports checked by name through their computed NIDs.
const char* const netNames[] = {"sceNetPoolCreate", "sceNetPoolDestroy", "sceNetResolverCreate", "sceNetResolverDestroy",
    "sceNetResolverStartNtoa", "sceNetSocket", "sceNetSocketClose", "sceNetErrnoLoc", "sceNetBind", "sceNetListen",
    "sceNetAccept", "sceNetConnect", "sceNetSend", "sceNetRecv", "sceNetSendto", "sceNetRecvfrom", "sceNetShutdown",
    "sceNetSetsockopt", "sceNetGetsockopt", "sceNetGetsockname", "sceNetGetpeername", "sceNetEpollCreate",
    "sceNetEpollControl", "sceNetEpollWait", "sceNetEpollDestroy", "sceNetEpollAbort", "sceNetGetMemoryPoolStats"};

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceNetImports> imports = std::make_unique<Cpu::SceNetImports>(machine);
    std::vector<std::array<std::uint64_t, 3>> triggered;

    Session() {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string_view::npos,
                "Network fixture requires the native modern TCG backend");
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(data, 65536, rw);
        constexpr std::array<std::uint8_t, 17> caller{0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x48, 0x89, 0x05, 0x03, 0x10, 0, 0,
                                                      0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
        imports->SetUserEventTrigger([this](std::uint64_t queue, std::int32_t id, std::uint64_t value) {
            if (queue == 0xdead) return 9;
            triggered.push_back({queue, static_cast<std::uint64_t>(id), value});
            return 0;
        });
    }

    std::uint64_t call(const std::string& nid, Lib lib, std::array<std::uint64_t, 6> args = {}) {
        const auto gate = imports->Resolve(qualified(nid, lib));
        require(gate.has_value(), "Gate did not resolve " + nid);
        machine.Write(0x2000, std::as_bytes(std::span(&*gate, 1)));
        const std::array regs{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        for (std::size_t index = 0; index < 6; ++index) machine.Set(regs[index], args[index]);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        constexpr std::array saved{Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (auto reg : saved) machine.Set(reg, 0x9876543210123456);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "Gate did not return through the x86 caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00, "Gate corrupted the stack");
        for (auto reg : saved) require(machine.Get(reg) == 0x9876543210123456, "Gate corrupted a callee-saved register");
        return machine.Get(Register::Rax);
    }
    std::int32_t call32(const std::string& nid, Lib lib, std::array<std::uint64_t, 6> args = {}) {
        return static_cast<std::int32_t>(call(nid, lib, args));
    }
    std::int32_t net(const char* name, std::array<std::uint64_t, 6> args = {}) {
        return call32(Nid(name), Lib::Net, args);
    }
    std::int64_t net64(const char* name, std::array<std::uint64_t, 6> args = {}) {
        return static_cast<std::int64_t>(call(Nid(name), Lib::Net, args));
    }
    static std::string Nid(const char* name) { return ::Nid::ComputeNid(name, ""); }

    std::int32_t errnoValue() {
        const auto cell = call(Nid("sceNetErrnoLoc"), Lib::Net);
        std::int32_t value = -1;
        machine.Read(cell, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    void put(std::uint64_t address, const void* bytes, std::size_t size) {
        machine.Write(address, std::as_bytes(std::span(static_cast<const std::uint8_t*>(bytes), size)));
    }
    void text(std::uint64_t address, const char* value) { put(address, value, std::strlen(value) + 1); }
    template<class T> T get(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    void loopback(std::uint64_t address, std::uint16_t hostPort) {
        const std::uint8_t bytes[16] = {16, 2, static_cast<std::uint8_t>(hostPort >> 8), static_cast<std::uint8_t>(hostPort), 127, 0, 0, 1};
        put(address, bytes, sizeof(bytes));
    }
};
}

namespace {

void gatesResolve(Session& s) {
    for (const auto& entry : required) require(s.imports->Resolve(qualified(entry.nid, entry.lib)).has_value(), std::string("missing ") + entry.nid);
    for (const char* name : netNames) require(s.imports->Resolve(qualified(Session::Nid(name), Lib::Net)).has_value(), std::string("missing ") + name);
    // Scope and version mismatches are not claimed.
    auto wrong = qualified("Q4qBuN-c0ZM", Lib::Net);
    wrong.ModuleMinor = 2;
    require(!s.imports->Resolve(wrong).has_value(), "wrong module version resolved");
    require(!s.imports->Resolve(qualified("Q4qBuN-c0ZM", Lib::Posix)).has_value(), "net NID resolved in the kernel scope");
    // The address-conversion exports belong to SceNetAddressImports.
    require(!s.imports->Resolve(qualified("9T2pDF2Ryqg", Lib::Net)).has_value(), "htonl claimed twice");
}

void pools(Session& s) {
    const auto pool = s.net("sceNetPoolCreate", {0, 4096, 0});
    require(pool > 0, "pool create");
    s.machine.Set(Register::Rax, 0);
    require(s.net("sceNetGetMemoryPoolStats", {static_cast<std::uint64_t>(pool), data}) == 0, "pool stats");
    require(s.get<std::uint64_t>(data) == 4096, "pool size");
    require(s.net("sceNetGetMemoryPoolStats", {static_cast<std::uint64_t>(pool), 0}) == static_cast<std::int32_t>(netBase | 22), "null stats");
    require(s.net("sceNetGetMemoryPoolStats", {999, data}) == static_cast<std::int32_t>(netBase | 9), "unknown pool stats");
    require(s.errnoValue() == 9, "pool errno");
    require(s.net("sceNetPoolCreate", {0, 0, 0}) == static_cast<std::int32_t>(netBase | 22), "empty pool");
    require(s.net("sceNetPoolDestroy", {static_cast<std::uint64_t>(pool)}) == 0, "pool destroy");
    require(s.net("sceNetPoolDestroy", {static_cast<std::uint64_t>(pool)}) == static_cast<std::int32_t>(netBase | 9), "double destroy");
}

void udpAndTcp(Session& s) {
    // UDP over loopback with PS5 sockaddr layouts on both ends.
    const auto a = s.net("sceNetSocket", {0, 2, 2, 0});
    const auto b = s.net("sceNetSocket", {0, 2, 2, 0});
    require(a >= 400 && b >= 400 && a != b, "net socket ids");
    s.loopback(data, 0);
    require(s.net("sceNetBind", {static_cast<std::uint64_t>(a), data, 16}) == 0, "bind a");
    require(s.net("sceNetBind", {static_cast<std::uint64_t>(a), data, 16}) == static_cast<std::int32_t>(netBase | 22), "rebinding a bound socket is EINVAL");
    const std::uint32_t capacity = 16;
    s.put(data + 64, &capacity, 4);
    require(s.net("sceNetGetsockname", {static_cast<std::uint64_t>(a), data + 32, data + 64}) == 0, "getsockname");
    const auto name = s.get<std::array<std::uint8_t, 16>>(data + 32);
    require(name[0] == 16 && name[1] == 2 && name[4] == 127 && name[7] == 1 && (name[2] | name[3]) != 0, "guest sockaddr layout");
    require(s.get<std::uint32_t>(data + 64) == 16, "sockaddr length");
    s.text(data + 128, "ping");
    require(s.net64("sceNetSendto", {static_cast<std::uint64_t>(b), data + 128, 5, 0, data + 32, 16}) == 5, "sendto");
    s.put(data + 64, &capacity, 4);
    require(s.net64("sceNetRecvfrom", {static_cast<std::uint64_t>(a), data + 256, 16, 0, data + 96, data + 64}) == 5, "recvfrom");
    char got[8]{};
    s.machine.Read(data + 256, std::as_writable_bytes(std::span(got, 5)));
    require(std::string(got) == "ping", "udp payload");
    require(s.get<std::uint8_t>(data + 96) == 16 && s.get<std::uint8_t>(data + 97) == 2, "from layout");
    // Empty non-blocking socket reports EAGAIN as the console does.
    const std::int32_t on = 1;
    s.put(data + 512, &on, 4);
    require(s.net("sceNetSetsockopt", {static_cast<std::uint64_t>(a), 0xFFFF, 0x1200, data + 512, 4}) == 0, "NBIO");
    require(s.net64("sceNetRecv", {static_cast<std::uint64_t>(a), data + 256, 16, 0}) == static_cast<std::int32_t>(netBase | 35), "EAGAIN");
    require(s.errnoValue() == 35, "EAGAIN errno");
    require(s.net("sceNetSetsockopt", {static_cast<std::uint64_t>(a), 0xFFFF, 0x1, data + 512, 4}) == static_cast<std::int32_t>(netBase | 45), "unsupported option");
    // A guest fault is reported, never dereferenced on the host.
    require(s.net64("sceNetRecv", {static_cast<std::uint64_t>(a), 0xdead0000, 16, 0}) < 0, "bad buffer");
    require(s.net("sceNetSocketClose", {static_cast<std::uint64_t>(a)}) == 0 && s.net("sceNetSocketClose", {static_cast<std::uint64_t>(b)}) == 0, "close");
    require(s.net("sceNetSocketClose", {static_cast<std::uint64_t>(a)}) == static_cast<std::int32_t>(netBase | 9), "double close");

    // TCP: listen, connect, accept, stream both ways, shutdown.
    const auto server = s.net("sceNetSocket", {0, 2, 1, 0});
    s.loopback(data, 0);
    require(s.net("sceNetBind", {static_cast<std::uint64_t>(server), data, 16}) == 0, "tcp bind");
    require(s.net("sceNetListen", {static_cast<std::uint64_t>(server), 4}) == 0, "listen");
    s.put(data + 64, &capacity, 4);
    require(s.net("sceNetGetsockname", {static_cast<std::uint64_t>(server), data + 32, data + 64}) == 0, "tcp name");
    const auto client = s.net("sceNetSocket", {0, 2, 1, 0});
    require(s.net("sceNetConnect", {static_cast<std::uint64_t>(client), data + 32, 16}) == 0, "connect");
    s.put(data + 64, &capacity, 4);
    const auto accepted = s.net("sceNetAccept", {static_cast<std::uint64_t>(server), data + 96, data + 64});
    require(accepted >= 400 && accepted != server && accepted != client, "accept");
    s.text(data + 128, "hello");
    require(s.net64("sceNetSend", {static_cast<std::uint64_t>(client), data + 128, 6, 0}) == 6, "tcp send");
    require(s.net64("sceNetRecv", {static_cast<std::uint64_t>(accepted), data + 256, 16, 0}) == 6, "tcp recv");
    require(s.get<std::uint8_t>(data + 256) == 'h', "tcp payload");
    require(s.net("sceNetShutdown", {static_cast<std::uint64_t>(client), 1}) == 0, "shutdown");
    require(s.net64("sceNetRecv", {static_cast<std::uint64_t>(accepted), data + 256, 16, 0}) == 0, "tcp eof");
    for (auto id : {server, client, accepted}) require(s.net("sceNetSocketClose", {static_cast<std::uint64_t>(id)}) == 0, "tcp close");
    // A blocking read with a timeout and an aborted blocking read both return promptly.
    const auto idle = s.net("sceNetSocket", {0, 2, 2, 0});
    const std::int32_t micros = 20000;
    s.put(data + 512, &micros, 4);
    require(s.net("sceNetSetsockopt", {static_cast<std::uint64_t>(idle), 0xFFFF, 0x1006, data + 512, 4}) == 0, "RCVTIMEO");
    require(s.net64("sceNetRecv", {static_cast<std::uint64_t>(idle), data + 256, 16, 0}) == static_cast<std::int32_t>(netBase | 35), "recv timeout");
    const std::uint32_t four = 4;
    s.put(data + 528, &four, 4);
    require(s.net("sceNetGetsockopt", {static_cast<std::uint64_t>(idle), 0xFFFF, 0x1006, data + 520, data + 528}) == 0 &&
            s.get<std::int32_t>(data + 520) == 20000, "RCVTIMEO reads back in microseconds");
    require(s.net("sceNetSocketAbort", {static_cast<std::uint64_t>(idle), 0}) == 0, "abort");
    require(s.net("sceNetSocketClose", {static_cast<std::uint64_t>(idle)}) == 0, "idle close");
}

void epoll(Session& s) {
    const auto sock = s.net("sceNetSocket", {0, 2, 2, 0});
    s.loopback(data, 0);
    require(s.net("sceNetBind", {static_cast<std::uint64_t>(sock), data, 16}) == 0, "epoll bind");
    const std::uint32_t capacity = 16;
    s.put(data + 64, &capacity, 4);
    require(s.net("sceNetGetsockname", {static_cast<std::uint64_t>(sock), data + 32, data + 64}) == 0, "epoll name");
    const auto eid = s.net("sceNetEpollCreate", {0, 0});
    require(eid >= 0x4000, "epoll create");
    std::uint8_t raw[24]{};
    const std::uint32_t in = 1;
    std::memcpy(raw, &in, 4);
    const std::uint64_t ident = 0x1234, user = 0xabcdef;
    std::memcpy(raw + 8, &ident, 8);
    std::memcpy(raw + 16, &user, 8);
    s.put(data + 1024, raw, sizeof(raw));
    require(s.net("sceNetEpollControl", {static_cast<std::uint64_t>(eid), 1, static_cast<std::uint64_t>(sock), data + 1024}) == 0, "epoll add");
    require(s.net("sceNetEpollControl", {static_cast<std::uint64_t>(eid), 1, static_cast<std::uint64_t>(sock), data + 1024}) == static_cast<std::int32_t>(netBase | 48), "epoll duplicate");
    require(s.net("sceNetEpollWait", {static_cast<std::uint64_t>(eid), data + 2048, 4, 0}) == 0, "epoll empty");
    const auto sender = s.net("sceNetSocket", {0, 2, 2, 0});
    s.text(data + 128, "x");
    require(s.net64("sceNetSendto", {static_cast<std::uint64_t>(sender), data + 128, 2, 0, data + 32, 16}) == 2, "epoll send");
    require(s.net("sceNetEpollWait", {static_cast<std::uint64_t>(eid), data + 2048, 4, 200000}) == 1, "epoll ready");
    require(s.get<std::uint32_t>(data + 2048) == 1 && s.get<std::uint64_t>(data + 2056) == 0x1234 && s.get<std::uint64_t>(data + 2064) == 0xabcdef, "epoll event");
    require(s.net("sceNetEpollAbort", {static_cast<std::uint64_t>(eid), 0}) == 0, "epoll abort");
    require(s.net("sceNetEpollWait", {static_cast<std::uint64_t>(eid), data + 2048, 4, 200000}) == static_cast<std::int32_t>(netBase | 53), "aborted wait");
    require(s.net("sceNetEpollControl", {static_cast<std::uint64_t>(eid), 2, static_cast<std::uint64_t>(sock), 0}) == 0, "epoll del");
    require(s.net("sceNetEpollDestroy", {static_cast<std::uint64_t>(eid)}) == 0, "epoll destroy");
    require(s.net("sceNetEpollDestroy", {static_cast<std::uint64_t>(eid)}) == static_cast<std::int32_t>(netBase | 9), "epoll double destroy");
    s.net("sceNetSocketClose", {static_cast<std::uint64_t>(sock)});
    s.net("sceNetSocketClose", {static_cast<std::uint64_t>(sender)});
}

void resolver(Session& s) {
    const auto rid = s.net("sceNetResolverCreate", {0, 0, 0});
    require(rid >= 0x100, "resolver create");
    s.text(data, "127.0.0.1");
    require(s.net("sceNetResolverStartNtoa", {static_cast<std::uint64_t>(rid), data, data + 64, 0, 0, 0}) == 0, "ntoa");
    require(s.get<std::uint32_t>(data + 64) == 0x0100007f, "ntoa address (network order)");
    require(s.net("sceNetResolverStartNtoa", {static_cast<std::uint64_t>(rid), 0, data + 64, 0, 0, 0}) == static_cast<std::int32_t>(netBase | 22), "null host");
    require(s.net("sceNetResolverStartNtoa", {999, data, data + 64, 0, 0, 0}) == static_cast<std::int32_t>(netBase | 9), "unknown resolver");
    require(s.net("sceNetResolverStartNtoaMultipleRecordsEx", {static_cast<std::uint64_t>(rid), data, data + 1024, 0, 0, 0}) == 0, "multiple records");
    require(s.get<std::int32_t>(data + 1024 + 320) == 1 && s.get<std::uint32_t>(data + 1024) == 0x0100007f &&
            s.get<std::int32_t>(data + 1024 + 16) == 2, "record layout");
    s.text(data, "name.invalid");
    const auto failure = s.net("sceNetResolverStartNtoa", {static_cast<std::uint64_t>(rid), data, data + 64, 0, 0, 0});
    require(static_cast<std::uint32_t>(failure) == 0x804101E1u, "unresolvable name reports ENODNS");
    require(s.net("sceNetResolverGetError", {static_cast<std::uint64_t>(rid), data + 80}) == 0 &&
            static_cast<std::uint32_t>(s.get<std::int32_t>(data + 80)) == 0x804101E1u, "resolver error status");
    require(s.net("sceNetResolverDestroy", {static_cast<std::uint64_t>(rid)}) == 0, "resolver destroy");
    require(s.net("sceNetResolverDestroy", {static_cast<std::uint64_t>(rid)}) == static_cast<std::int32_t>(netBase | 9), "resolver double destroy");
}

void posixSockets(Session& s) {
    const auto posix = [&](const char* nid, std::array<std::uint64_t, 6> args) {
        return static_cast<std::int64_t>(s.call(nid, Lib::Posix, args));
    };
    const auto socketFd = posix("TU-d9PfIHPM", {2, 2, 0});
    require(socketFd >= 0x10000000, "posix socket descriptor range");
    require(posix("TU-d9PfIHPM", {99, 2, 0}) == -1, "bad family");
    require(s.errnoValue() == 47, "EAFNOSUPPORT");
    s.loopback(data, 0);
    require(posix("KuOmgKoqCdY", {static_cast<std::uint64_t>(socketFd), data, 16}) == 0, "posix bind");
    // SO_RCVTIMEO takes the FreeBSD timeval (two int64) and reads back identically.
    const std::int64_t timeval[2] = {0, 30000};
    s.put(data + 512, timeval, sizeof(timeval));
    require(posix("fFxGkxF2bVo", {static_cast<std::uint64_t>(socketFd), 0xFFFF, 0x1006, data + 512, 16}) == 0, "posix setsockopt");
    const std::uint32_t length = 16;
    s.put(data + 640, &length, 4);
    require(posix("6O8EwYOgH9Y", {static_cast<std::uint64_t>(socketFd), 0xFFFF, 0x1006, data + 576, data + 640}) == 0, "posix getsockopt");
    require(s.get<std::int64_t>(data + 584) == 30000 && s.get<std::uint32_t>(data + 640) == 16, "timeval round trip");
    require(posix("fFxGkxF2bVo", {static_cast<std::uint64_t>(socketFd), 0xFFFF, 0x9999, data + 512, 16}) == -1 && s.errnoValue() == 42, "ENOPROTOOPT");
    require(posix("lUk6wrGXyMw", {static_cast<std::uint64_t>(socketFd), data + 256, 8, 0, 0, 0}) == -1 && s.errnoValue() == 35, "posix recv timeout is EAGAIN");
    // sendto/recvfrom between a posix socket and a net socket sharing one table.
    const auto peer = s.net("sceNetSocket", {0, 2, 2, 0});
    s.loopback(data + 1024, 0);
    require(s.net("sceNetBind", {static_cast<std::uint64_t>(peer), data + 1024, 16}) == 0, "peer bind");
    const std::uint32_t capacity = 16;
    s.put(data + 640, &capacity, 4);
    require(s.net("sceNetGetsockname", {static_cast<std::uint64_t>(peer), data + 1056, data + 640}) == 0, "peer name");
    s.text(data + 128, "abc");
    require(posix("oBr313PppNE", {static_cast<std::uint64_t>(socketFd), data + 128, 4, 0, data + 1056, 16}) == 4, "posix sendto");
    require(s.net64("sceNetRecv", {static_cast<std::uint64_t>(peer), data + 256, 16, 0}) == 4, "net recv of posix send");
    // select over net sockets: readable after a send, timeout otherwise.
    std::uint64_t set[16]{};
    set[peer / 64] |= std::uint64_t{1} << (peer % 64);
    s.put(data + 2048, set, sizeof(set));
    const std::int64_t zero[2] = {0, 20000};
    s.put(data + 3072, zero, sizeof(zero));
    require(posix("T8fER+tIGgk", {static_cast<std::uint64_t>(peer + 1), data + 2048, 0, 0, data + 3072}) == 0, "select timeout");
    require(s.get<std::uint64_t>(data + 2048 + (peer / 64) * 8) == 0, "select clears the set on timeout");
    s.put(data + 2048, set, sizeof(set));
    require(posix("oBr313PppNE", {static_cast<std::uint64_t>(socketFd), data + 128, 4, 0, data + 1056, 16}) == 4, "send for select");
    require(posix("T8fER+tIGgk", {static_cast<std::uint64_t>(peer + 1), data + 2048, 0, 0, data + 3072}) == 1, "select ready");
    require(((s.get<std::uint64_t>(data + 2048 + (peer / 64) * 8) >> (peer % 64)) & 1) == 1, "select keeps the ready bit");
    require(posix("T8fER+tIGgk", {5000, 0, 0, 0, 0}) == -1 && s.errnoValue() == 22, "select nfds bound");
    s.net("sceNetSocketClose", {static_cast<std::uint64_t>(peer)});
    // POSIX TCP: listen, connect and accept on the same table.
    const auto connectNid = ::Nid::ComputeNid("connect", "libkernel"), acceptNid = ::Nid::ComputeNid("accept", "libkernel");
    require(connectNid == "XVL8So3QJUk" && acceptNid == "3e+4Iv7IJ8U", "posix connect/accept NIDs");
    const auto listenNid = ::Nid::ComputeNid("listen", "libkernel");
    const auto call = [&](const std::string& nid, std::array<std::uint64_t, 6> args) { return static_cast<std::int64_t>(s.call(nid.c_str(), Lib::Posix, args)); };
    const auto server = call("TU-d9PfIHPM", {2, 1, 0});
    s.loopback(data + 1024, 0);
    require(call(::Nid::ComputeNid("bind", "libkernel"), {static_cast<std::uint64_t>(server), data + 1024, 16}) == 0, "posix tcp bind");
    require(call(listenNid, {static_cast<std::uint64_t>(server), 2}) == 0, "posix listen");
    const std::uint32_t size = 16;
    s.put(data + 640, &size, 4);
    require(s.net("sceNetGetsockname", {static_cast<std::uint64_t>(server), data + 1056, data + 640}) == 0, "posix tcp name");
    const auto client = call("TU-d9PfIHPM", {2, 1, 0});
    require(call(connectNid, {static_cast<std::uint64_t>(client), data + 1056, 16}) == 0, "posix connect");
    s.put(data + 640, &size, 4);
    const auto accepted = call(acceptNid, {static_cast<std::uint64_t>(server), data + 1088, data + 640});
    require(accepted >= 0x10000000 && accepted != server && accepted != client, "posix accept");
    require(s.get<std::uint32_t>(data + 640) == 16, "accept address length");
    s.text(data + 128, "hi");
    require(s.net64("sceNetSend", {static_cast<std::uint64_t>(client), data + 128, 3, 0}) == 3, "posix tcp send");
    require(s.net64("sceNetRecv", {static_cast<std::uint64_t>(accepted), data + 256, 16, 0}) == 3, "posix tcp recv");
    require(call(connectNid, {9999, data + 1056, 16}) == -1 && s.errnoValue() == 9, "connect on a bad descriptor is EBADF");
    require(call(acceptNid, {static_cast<std::uint64_t>(client), 0, 0}) == -1 && s.errnoValue() == 22, "accept on a non-listener is EINVAL");
    for (auto id : {server, client, accepted}) s.net("sceNetSocketClose", {static_cast<std::uint64_t>(id)});
}

void ssl(Session& s) {
    const auto init = [&](std::uint64_t size) { return s.call32("hdpVEUDFW3s", Lib::Ssl, {size}); };
    const auto id = init(0x10000);
    require(id > 0, "ssl init");
    const auto stats = [&](std::int32_t context) {
        return static_cast<std::uint32_t>(s.call32(Session::Nid("sceSslGetMemoryPoolStats"), Lib::Ssl, {static_cast<std::uint64_t>(context), data}));
    };
    require(stats(id) == 0 && s.get<std::uint64_t>(data) == 0x10000, "ssl pool stats");
    require(stats(9999) == 0x8095F006u && s.get<std::uint64_t>(data) == 0, "unknown ssl context: error code and zeroed stats");
    require(static_cast<std::uint32_t>(s.call32(Session::Nid("sceSslGetMemoryPoolStats"), Lib::Ssl, {static_cast<std::uint64_t>(id), 0})) == 0x8095F007u, "null stats");
    require(static_cast<std::uint32_t>(s.call32(Session::Nid("sceSslGetCaList"), Lib::Ssl, {static_cast<std::uint64_t>(id), data})) == 0x8095F004u, "no CA list offline");
    require(s.call32("0K1yQ6Lv-Yc", Lib::Ssl, {static_cast<std::uint64_t>(id)}) == 0, "ssl term");
}

void http2(Session& s) {
    const auto h = [&](const char* name, std::array<std::uint64_t, 6> args = {}) {
        return static_cast<std::uint32_t>(s.call32(Session::Nid(name), Lib::Http2, args));
    };
    const auto context = static_cast<std::int32_t>(h("sceHttp2Init", {1, 2, 0x20000, 4}));
    require(context > 0, "http2 init");
    const auto tmpl = static_cast<std::int32_t>(h("sceHttp2CreateTemplate", {static_cast<std::uint64_t>(context), 0, 2, 1}));
    require(tmpl > 0, "template");
    s.text(data, "GET");
    s.text(data + 16, "https://example.invalid/");
    const auto request = static_cast<std::int32_t>(h("sceHttp2CreateRequestWithURL", {static_cast<std::uint64_t>(tmpl), data, data + 16, 0}));
    require(request > 0 && request != tmpl, "request");
    require(h("sceHttp2SetTimeOut", {static_cast<std::uint64_t>(request), 1000}) == 0, "set timeout");
    require(h("sceHttp2AddRequestHeader", {static_cast<std::uint64_t>(request), data, data, 0}) == 0, "add header");
    for (const char* name : {"sceHttp2SendRequest", "sceHttp2GetStatusCode", "sceHttp2GetAllResponseHeaders", "sceHttp2GetResponseContentLength", "sceHttp2ReadDataAsync"})
        require(h(name, {static_cast<std::uint64_t>(request), data, 0, 0, 0}) == 0x80436063u, std::string(name) + " fails with the offline network error");
    require(h("sceHttp2WebSocketCreateRequest") == 0x80436063u, "websocket");
    // Async: the completion is signalled on the caller's equeue and collected with WaitAsync.
    require(h("sceHttp2SendRequestAsync", {static_cast<std::uint64_t>(request), 0, 0, 0, 0}) == 0x817B1225u, "async without queue option");
    const std::uint8_t option[32] = {0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0, 0, 0, 0, 0, 0};
    s.put(data + 256, option, sizeof(option));
    require(h("sceHttp2SendRequestAsync", {static_cast<std::uint64_t>(request), 0, 0, data + 256, 0}) == 0, "async send");
    require(s.triggered.size() == 1 && s.triggered[0][0] == 0x12345678 && s.triggered[0][1] == 7 && s.triggered[0][2] == 0x2211, "equeue user event");
    require(h("sceHttp2WaitAsync", {static_cast<std::uint64_t>(request), data + 512, 0, 0}) == 0, "wait async");
    require(s.get<std::int32_t>(data + 516) == request && static_cast<std::uint32_t>(s.get<std::int32_t>(data + 520)) == 0x80436063u, "async result");
    require(h("sceHttp2WaitAsync", {static_cast<std::uint64_t>(request), data + 512, 0, 0}) == 0x817B1100u, "nothing left to wait for");
    std::uint8_t dead[32]{};
    dead[0] = 0xad; dead[1] = 0xde;
    s.put(data + 300, dead, sizeof(dead));
    require(h("sceHttp2SendRequestAsync", {static_cast<std::uint64_t>(request), 0, 0, data + 300, 0}) == 0x80436063u, "failed trigger falls back to the network error");
    // Pool stats follow upstream PR 3216.
    require(h("sceHttp2GetMemoryPoolStats", {static_cast<std::uint64_t>(context), data + 1024}) == 0 && s.get<std::uint64_t>(data + 1024) == 0x20000, "pool stats");
    require(h("sceHttp2GetMemoryPoolStats", {static_cast<std::uint64_t>(context), 0}) == 0x817B1225u, "null stats");
    require(h("sceHttp2GetMemoryPoolStats", {777, data + 1024}) == 0x817B1100u && s.get<std::uint64_t>(data + 1024) == 0, "unknown context");
    require(h("sceHttp2DeleteRequest", {static_cast<std::uint64_t>(request)}) == 0 && h("sceHttp2DeleteTemplate", {static_cast<std::uint64_t>(tmpl)}) == 0, "delete");
    require(h("sceHttp2Term", {static_cast<std::uint64_t>(context)}) == 0, "term");
    require(h("sceHttp2GetMemoryPoolStats", {static_cast<std::uint64_t>(context), data + 1024}) == 0x817B1100u, "terminated context");
}

}

int main() {
    try {
        Session session;
        gatesResolve(session);
        pools(session);
        udpAndTcp(session);
        epoll(session);
        resolver(session);
        posixSockets(session);
        ssl(session);
        http2(session);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    std::cout << "SceNetImports tests passed\n";
    return 0;
}
