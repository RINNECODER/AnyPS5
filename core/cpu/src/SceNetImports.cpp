#include <cpu/SceNetImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <span>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Cpu {
namespace {

// FreeBSD errno numbers as the guest sees them.
constexpr int GEINTR = 4, GEIO = 5, GEBADF = 9, GENOMEM = 12, GEFAULT = 14, GEINVAL = 22, GEAGAIN = 35,
              GEINPROGRESS = 36, GENOTSOCK = 38, GEMSGSIZE = 40, GENOPROTOOPT = 42, GEOPNOTSUPP = 45,
              GEAFNOSUPPORT = 47, GEPROTONOSUPPORT = 43, GECONNABORTED = 53, GENOENT = 2, GETIMEDOUT = 60;
constexpr std::uint32_t netErrorBase = 0x80410100u;
constexpr std::uint32_t resolverNoDns = 0x804101E1u;
constexpr std::uint32_t http2ErrorNetwork = 0x80436063u;
constexpr std::uint32_t http2ErrorInvalidId = 0x817B1100u;
constexpr std::uint32_t http2ErrorNullPointer = 0x817B1225u;
constexpr std::uint32_t sslErrorInvalidArg = 0x8095177Au;
constexpr std::uint32_t sslErrorNotFound = 0x8095F004u;
constexpr std::uint32_t sslErrorInvalidId = 0x8095F006u;
constexpr std::uint32_t sslErrorInvalidValue = 0x8095F007u;

constexpr int guestAfInet = 2, guestAfInet6 = 28;
constexpr int solSocket = 0xFFFF;
constexpr int soNbio = 0x1200, soRcvTimeo = 0x1006, soSndTimeo = 0x1005, soError = 0x1007, soType = 0x1008;
constexpr int firstNetSocket = 400, endNetSocket = 1000;
constexpr int firstPosixSocket = 0x10000000;
constexpr std::size_t ioCap = 1u << 20;
constexpr int waitSliceMs = 50;

int guestErrno(int error) {
    switch (error) {
        case 0: return 0;
        case EOPNOTSUPP: return GEOPNOTSUPP;
        case EPROTO: return 92;
        default: return error > 0 && error <= 72 ? error : GEIO;
    }
}

struct Native {
    int fd;
    std::atomic<bool> aborted{false};
    explicit Native(int value) : fd(value) {}
    ~Native() { if (fd >= 0) ::close(fd); }
};

struct Sock {
    int family = 0;
    int type = 0;
    bool posix = false;
    std::shared_ptr<Native> native;
    bool nonblock = false;
    bool bound = false;
    bool listening = false;
    std::int64_t rcvUs = 0;
    std::int64_t sndUs = 0;
};

struct EpollEvent {  // NetEpollEvent: events, reserved, ident, data
    std::uint32_t events;
    std::uint32_t reserved;
    std::uint64_t ident;
    std::uint64_t data;
};
static_assert(sizeof(EpollEvent) == 24);

enum class Scope { Net, Ssl, Http2, Posix };

std::uint16_t swap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }
std::uint32_t swap32(std::uint32_t v) { return __builtin_bswap32(v); }
std::uint64_t swap64(std::uint64_t v) { return __builtin_bswap64(v); }

// Guest sockaddr (len, family, ...) to a macOS sockaddr. Only AF_INET and AF_INET6 exist.
bool toNative(const std::uint8_t* g, std::uint32_t length, int& guestFamily, sockaddr_storage& native, socklen_t& size) {
    std::memset(&native, 0, sizeof(native));
    if (length < 2) return false;
    if (g[1] == guestAfInet && length >= 16) {
        auto& v4 = reinterpret_cast<sockaddr_in&>(native);
        v4.sin_len = sizeof(v4);
        v4.sin_family = AF_INET;
        std::memcpy(&v4.sin_port, g + 2, 2);
        std::memcpy(&v4.sin_addr, g + 4, 4);
        size = sizeof(v4);
        guestFamily = guestAfInet;
        return true;
    }
    if (g[1] == guestAfInet6 && length >= 28) {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(native);
        v6.sin6_len = sizeof(v6);
        v6.sin6_family = AF_INET6;
        std::memcpy(&v6.sin6_port, g + 2, 2);
        std::memcpy(&v6.sin6_flowinfo, g + 4, 4);
        std::memcpy(&v6.sin6_addr, g + 8, 16);
        std::memcpy(&v6.sin6_scope_id, g + 24, 4);
        size = sizeof(v6);
        guestFamily = guestAfInet6;
        return true;
    }
    return false;
}

// macOS sockaddr to the guest layout; returns the byte count of the full guest address.
std::size_t fromNative(const sockaddr_storage& native, std::uint8_t (&out)[28]) {
    std::memset(out, 0, sizeof(out));
    if (native.ss_family == AF_INET) {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(native);
        out[0] = 16;
        out[1] = guestAfInet;
        std::memcpy(out + 2, &v4.sin_port, 2);
        std::memcpy(out + 4, &v4.sin_addr, 4);
        return 16;
    }
    if (native.ss_family == AF_INET6) {
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(native);
        out[0] = 28;
        out[1] = guestAfInet6;
        std::memcpy(out + 2, &v6.sin6_port, 2);
        std::memcpy(out + 4, &v6.sin6_flowinfo, 4);
        std::memcpy(out + 8, &v6.sin6_addr, 16);
        std::memcpy(out + 24, &v6.sin6_scope_id, 4);
        return 28;
    }
    return 0;
}

constexpr std::array<Register, 6> argumentRegisters{Register::Rdi, Register::Rsi, Register::Rdx,
                                                    Register::Rcx, Register::R8, Register::R9};

}

struct SceNetImports::Impl {
    using Handler = std::function<void(Machine&)>;
    struct Entry { Scope scope; Handler handler; };

    Machine& machine;
    SceHostTrampolines trampolines;
    std::uint64_t scratchErrno;
    std::map<std::string, Entry> services;
    std::map<std::string, std::uint64_t> gates;
    std::function<std::uint64_t()> errnoAddress;
    std::function<bool()> stopCheck;
    std::function<int(std::uint64_t, std::int32_t, std::uint64_t)> userEvent;

    std::mutex mutex;
    std::map<int, Sock> socks;
    int nextNet = firstNetSocket;
    int nextPosix = firstPosixSocket;
    std::map<int, std::uint64_t> pools;
    int nextPool = 1;
    std::map<int, int> resolvers;
    int nextResolver = 0x100;
    std::map<int, std::map<int, EpollEvent>> epolls;
    std::set<int> epollAborted;
    int nextEpoll = 0x4000;

    std::mutex httpMutex;
    std::atomic<int> nextHandle{1};
    std::map<int, std::uint64_t> http2Pools;
    std::map<int, std::vector<std::array<std::int32_t, 4>>> completions;
    std::map<int, std::uint64_t> sslPools;

    Impl(Machine& guest, std::uint64_t base)
        : machine(guest), trampolines(guest, base, SceHostTrampolines::DefaultCapacity, "SCE network import"),
          scratchErrno(base + 0x100000) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE network import gates require a nonzero aligned low canonical guest page");
        machine.Map(scratchErrno, 4096, Permission::Read | Permission::Write);
        registerNet();
        registerPosix();
        registerSsl();
        registerHttp2();
    }

    // ---- guest memory and ABI helpers ----
    static std::uint64_t arg(Machine& m, int index) { return m.Get(argumentRegisters[static_cast<std::size_t>(index)]); }
    static int iarg(Machine& m, int index) { return static_cast<int>(static_cast<std::uint32_t>(arg(m, index))); }
    static void ret(Machine& m, std::int64_t value) { m.Set(Register::Rax, static_cast<std::uint64_t>(value)); }

    static bool rd(Machine& m, std::uint64_t address, void* out, std::size_t size) {
        if (!size) return true;
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address) return false;
        try {
            m.CheckAccess(address, size, Permission::Read);
            m.Read(address, std::as_writable_bytes(std::span(static_cast<std::byte*>(out), size)));
            return true;
        } catch (const std::exception&) { return false; }
    }
    static bool wr(Machine& m, std::uint64_t address, const void* in, std::size_t size) {
        if (!size) return true;
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address) return false;
        try {
            m.CheckAccess(address, size, Permission::Write);
            m.Write(address, std::as_bytes(std::span(static_cast<const std::byte*>(in), size)));
            return true;
        } catch (const std::exception&) { return false; }
    }
    static bool rdString(Machine& m, std::uint64_t address, std::string& out, std::size_t limit = 4096) {
        out.clear();
        for (std::size_t index = 0; index < limit; ++index) {
            char c;
            if (!rd(m, address + index, &c, 1)) return false;
            if (!c) return true;
            out.push_back(c);
        }
        return false;
    }

    std::uint64_t errnoCell() {
        std::uint64_t cell = errnoAddress ? errnoAddress() : 0;
        return cell ? cell : scratchErrno;
    }
    void setErrno(Machine& m, int error) {
        const std::int32_t value = error;
        wr(m, errnoCell(), &value, sizeof(value));
    }
    // value >= 0 passes through; -errno becomes the sceNet error code and sets the errno cell.
    void netResult(Machine& m, std::int64_t value) {
        if (value >= 0) { ret(m, value); return; }
        setErrno(m, static_cast<int>(-value));
        ret(m, static_cast<std::int32_t>(netErrorBase | static_cast<std::uint32_t>(-value)));
    }
    void posixResult(Machine& m, std::int64_t value) {
        if (value >= 0) { ret(m, value); return; }
        setErrno(m, static_cast<int>(-value));
        ret(m, -1);
    }

    // ---- socket table ----
    bool lookup(int id, Sock& out) {
        std::lock_guard lock(mutex);
        const auto found = socks.find(id);
        if (found == socks.end()) return false;
        out = found->second;
        return true;
    }
    int insert(Sock sock) {
        std::lock_guard lock(mutex);
        int id = 0;
        if (sock.posix) id = nextPosix++;
        else {
            for (int tries = firstNetSocket; tries < endNetSocket; ++tries) {
                if (nextNet >= endNetSocket) nextNet = firstNetSocket;
                if (!socks.count(nextNet)) { id = nextNet++; break; }
                ++nextNet;
            }
            if (!id) return -GENOMEM;
        }
        socks.emplace(id, std::move(sock));
        return id;
    }
    static int makeNonBlocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -guestErrno(errno);
#ifdef SO_NOSIGPIPE
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        return 0;
    }

    // Waits for the socket to become ready; 0 on ready, -errno otherwise.
    std::int64_t waitFor(const Sock& sock, short events, std::int64_t timeoutUs) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (sock.native->aborted.load()) return -GECONNABORTED;
            if (stopCheck && stopCheck()) return -GEINTR;
            int slice = waitSliceMs;
            if (timeoutUs > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                const auto remaining = timeoutUs - elapsed;
                if (remaining <= 0) return -GEAGAIN;
                slice = static_cast<int>(std::min<std::int64_t>(waitSliceMs, (remaining + 999) / 1000));
            }
            pollfd p{sock.native->fd, events, 0};
            const int result = ::poll(&p, 1, slice);
            if (result < 0) {
                if (errno == EINTR) continue;
                return -guestErrno(errno);
            }
            if (result > 0) return 0;
        }
    }

    // ---- socket operations; negative results are -guest errno ----
    std::int64_t opSocket(int family, int type, int protocol, bool posix) {
        if (family != guestAfInet && family != guestAfInet6) return -GEAFNOSUPPORT;
        bool nonblock = false;
        if (posix) {
            if (type & 0x20000000) nonblock = true;
            type &= ~(0x10000000 | 0x20000000);
        }
        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) return -GEPROTONOSUPPORT;
        const int fd = ::socket(family == guestAfInet ? AF_INET : AF_INET6, type, protocol);
        if (fd < 0) return -guestErrno(errno);
        auto native = std::make_shared<Native>(fd);
        if (const int error = makeNonBlocking(fd)) return error;
        Sock sock;
        sock.family = family;
        sock.type = type;
        sock.posix = posix;
        sock.native = std::move(native);
        sock.nonblock = nonblock;
        return insert(std::move(sock));
    }
    std::int64_t opClose(int id) {
        std::shared_ptr<Native> native;
        {
            std::lock_guard lock(mutex);
            const auto found = socks.find(id);
            if (found == socks.end()) return -GEBADF;
            native = found->second.native;
            socks.erase(found);
        }
        ::shutdown(native->fd, SHUT_RDWR);
        return 0;
    }
    std::int64_t opAbort(int id) {
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        sock.native->aborted = true;
        ::shutdown(sock.native->fd, SHUT_RDWR);
        return 0;
    }
    std::int64_t readAddress(Machine& m, const Sock& sock, std::uint64_t address, std::uint32_t length,
                             sockaddr_storage& native, socklen_t& size) {
        std::uint8_t bytes[28]{};
        if (!address) return -GEINVAL;
        if (!rd(m, address, bytes, std::min<std::uint32_t>(length, sizeof(bytes)))) return -GEFAULT;
        int family = 0;
        if (!toNative(bytes, std::min<std::uint32_t>(length, sizeof(bytes)), family, native, size)) return -GEINVAL;
        if (family != sock.family) return -GEAFNOSUPPORT;
        return 0;
    }
    bool writeAddress(Machine& m, const sockaddr_storage& native, socklen_t nativeLength, std::uint64_t address,
                      std::uint64_t lengthPtr) {
        std::uint32_t capacity = 0;
        if (!rd(m, lengthPtr, &capacity, sizeof(capacity))) return false;
        std::uint8_t bytes[28];
        const std::size_t required = nativeLength ? fromNative(native, bytes) : 0;
        if (nativeLength && !required) return false;
        if (!wr(m, address, bytes, std::min<std::size_t>(capacity, required))) return false;
        const std::uint32_t full = static_cast<std::uint32_t>(required);
        return wr(m, lengthPtr, &full, sizeof(full));
    }
    std::int64_t opBind(Machine& m, int id, std::uint64_t address, std::uint32_t length) {
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        sockaddr_storage native;
        socklen_t size = 0;
        if (const auto error = readAddress(m, sock, address, length, native, size)) return error;
        if (::bind(sock.native->fd, reinterpret_cast<sockaddr*>(&native), size) != 0) return -guestErrno(errno);
        std::lock_guard lock(mutex);
        if (const auto found = socks.find(id); found != socks.end()) found->second.bound = true;
        return 0;
    }
    std::int64_t opListen(int id, int backlog) {
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        if (::listen(sock.native->fd, backlog) != 0) return -guestErrno(errno);
        std::lock_guard lock(mutex);
        if (const auto found = socks.find(id); found != socks.end()) found->second.listening = true;
        return 0;
    }
    std::int64_t opAccept(Machine& m, int id, std::uint64_t address, std::uint64_t lengthPtr) {
        Sock listener;
        if (!lookup(id, listener)) return -GEBADF;
        if (!listener.listening) return -GEINVAL;
        if (address && !lengthPtr) return -GEINVAL;
        for (;;) {
            sockaddr_storage peer{};
            socklen_t peerLength = sizeof(peer);
            const int fd = ::accept(listener.native->fd, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (listener.nonblock) return -GEAGAIN;
                    if (const auto waited = waitFor(listener, POLLIN, listener.rcvUs)) return waited;
                    continue;
                }
                return -guestErrno(errno);
            }
            auto native = std::make_shared<Native>(fd);
            if (const int error = makeNonBlocking(fd)) return error;
            Sock connection;
            connection.family = listener.family;
            connection.type = SOCK_STREAM;
            connection.posix = listener.posix;
            connection.native = std::move(native);
            const int accepted = insert(std::move(connection));
            if (accepted < 0) return accepted;
            if (address && !writeAddress(m, peer, peerLength, address, lengthPtr)) {
                opClose(accepted);
                return -GEFAULT;
            }
            return accepted;
        }
    }
    std::int64_t opConnect(Machine& m, int id, std::uint64_t address, std::uint32_t length) {
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        sockaddr_storage native;
        socklen_t size = 0;
        if (const auto error = readAddress(m, sock, address, length, native, size)) return error;
        if (::connect(sock.native->fd, reinterpret_cast<sockaddr*>(&native), size) == 0) return 0;
        if (errno != EINPROGRESS) return -guestErrno(errno);
        if (sock.nonblock) return -GEINPROGRESS;
        if (const auto waited = waitFor(sock, POLLOUT, sock.sndUs)) return waited == -GEAGAIN ? -GETIMEDOUT : waited;
        int pending = 0;
        socklen_t pendingLength = sizeof(pending);
        if (::getsockopt(sock.native->fd, SOL_SOCKET, SO_ERROR, &pending, &pendingLength) != 0) return -guestErrno(errno);
        return pending ? -guestErrno(pending) : 0;
    }
    static bool receiveFlags(int flags) { return (flags & ~(0x1 | 0x2 | 0x40 | 0x80)) == 0; }
    static bool sendFlags(int flags) { return (flags & ~(0x80 | 0x20000)) == 0; }
    std::int64_t opRecv(Machine& m, int id, std::uint64_t buffer, std::uint64_t length, int flags,
                        std::uint64_t from, std::uint64_t fromLength) {
        if (!receiveFlags(flags)) return -GEOPNOTSUPP;
        if (!buffer && length) return -GEINVAL;
        if (length > INT_MAX) return -GEMSGSIZE;
        if (from && !fromLength) return -GEINVAL;
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        std::vector<std::byte> data(std::min<std::uint64_t>(length, ioCap));
        const bool nonblocking = sock.nonblock || (flags & 0x80);
        const int hostFlags = (flags & 0x1 ? MSG_OOB : 0) | (flags & 0x2 ? MSG_PEEK : 0);
        sockaddr_storage peer{};
        socklen_t peerLength = sizeof(peer);
        ssize_t received;
        for (;;) {
            peerLength = sizeof(peer);
            received = ::recvfrom(sock.native->fd, data.data(), data.size(), hostFlags,
                                  reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (received >= 0) break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (nonblocking) return -GEAGAIN;
                if (const auto waited = waitFor(sock, POLLIN, sock.rcvUs)) return waited;
                continue;
            }
            return -guestErrno(errno);
        }
        if (!wr(m, buffer, data.data(), static_cast<std::size_t>(received))) return -GEFAULT;
        if (from && !writeAddress(m, peer, peerLength, from, fromLength)) return -GEFAULT;
        return received;
    }
    std::int64_t opSend(Machine& m, int id, std::uint64_t buffer, std::uint64_t length, int flags,
                        std::uint64_t to, std::uint32_t toLength) {
        if (!sendFlags(flags)) return -GEOPNOTSUPP;
        if (!buffer && length) return -GEINVAL;
        if (length > INT_MAX) return -GEMSGSIZE;
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        sockaddr_storage destination;
        socklen_t destinationLength = 0;
        if (to) {
            if (const auto error = readAddress(m, sock, to, toLength, destination, destinationLength)) return error;
        } else if (toLength) {
            return -GEINVAL;
        }
        std::vector<std::byte> data(std::min<std::uint64_t>(length, ioCap));
        if (!rd(m, buffer, data.data(), data.size())) return -GEFAULT;
        const bool nonblocking = sock.nonblock || (flags & 0x80);
        for (;;) {
            const ssize_t sent = to ? ::sendto(sock.native->fd, data.data(), data.size(), 0,
                                               reinterpret_cast<sockaddr*>(&destination), destinationLength)
                                    : ::send(sock.native->fd, data.data(), data.size(), 0);
            if (sent >= 0) return sent;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (nonblocking) return -GEAGAIN;
                if (const auto waited = waitFor(sock, POLLOUT, sock.sndUs)) return waited;
                continue;
            }
            return -guestErrno(errno);
        }
    }
    std::int64_t opShutdown(int id, int how) {
        if (how < 0 || how > 2) return -GEINVAL;
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        return ::shutdown(sock.native->fd, how) == 0 ? 0 : -guestErrno(errno);
    }
    std::int64_t opName(Machine& m, int id, std::uint64_t address, std::uint64_t lengthPtr, bool peer) {
        if (!address || !lengthPtr) return -GEINVAL;
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        sockaddr_storage native{};
        socklen_t size = sizeof(native);
        const int result = peer ? ::getpeername(sock.native->fd, reinterpret_cast<sockaddr*>(&native), &size)
                                : ::getsockname(sock.native->fd, reinterpret_cast<sockaddr*>(&native), &size);
        if (result != 0) return -guestErrno(errno);
        return writeAddress(m, native, size, address, lengthPtr) ? 0 : -GEFAULT;
    }
    static std::int64_t timevalMicros(const std::uint8_t* bytes, bool net) {
        if (net) { std::int32_t value; std::memcpy(&value, bytes, 4); return value; }
        std::int64_t seconds, micros;
        std::memcpy(&seconds, bytes, 8);
        std::memcpy(&micros, bytes + 8, 8);
        if (seconds < 0 || micros < 0 || micros >= 1000000) return -1;
        return seconds * 1000000 + micros;
    }
    std::int64_t opSetOption(Machine& m, int id, int level, int name, std::uint64_t value, std::uint32_t length, bool net) {
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        const int unsupported = net ? -GEOPNOTSUPP : -GENOPROTOOPT;
        std::uint8_t bytes[64]{};
        if (!value || length < sizeof(int)) return -GEINVAL;
        if (!rd(m, value, bytes, std::min<std::uint32_t>(length, sizeof(bytes)))) return -GEFAULT;
        std::int32_t integer;
        std::memcpy(&integer, bytes, sizeof(integer));
        if (level == solSocket) {
            if (name == soNbio) {
                std::lock_guard lock(mutex);
                if (const auto found = socks.find(id); found != socks.end()) found->second.nonblock = integer != 0;
                return 0;
            }
            if (name == soRcvTimeo || name == soSndTimeo) {
                if (!net && length < 16) return -GEINVAL;
                const auto micros = timevalMicros(bytes, net);
                if (micros < 0) return -GEINVAL;
                std::lock_guard lock(mutex);
                if (const auto found = socks.find(id); found != socks.end())
                    (name == soRcvTimeo ? found->second.rcvUs : found->second.sndUs) = micros;
                return 0;
            }
            switch (name) {
                case 0x4: case 0x8: case 0x20: case 0x200: case 0x1001: case 0x1002: case 0x800: {
                    if (::setsockopt(sock.native->fd, SOL_SOCKET, name, &integer, sizeof(integer)) != 0)
                        return -guestErrno(errno);
                    return 0;
                }
                case 0x80: {
                    if (length < 8) return -GEINVAL;
                    linger value{};
                    std::memcpy(&value.l_onoff, bytes, 4);
                    std::memcpy(&value.l_linger, bytes + 4, 4);
                    return ::setsockopt(sock.native->fd, SOL_SOCKET, SO_LINGER, &value, sizeof(value)) == 0 ? 0 : -guestErrno(errno);
                }
                default: return unsupported;
            }
        }
        if (level == IPPROTO_TCP && name == TCP_NODELAY)
            return ::setsockopt(sock.native->fd, IPPROTO_TCP, TCP_NODELAY, &integer, sizeof(integer)) == 0 ? 0 : -guestErrno(errno);
        if (level == IPPROTO_IP) {
            switch (name) {
                case IP_TTL: case IP_MULTICAST_TTL: case IP_MULTICAST_LOOP: case IP_MULTICAST_IF:
                case IP_ADD_MEMBERSHIP: case IP_DROP_MEMBERSHIP:
                    return ::setsockopt(sock.native->fd, IPPROTO_IP, name, bytes, std::min<std::uint32_t>(length, sizeof(bytes))) == 0
                               ? 0 : -guestErrno(errno);
                default: return unsupported;
            }
        }
        return unsupported;
    }
    std::int64_t opGetOption(Machine& m, int id, int level, int name, std::uint64_t value, std::uint64_t lengthPtr, bool net) {
        Sock sock;
        if (!lookup(id, sock)) return -GEBADF;
        std::uint32_t capacity = 0;
        if (!value || !lengthPtr) return -GEINVAL;
        if (!rd(m, lengthPtr, &capacity, sizeof(capacity))) return -GEFAULT;
        if (capacity < sizeof(int)) return -GEINVAL;
        const int unsupported = net ? -GEOPNOTSUPP : -GENOPROTOOPT;
        std::uint8_t out[16]{};
        std::uint32_t size = sizeof(int);
        std::int32_t integer = 0;
        if (level == solSocket) {
            switch (name) {
                case soNbio: integer = sock.nonblock ? 1 : 0; break;
                case soRcvTimeo: case soSndTimeo: {
                    const auto micros = name == soRcvTimeo ? sock.rcvUs : sock.sndUs;
                    if (net) integer = static_cast<std::int32_t>(micros);
                    else {
                        if (capacity < 16) return -GEINVAL;
                        const std::int64_t seconds = micros / 1000000, rest = micros % 1000000;
                        std::memcpy(out, &seconds, 8);
                        std::memcpy(out + 8, &rest, 8);
                        size = 16;
                    }
                    break;
                }
                case soType: integer = sock.type; break;
                case soError: {
                    int pending = 0;
                    socklen_t pendingLength = sizeof(pending);
                    if (::getsockopt(sock.native->fd, SOL_SOCKET, SO_ERROR, &pending, &pendingLength) != 0)
                        return -guestErrno(errno);
                    integer = guestErrno(pending);
                    break;
                }
                case 0x4: case 0x8: case 0x20: case 0x200: case 0x1001: case 0x1002: case 0x800: {
                    socklen_t length = sizeof(integer);
                    if (::getsockopt(sock.native->fd, SOL_SOCKET, name, &integer, &length) != 0) return -guestErrno(errno);
                    break;
                }
                default: return unsupported;
            }
        } else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
            socklen_t length = sizeof(integer);
            if (::getsockopt(sock.native->fd, IPPROTO_TCP, TCP_NODELAY, &integer, &length) != 0) return -guestErrno(errno);
        } else {
            return unsupported;
        }
        if (size == sizeof(int)) std::memcpy(out, &integer, sizeof(integer));
        if (!wr(m, value, out, size) || !wr(m, lengthPtr, &size, sizeof(size))) return -GEFAULT;
        return 0;
    }

    // POSIX select over the socket table. fd_set is 1024 bits; sockets above that cannot be selected.
    std::int64_t opSelect(Machine& m, int nfds, std::uint64_t readSet, std::uint64_t writeSet, std::uint64_t exceptSet,
                          std::uint64_t timeoutPtr) {
        if (nfds < 0 || nfds > 1024) return -GEINVAL;
        std::int64_t timeoutUs = -1;
        if (timeoutPtr) {
            std::uint8_t raw[16];
            if (!rd(m, timeoutPtr, raw, sizeof(raw))) return -GEFAULT;
            timeoutUs = timevalMicros(raw, false);
            if (timeoutUs < 0) return -GEINVAL;
        }
        const std::size_t words = static_cast<std::size_t>((nfds + 63) / 64);
        std::uint64_t sets[3][16]{};
        const std::uint64_t pointers[3] = {readSet, writeSet, exceptSet};
        for (int index = 0; index < 3; ++index)
            if (pointers[index] && !rd(m, pointers[index], sets[index], words * 8)) return -GEFAULT;
        struct Watch { int id; Sock sock; short events; };
        std::vector<Watch> watches;
        for (int fd = 0; fd < nfds; ++fd) {
            short events = 0;
            const auto bit = [&](int set) { return (sets[set][fd / 64] >> (fd % 64)) & 1u; };
            if (bit(0)) events |= POLLIN;
            if (bit(1)) events |= POLLOUT;
            if (bit(2)) events |= POLLPRI;
            if (!events) continue;
            Sock sock;
            if (!lookup(fd, sock)) return -GEBADF;
            watches.push_back({fd, std::move(sock), events});
        }
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            int slice = waitSliceMs;
            if (timeoutUs >= 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                slice = static_cast<int>(std::clamp<std::int64_t>((timeoutUs - elapsed + 999) / 1000, 0, waitSliceMs));
            }
            if (stopCheck && stopCheck()) return -GEINTR;
            std::vector<pollfd> polls;
            for (const auto& watch : watches) polls.push_back({watch.sock.native->fd, watch.events, 0});
            int ready = 0;
            if (polls.empty()) {
                if (slice > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            } else {
                ready = ::poll(polls.data(), static_cast<nfds_t>(polls.size()), slice);
                if (ready < 0 && errno != EINTR) return -guestErrno(errno);
            }
            const bool expired = timeoutUs >= 0 &&
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() >= timeoutUs;
            if (ready > 0) {
                std::uint64_t out[3][16]{};
                int count = 0;
                for (std::size_t index = 0; index < watches.size(); ++index) {
                    const int fd = watches[index].id;
                    const short got = polls[index].revents;
                    const auto mark = [&](int set) { out[set][fd / 64] |= std::uint64_t{1} << (fd % 64); ++count; };
                    if ((watches[index].events & POLLIN) && (got & (POLLIN | POLLHUP | POLLERR))) mark(0);
                    if ((watches[index].events & POLLOUT) && (got & (POLLOUT | POLLERR))) mark(1);
                    if ((watches[index].events & POLLPRI) && (got & POLLPRI)) mark(2);
                }
                if (count > 0) {
                    for (int set = 0; set < 3; ++set)
                        if (pointers[set] && !wr(m, pointers[set], out[set], words * 8)) return -GEFAULT;
                    return count;
                }
            }
            if (expired) {
                const std::uint64_t zero[16]{};
                for (int set = 0; set < 3; ++set)
                    if (pointers[set] && !wr(m, pointers[set], zero, words * 8)) return -GEFAULT;
                return 0;
            }
        }
    }

    // ---- registration ----
    void add(Scope scope, const std::string& name, Handler handler) {
        const auto nid = scope == Scope::Posix ? Nid::ComputeNid(name, "libkernel") : Nid::ComputeNid(name, "");
        services[nid] = Entry{scope, std::move(handler)};
    }
    void addNid(Scope scope, const std::string& nid, Handler handler) { services[nid] = Entry{scope, std::move(handler)}; }

    void registerNet() {
        const auto net = [this](const char* name, std::function<std::int64_t(Machine&)> body) {
            add(Scope::Net, name, [this, body = std::move(body)](Machine& m) { netResult(m, body(m)); });
        };
        net("sceNetInit", [](Machine&) { return std::int64_t{0}; });
        net("sceNetTerm", [](Machine&) { return std::int64_t{0}; });
        add(Scope::Net, "sceNetErrnoLoc", [this](Machine& m) { ret(m, static_cast<std::int64_t>(errnoCell())); });
        add(Scope::Net, "sceNetHtonll", [](Machine& m) { ret(m, static_cast<std::int64_t>(swap64(arg(m, 0)))); });
        add(Scope::Net, "sceNetNtohll", [](Machine& m) { ret(m, static_cast<std::int64_t>(swap64(arg(m, 0)))); });
        add(Scope::Net, "sceNetNtohl", [](Machine& m) { m.Set(Register::Rax, swap32(static_cast<std::uint32_t>(arg(m, 0)))); });
        add(Scope::Net, "sceNetNtohs", [](Machine& m) { m.Set(Register::Rax, swap16(static_cast<std::uint16_t>(arg(m, 0)))); });
        net("sceNetPoolCreate", [this](Machine& m) -> std::int64_t {
            const int size = iarg(m, 1);
            if (size <= 0) return -GEINVAL;
            std::lock_guard lock(mutex);
            const int id = nextPool++;
            pools[id] = static_cast<std::uint64_t>(size);
            return id;
        });
        net("sceNetPoolDestroy", [this](Machine& m) -> std::int64_t {
            std::lock_guard lock(mutex);
            return pools.erase(iarg(m, 0)) ? 0 : -GEBADF;
        });
        net("sceNetGetMemoryPoolStats", [this](Machine& m) -> std::int64_t {
            const auto stats = arg(m, 1);
            if (!stats) return -GEINVAL;
            std::uint64_t size;
            {
                std::lock_guard lock(mutex);
                const auto pool = pools.find(iarg(m, 0));
                if (pool == pools.end()) return -GEBADF;
                size = pool->second;
            }
            const std::uint64_t out[3] = {size, 0, 0};
            return wr(m, stats, out, sizeof(out)) ? 0 : -GEFAULT;
        });
        net("sceNetSocket", [this](Machine& m) { return opSocket(iarg(m, 1), iarg(m, 2), iarg(m, 3), false); });
        net("sceNetSocketClose", [this](Machine& m) { return opClose(iarg(m, 0)); });
        net("sceNetSocketAbort", [this](Machine& m) { return opAbort(iarg(m, 0)); });
        net("sceNetBind", [this](Machine& m) { return opBind(m, iarg(m, 0), arg(m, 1), static_cast<std::uint32_t>(arg(m, 2))); });
        net("sceNetListen", [this](Machine& m) { return opListen(iarg(m, 0), iarg(m, 1)); });
        net("sceNetAccept", [this](Machine& m) { return opAccept(m, iarg(m, 0), arg(m, 1), arg(m, 2)); });
        net("sceNetConnect", [this](Machine& m) { return opConnect(m, iarg(m, 0), arg(m, 1), static_cast<std::uint32_t>(arg(m, 2))); });
        net("sceNetRecv", [this](Machine& m) { return opRecv(m, iarg(m, 0), arg(m, 1), arg(m, 2), iarg(m, 3), 0, 0); });
        net("sceNetRecvfrom", [this](Machine& m) { return opRecv(m, iarg(m, 0), arg(m, 1), arg(m, 2), iarg(m, 3), arg(m, 4), arg(m, 5)); });
        net("sceNetSend", [this](Machine& m) { return opSend(m, iarg(m, 0), arg(m, 1), arg(m, 2), iarg(m, 3), 0, 0); });
        net("sceNetSendto", [this](Machine& m) {
            return opSend(m, iarg(m, 0), arg(m, 1), arg(m, 2), iarg(m, 3), arg(m, 4), static_cast<std::uint32_t>(arg(m, 5)));
        });
        net("sceNetShutdown", [this](Machine& m) { return opShutdown(iarg(m, 0), iarg(m, 1)); });
        net("sceNetSetsockopt", [this](Machine& m) {
            return opSetOption(m, iarg(m, 0), iarg(m, 1), iarg(m, 2), arg(m, 3), static_cast<std::uint32_t>(arg(m, 4)), true);
        });
        net("sceNetGetsockopt", [this](Machine& m) { return opGetOption(m, iarg(m, 0), iarg(m, 1), iarg(m, 2), arg(m, 3), arg(m, 4), true); });
        net("sceNetGetsockname", [this](Machine& m) { return opName(m, iarg(m, 0), arg(m, 1), arg(m, 2), false); });
        net("sceNetGetpeername", [this](Machine& m) { return opName(m, iarg(m, 0), arg(m, 1), arg(m, 2), true); });
        net("sceNetGetSockInfo", [this](Machine& m) -> std::int64_t { Sock sock; return lookup(iarg(m, 0), sock) ? 0 : -GEBADF; });
        registerResolver(net);
        registerEpoll(net);
    }

    // Resolver: IPv4 only, through the host resolver, with the console's no-DNS error on failure.
    std::int64_t resolve(Machine& m, int rid, std::uint64_t hostname, std::vector<std::uint32_t>& addresses) {
        std::string name;
        {
            std::lock_guard lock(mutex);
            if (!resolvers.count(rid)) return -GEBADF;
        }
        if (!hostname) return -GEINVAL;
        if (!rdString(m, hostname, name)) return -GEFAULT;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* results = nullptr;
        const int status = ::getaddrinfo(name.c_str(), nullptr, &hints, &results);
        if (status != 0) {
            setErrno(m, status == EAI_AGAIN ? GETIMEDOUT : GENOENT);
            std::lock_guard lock(mutex);
            if (const auto found = resolvers.find(rid); found != resolvers.end()) found->second = static_cast<int>(resolverNoDns);
            return -static_cast<std::int64_t>(0x100000000ll);  // marker: resolver failure, not an errno
        }
        for (const addrinfo* entry = results; entry; entry = entry->ai_next) {
            const auto address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr;
            if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.push_back(address);
        }
        ::freeaddrinfo(results);
        std::lock_guard lock(mutex);
        if (const auto found = resolvers.find(rid); found != resolvers.end()) found->second = 0;
        return 0;
    }
    template<class Fn> void registerResolver(Fn&& net) {
        net("sceNetResolverCreate", [this](Machine&) -> std::int64_t {
            std::lock_guard lock(mutex);
            const int id = nextResolver++;
            resolvers[id] = 0;
            return id;
        });
        net("sceNetResolverDestroy", [this](Machine& m) -> std::int64_t {
            std::lock_guard lock(mutex);
            return resolvers.erase(iarg(m, 0)) ? 0 : -GEBADF;
        });
        const auto finish = [this](Machine& m, std::int64_t status) {
            if (status == -static_cast<std::int64_t>(0x100000000ll)) { ret(m, static_cast<std::int32_t>(resolverNoDns)); return true; }
            return false;
        };
        add(Scope::Net, "sceNetResolverStartNtoa", [this, finish](Machine& m) {
            if (!arg(m, 1) || !arg(m, 2)) { netResult(m, -GEINVAL); return; }
            std::vector<std::uint32_t> addresses;
            auto status = resolve(m, iarg(m, 0), arg(m, 1), addresses);
            if (finish(m, status)) return;
            if (status == 0 && !wr(m, arg(m, 2), addresses.data(), sizeof(std::uint32_t))) status = -GEFAULT;
            netResult(m, status);
        });
        add(Scope::Net, "sceNetResolverStartNtoaMultipleRecordsEx", [this, finish](Machine& m) {
            if (!arg(m, 1) || !arg(m, 2)) { netResult(m, -GEINVAL); return; }
            if (iarg(m, 5) != 0) { netResult(m, -GEINVAL); return; }
            std::vector<std::uint32_t> addresses;
            auto status = resolve(m, iarg(m, 0), arg(m, 1), addresses);
            if (finish(m, status)) return;
            if (status == 0) {
                // NetResolverInfo: 10 records of 32 bytes (address, 12 bytes of v6, family, 12 reserved), count, count4.
                std::uint8_t info[384]{};
                const std::int32_t count = static_cast<std::int32_t>(std::min<std::size_t>(addresses.size(), 10));
                for (std::int32_t index = 0; index < count; ++index) {
                    std::memcpy(info + index * 32, &addresses[static_cast<std::size_t>(index)], 4);
                    const std::int32_t family = guestAfInet;
                    std::memcpy(info + index * 32 + 16, &family, 4);
                }
                std::memcpy(info + 320, &count, 4);
                std::memcpy(info + 324, &count, 4);
                if (!wr(m, arg(m, 2), info, sizeof(info))) status = -GEFAULT;
            }
            netResult(m, status);
        });
        // The sceNetResolverStartNtoaMultipleRecords entry (no Ex) takes the same arguments.
        add(Scope::Net, "sceNetResolverStartNtoaMultipleRecords", [this](Machine& m) {
            services.at(Nid::ComputeNid("sceNetResolverStartNtoaMultipleRecordsEx", "")).handler(m);
        });
        add(Scope::Net, "sceNetResolverStartAton", [this](Machine& m) {
            const int rid = iarg(m, 0);
            const auto address = arg(m, 1), hostname = arg(m, 2);
            const int length = iarg(m, 3);
            if (!address || !hostname || length <= 0) { netResult(m, -GEINVAL); return; }
            {
                std::lock_guard lock(mutex);
                if (!resolvers.count(rid)) { netResult(m, -GEBADF); return; }
            }
            sockaddr_in native{};
            native.sin_len = sizeof(native);
            native.sin_family = AF_INET;
            if (!rd(m, address, &native.sin_addr, 4)) { netResult(m, -GEFAULT); return; }
            std::vector<char> name(static_cast<std::size_t>(std::min(length, 1025)));
            const int status = ::getnameinfo(reinterpret_cast<sockaddr*>(&native), sizeof(native), name.data(),
                                             static_cast<socklen_t>(name.size()), nullptr, 0, NI_NAMEREQD);
            std::lock_guard lock(mutex);
            if (status != 0) {
                resolvers[rid] = static_cast<int>(resolverNoDns);
                setErrno(m, status == EAI_AGAIN ? GETIMEDOUT : GENOENT);
                ret(m, static_cast<std::int32_t>(resolverNoDns));
                return;
            }
            resolvers[rid] = 0;
            if (!wr(m, hostname, name.data(), std::strlen(name.data()) + 1)) { netResult(m, -GEFAULT); return; }
            ret(m, 0);
        });
        net("sceNetResolverGetError", [this](Machine& m) -> std::int64_t {
            if (!arg(m, 1)) return -GEINVAL;
            int status;
            {
                std::lock_guard lock(mutex);
                const auto found = resolvers.find(iarg(m, 0));
                if (found == resolvers.end()) return -GEBADF;
                status = found->second;
            }
            return wr(m, arg(m, 1), &status, sizeof(status)) ? 0 : -GEFAULT;
        });
        net("sceNetResolverAbort", [this](Machine& m) -> std::int64_t {
            std::lock_guard lock(mutex);
            return resolvers.count(iarg(m, 0)) ? 0 : -GEBADF;
        });
    }

    template<class Fn> void registerEpoll(Fn&& net) {
        net("sceNetEpollCreate", [this](Machine&) -> std::int64_t {
            std::lock_guard lock(mutex);
            const int id = nextEpoll++;
            epolls.emplace(id, std::map<int, EpollEvent>{});
            return id;
        });
        net("sceNetEpollDestroy", [this](Machine& m) -> std::int64_t {
            std::lock_guard lock(mutex);
            const int id = iarg(m, 0);
            if (!epolls.erase(id)) return -GEBADF;
            epollAborted.erase(id);
            return 0;
        });
        net("sceNetEpollAbort", [this](Machine& m) -> std::int64_t {
            std::lock_guard lock(mutex);
            const int id = iarg(m, 0);
            if (!epolls.count(id)) return -GEBADF;
            epollAborted.insert(id);
            return 0;
        });
        net("sceNetEpollControl", [this](Machine& m) -> std::int64_t {
            const int eid = iarg(m, 0), op = iarg(m, 1), id = iarg(m, 2);
            EpollEvent event{};
            if (op < 1 || op > 3) return -GEINVAL;
            if (op != 2) {
                if (!arg(m, 3)) return -GEINVAL;
                if (!rd(m, arg(m, 3), &event, sizeof(event))) return -GEFAULT;
            }
            std::lock_guard lock(mutex);
            const auto set = epolls.find(eid);
            if (set == epolls.end()) return -GEBADF;
            if (op != 2 && !socks.count(id)) return -GEBADF;
            auto& registrations = set->second;
            if (op == 1) return registrations.emplace(id, event).second ? 0 : -48;
            if (op == 2) return registrations.erase(id) ? 0 : -GENOENT;
            const auto found = registrations.find(id);
            if (found == registrations.end()) return -GENOENT;
            found->second = event;
            return 0;
        });
        net("sceNetEpollWait", [this](Machine& m) -> std::int64_t {
            const int eid = iarg(m, 0);
            const auto events = arg(m, 1);
            const int maxEvents = iarg(m, 2);
            const int timeout = iarg(m, 3);  // microseconds; negative waits forever
            if (!events || maxEvents <= 0) return -GEINVAL;
            const auto start = std::chrono::steady_clock::now();
            for (;;) {
                std::vector<EpollEvent> registered;
                std::vector<std::shared_ptr<Native>> natives;
                {
                    std::lock_guard lock(mutex);
                    const auto set = epolls.find(eid);
                    if (set == epolls.end()) return -GEBADF;
                    if (epollAborted.erase(eid)) return -GECONNABORTED;
                    for (const auto& [id, event] : set->second) {
                        const auto sock = socks.find(id);
                        if (sock == socks.end()) continue;
                        registered.push_back(event);
                        natives.push_back(sock->second.native);
                    }
                }
                if (stopCheck && stopCheck()) return -GEINTR;
                int slice = waitSliceMs;
                if (timeout >= 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                    slice = static_cast<int>(std::clamp<std::int64_t>((timeout - elapsed + 999) / 1000, 0, waitSliceMs));
                }
                std::vector<pollfd> polls(registered.size());
                for (std::size_t index = 0; index < polls.size(); ++index) {
                    polls[index].fd = natives[index]->fd;
                    if (registered[index].events & 1) polls[index].events |= POLLIN;
                    if (registered[index].events & 4) polls[index].events |= POLLOUT;
                }
                int ready = 0;
                if (polls.empty()) {
                    if (slice > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                } else {
                    ready = ::poll(polls.data(), static_cast<nfds_t>(polls.size()), slice);
                    if (ready < 0 && errno != EINTR) return -guestErrno(errno);
                }
                {
                    std::lock_guard lock(mutex);
                    if (!epolls.count(eid)) return -GEBADF;
                    if (epollAborted.erase(eid)) return -GECONNABORTED;
                }
                if (ready > 0) {
                    std::vector<EpollEvent> out;
                    for (std::size_t index = 0; index < polls.size() && out.size() < static_cast<std::size_t>(maxEvents); ++index) {
                        const short got = polls[index].revents;
                        if (!got) continue;
                        EpollEvent event = registered[index];
                        event.events = ((got & (POLLIN | POLLPRI)) ? 1u : 0u) | ((got & POLLOUT) ? 4u : 0u) |
                                       ((got & POLLERR) ? 8u : 0u) | ((got & POLLHUP) ? 16u : 0u);
                        out.push_back(event);
                    }
                    if (!out.empty()) return wr(m, events, out.data(), out.size() * sizeof(EpollEvent)) ? static_cast<std::int64_t>(out.size()) : -GEFAULT;
                }
                if (timeout >= 0 && std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() >= timeout)
                    return 0;
            }
        });
    }

    void registerPosix() {
        const auto posix = [this](const char* name, std::function<std::int64_t(Machine&)> body) {
            add(Scope::Posix, name, [this, body = std::move(body)](Machine& m) { posixResult(m, body(m)); });
        };
        posix("socket", [this](Machine& m) { return opSocket(iarg(m, 0), iarg(m, 1), iarg(m, 2), true); });
        posix("bind", [this](Machine& m) { return opBind(m, iarg(m, 0), arg(m, 1), static_cast<std::uint32_t>(arg(m, 2))); });
        posix("connect", [this](Machine& m) { return opConnect(m, iarg(m, 0), arg(m, 1), static_cast<std::uint32_t>(arg(m, 2))); });
        posix("listen", [this](Machine& m) { return opListen(iarg(m, 0), iarg(m, 1)); });
        posix("accept", [this](Machine& m) { return opAccept(m, iarg(m, 0), arg(m, 1), arg(m, 2)); });
        posix("setsockopt", [this](Machine& m) {
            return opSetOption(m, iarg(m, 0), iarg(m, 1), iarg(m, 2), arg(m, 3), static_cast<std::uint32_t>(arg(m, 4)), false);
        });
        posix("getsockopt", [this](Machine& m) { return opGetOption(m, iarg(m, 0), iarg(m, 1), iarg(m, 2), arg(m, 3), arg(m, 4), false); });
        posix("sendto", [this](Machine& m) {
            return opSend(m, iarg(m, 0), arg(m, 1), arg(m, 2), iarg(m, 3), arg(m, 4), static_cast<std::uint32_t>(arg(m, 5)));
        });
        posix("recvfrom", [this](Machine& m) {
            return opRecv(m, iarg(m, 0), arg(m, 1), arg(m, 2), iarg(m, 3), arg(m, 4), arg(m, 5));
        });
        posix("select", [this](Machine& m) { return opSelect(m, iarg(m, 0), arg(m, 1), arg(m, 2), arg(m, 3), arg(m, 4)); });
    }

    // ---- libSceSsl: contexts and certificate stores without any network ----
    void registerSsl() {
        const auto ssl = [this](const char* name, std::function<std::uint32_t(Machine&)> body) {
            add(Scope::Ssl, name, [body = std::move(body)](Machine& m) { ret(m, static_cast<std::int32_t>(body(m))); });
        };
        ssl("sceSslInit", [this](Machine& m) -> std::uint32_t {
            const int id = nextHandle.fetch_add(1);
            std::lock_guard lock(httpMutex);
            sslPools[id] = arg(m, 0);
            return static_cast<std::uint32_t>(id);
        });
        ssl("sceSslTerm", [this](Machine& m) -> std::uint32_t {
            std::lock_guard lock(httpMutex);
            sslPools.erase(iarg(m, 0));
            return 0;
        });
        const auto known = [this](Machine& m) {
            std::lock_guard lock(httpMutex);
            return sslPools.count(iarg(m, 0)) != 0;
        };
        ssl("sceSslGetMemoryPoolStats", [this](Machine& m) -> std::uint32_t {
            if (!arg(m, 1)) return sslErrorInvalidValue;
            std::uint64_t size = 0;
            bool found;
            {
                std::lock_guard lock(httpMutex);
                const auto pool = sslPools.find(iarg(m, 0));
                found = pool != sslPools.end();
                if (found) size = pool->second;
            }
            const std::uint64_t out[4] = {found ? size : 0, 0, 0, 0};
            if (!wr(m, arg(m, 1), out, sizeof(out))) return sslErrorInvalidValue;
            return found ? 0 : sslErrorInvalidId;
        });
        ssl("sceSslFreeCaCerts", [](Machine& m) -> std::uint32_t {
            if (!arg(m, 1)) return sslErrorInvalidArg;
            const std::uint8_t zero[24]{};
            return wr(m, arg(m, 1), zero, sizeof(zero)) ? 0 : sslErrorInvalidArg;
        });
        ssl("sceSslGetCaCerts", [](Machine& m) -> std::uint32_t {
            if (!arg(m, 1)) return sslErrorInvalidArg;
            const std::uint8_t zero[24]{};
            return wr(m, arg(m, 1), zero, sizeof(zero)) ? sslErrorNotFound : sslErrorInvalidArg;
        });
        ssl("sceSslGetCaList", [known](Machine& m) -> std::uint32_t {
            if (!arg(m, 1)) return sslErrorInvalidArg;
            if (!known(m)) return sslErrorInvalidId;
            const std::uint8_t zero[16]{};
            return wr(m, arg(m, 1), zero, sizeof(zero)) ? sslErrorNotFound : sslErrorInvalidArg;
        });
        ssl("sceSslFreeCaList", [known](Machine& m) -> std::uint32_t {
            if (!arg(m, 1)) return sslErrorInvalidArg;
            if (!known(m)) return sslErrorInvalidId;
            std::uint8_t list[16];
            if (!rd(m, arg(m, 1), list, sizeof(list))) return sslErrorInvalidArg;
            static const std::uint8_t zero[16]{};
            return std::memcmp(list, zero, sizeof(list)) == 0 ? 0 : sslErrorInvalidArg;
        });
        ssl("sceSslUnloadCert", [known](Machine& m) -> std::uint32_t { return known(m) ? 0 : sslErrorInvalidId; });
        // Certificate inspection needs a real certificate store; offline there is nothing to inspect.
        for (const char* name : {"sceSslClose", "sceSslGetSerialNumber", "sceSslLoadCert", "sceSslFreeSslCertName",
                                 "sceSslGetIssuerName", "sceSslGetNameEntryCount", "sceSslGetNameEntryInfo",
                                 "sceSslGetPem", "sceSslGetSubjectName"})
            ssl(name, [](Machine&) -> std::uint32_t { return sslErrorInvalidValue; });
    }

    // ---- libSceHttp2: objects and templates exist, every request fails like an offline console ----
    void registerHttp2() {
        const auto http = [this](const char* name, std::function<std::uint32_t(Machine&)> body) {
            add(Scope::Http2, name, [body = std::move(body)](Machine& m) { ret(m, static_cast<std::int32_t>(body(m))); });
        };
        const auto zero = [](Machine&) -> std::uint32_t { return 0; };
        const auto network = [](Machine&) -> std::uint32_t { return http2ErrorNetwork; };
        const auto handle = [this](Machine&) -> std::uint32_t { return static_cast<std::uint32_t>(nextHandle.fetch_add(1)); };
        for (const char* name : {"sceHttp2AddRequestHeader", "sceHttp2DeleteTemplate", "sceHttp2SetAuthEnabled",
                                 "sceHttp2SetAutoRedirect", "sceHttp2SetConnectionWaitTimeOut", "sceHttp2SetConnectTimeOut",
                                 "sceHttp2SetInflateGZIPEnabled", "sceHttp2SetRecvTimeOut", "sceHttp2SetRedirectCallback",
                                 "sceHttp2SetRequestContentLength", "sceHttp2SetResolveTimeOut", "sceHttp2SetSendTimeOut",
                                 "sceHttp2SetSslCallback", "sceHttp2SetTimeOut", "sceHttp2SslDisableOption",
                                 "sceHttp2SslEnableOption", "sceHttp2SetMinSslVersion", "sceHttp2AbortRequest",
                                 "sceHttp2CookieFlush", "sceHttp2SetCookieBox", "sceHttp2SetRequestNoContentLength",
                                 "sceHttp2SetResolveRetry"})
            http(name, zero);
        for (const char* name : {"sceHttp2GetAllResponseHeaders", "sceHttp2GetResponseContentLength", "sceHttp2GetStatusCode",
                                 "sceHttp2ReadData", "sceHttp2ReadDataAsync", "sceHttp2SendRequest",
                                 "sceHttp2WebSocketCreateRequest", "sceHttp2WebSocketCloseAsync",
                                 "sceHttp2WebSocketSendTextMessageAsync", "sceHttp2WebSocketSendDataMessageAsync"})
            http(name, network);
        http("sceHttp2CreateRequestWithURL", handle);
        http("sceHttp2CreateTemplate", handle);
        http("sceHttp2CreateCookieBox", handle);
        http("sceHttp2Init", [this](Machine& m) -> std::uint32_t {
            const int id = nextHandle.fetch_add(1);
            std::lock_guard lock(httpMutex);
            http2Pools[id] = arg(m, 2);
            return static_cast<std::uint32_t>(id);
        });
        http("sceHttp2Term", [this](Machine& m) -> std::uint32_t {
            std::lock_guard lock(httpMutex);
            http2Pools.erase(iarg(m, 0));
            return 0;
        });
        http("sceHttp2DeleteRequest", [this](Machine& m) -> std::uint32_t {
            std::lock_guard lock(httpMutex);
            completions.erase(iarg(m, 0));
            return 0;
        });
        http("sceHttp2GetMemoryPoolStats", [this](Machine& m) -> std::uint32_t {
            if (!arg(m, 1)) return http2ErrorNullPointer;
            std::uint64_t size = 0;
            bool found;
            {
                std::lock_guard lock(httpMutex);
                const auto pool = http2Pools.find(iarg(m, 0));
                found = pool != http2Pools.end();
                if (found) size = pool->second;
            }
            const std::uint64_t out[4] = {found ? size : 0, 0, 0, 0};
            if (!wr(m, arg(m, 1), out, sizeof(out))) return http2ErrorNullPointer;
            return found ? 0 : http2ErrorInvalidId;
        });
        // The completion is the network error; it is delivered through the caller's equeue user event.
        http("sceHttp2SendRequestAsync", [this](Machine& m) -> std::uint32_t {
            const int request = iarg(m, 0);
            const auto option = arg(m, 3);
            if (!option) return http2ErrorNullPointer;
            std::uint8_t raw[32];
            if (!rd(m, option, raw, sizeof(raw))) return http2ErrorNullPointer;
            std::uint64_t queue, userData;
            std::int32_t eventId;
            std::memcpy(&queue, raw, 8);
            std::memcpy(&eventId, raw + 8, 4);
            std::memcpy(&userData, raw + 16, 8);
            if (!userEvent) return http2ErrorNetwork;
            {
                std::lock_guard lock(httpMutex);
                completions[request].push_back({0, request, static_cast<std::int32_t>(http2ErrorNetwork), 0});
            }
            if (userEvent(queue, eventId, userData) != 0) {
                std::lock_guard lock(httpMutex);
                completions[request].pop_back();
                return http2ErrorNetwork;
            }
            return 0;
        });
        http("sceHttp2WaitAsync", [this](Machine& m) -> std::uint32_t {
            const int request = iarg(m, 0);
            if (!arg(m, 1)) return http2ErrorNullPointer;
            std::array<std::int32_t, 4> entry;
            {
                std::lock_guard lock(httpMutex);
                const auto pending = completions.find(request);
                if (pending == completions.end() || pending->second.empty()) return http2ErrorInvalidId;
                entry = pending->second.front();
                pending->second.erase(pending->second.begin());
            }
            const std::int32_t out[6] = {entry[0], entry[1], entry[2], 0, 0, 0};  // event_type, req_id, result, pad, reserved
            return wr(m, arg(m, 1), out, sizeof(out)) ? 0 : http2ErrorNullPointer;
        });
        // Three setter-style exports whose names are not in the upstream list; they only configure objects.
        for (const char* nid : {"YfaXPnc8PJE", "-MU9L6S3Fj4", "D1IAo-CNswA"})
            addNid(Scope::Http2, nid, [](Machine& m) { ret(m, 0); });
    }
};

SceNetImports::SceNetImports(Machine& machine, std::uint64_t gateBase) : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceNetImports::~SceNetImports() = default;
void SceNetImports::SetErrnoAddress(std::function<std::uint64_t()> provider) { impl->errnoAddress = std::move(provider); }
void SceNetImports::SetStopCheck(std::function<bool()> check) { impl->stopCheck = std::move(check); }
void SceNetImports::SetUserEventTrigger(std::function<int(std::uint64_t, std::int32_t, std::uint64_t)> trigger) {
    impl->userEvent = std::move(trigger);
}

std::optional<std::uint64_t> SceNetImports::Resolve(const SceImport& import) {
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) return std::nullopt;
    const auto matches = [&] {
        switch (service->second.scope) {
            case Scope::Net: return import.LibraryName == "libSceNet" && import.ModuleName == "libSceNet" &&
                                    import.ModuleMajor == 1 && import.ModuleMinor == 1;
            case Scope::Ssl: return import.LibraryName == "libSceSsl" && import.ModuleName == "libSceSsl" &&
                                    import.ModuleMajor == 2 && import.ModuleMinor == 1;
            case Scope::Http2: return import.LibraryName == "libSceHttp2" && import.ModuleName == "libSceHttp2" &&
                                      import.ModuleMajor == 1 && import.ModuleMinor == 1;
            case Scope::Posix: return (import.LibraryName == "libScePosix" || import.LibraryName == "libkernel") &&
                                      import.ModuleName == "libkernel" && import.ModuleMajor == 1 && import.ModuleMinor == 1;
        }
        return false;
    }();
    if (!matches || import.LibraryVersion != 1) return std::nullopt;
    const std::string key = import.Nid + "|" + import.LibraryName + ":" + std::to_string(import.LibraryId) + "|" +
                            import.ModuleName + ":" + std::to_string(import.ModuleId);
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl), nid = import.Nid](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE network import runtime has expired");
        context->services.at(nid).handler(guest);
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
