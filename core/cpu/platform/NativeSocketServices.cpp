// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded Machine/GuestThreads adaptation of shadPS4 net.cpp, net_translate.cpp,
// net_error.h, host_net.cpp and guest_net.cpp at
// 945dbc3cc3eee80ac3e053b438502ed936fa6bb2. This public engineering profile
// admits only IPv4 TCP/UDP creation, close and network errno; no retail ABI claim.
#include "NativeSocketServices.hpp"
#include "../src/SceImageData.hpp"
#include <cpu/GuestThreads.hpp>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <limits>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <tuple>
#include <unistd.h>

namespace Cpu::Platform {
namespace {
constexpr std::size_t pageSize = 4096;
constexpr std::uint64_t canonicalLimit = 0x800000000000ULL;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr std::array<std::string_view, 3> nids{"Q4qBuN-c0ZM", "45ggEzakPJQ", "HQOwnfMGipQ"};
// Process-wide virtual identities are never recycled, including across providers.
// Exhaustion fails rather than wrapping into an old session's descriptor.
std::atomic<std::uint32_t> nextDescriptor{32};
std::optional<std::int32_t> reserveDescriptor() {
    auto candidate = nextDescriptor.load(std::memory_order_relaxed);
    constexpr auto exhausted = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1u;
    while (candidate < exhausted) {
        if (nextDescriptor.compare_exchange_weak(candidate, candidate + 1,
                std::memory_order_relaxed)) return static_cast<std::int32_t>(candidate);
    }
    return std::nullopt;
}
std::int32_t scalar(std::uint64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}
std::int32_t guestError(int hostError) {
    // Pinned Host::FromNativeError -> ToOrbisErrno translation. Do not depend
    // on host errno numerals even where Darwin and Orbis happen to agree.
    switch (hostError) {
    case EPERM: return 1; case ENOENT: return 2; case EINTR: return 4;
    case EIO: return 5; case EBADF: return 9; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16;
    case EEXIST: return 17; case ENODEV: return 19; case EINVAL: return 22;
    case ENFILE: return 23; case EMFILE: return 24; case ENOSPC: return 28;
    case EPIPE: return 32; case EAGAIN: return 35;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return 35;
#endif
    case EINPROGRESS: return 36; case EALREADY: return 37;
    case ENOTSOCK: return 9; case EDESTADDRREQ: return 39;
    case EMSGSIZE: return 40; case EPROTOTYPE: return 41;
    case ENOPROTOOPT: return 42; case EPROTONOSUPPORT: return 43;
    case ESOCKTNOSUPPORT: return 44; case EOPNOTSUPP: return 45;
#if ENOTSUP != EOPNOTSUPP
    case ENOTSUP: return 45;
#endif
    case EPFNOSUPPORT: return 46; case EAFNOSUPPORT: return 47;
    case EADDRINUSE: return 48; case EADDRNOTAVAIL: return 49;
    case ENETDOWN: return 50; case ENETUNREACH: return 51;
    case ENETRESET: return 52; case ECONNABORTED: return 53;
    case ECONNRESET: return 54; case ENOBUFS: return 55;
    case EISCONN: return 56; case ENOTCONN: return 57;
    case ESHUTDOWN: return 32; case ETOOMANYREFS: return 59;
    case ETIMEDOUT: return 60; case ECONNREFUSED: return 61;
    case ELOOP: return 62; case ENAMETOOLONG: return 63;
    case EHOSTDOWN: return 64; case EHOSTUNREACH: return 65;
    case ENOTEMPTY: return 66; case ECANCELED: return 85;
    case 0: return 0;
    default: return 204;
    }
}
void checkVacant(Machine& machine, std::uint64_t address) {
    if (!address || (address & (pageSize - 1)) || address > canonicalLimit - pageSize)
        throw std::invalid_argument("Native socket invalid guest page");
    for (const auto& mapping : machine.Mappings())
        if (mapping.Address < address + pageSize && address < mapping.Address + mapping.Size)
            throw std::invalid_argument("Native socket guest page already mapped");
}
struct Socket {
    int fd;
    explicit Socket(int descriptor) : fd(descriptor) {}
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
}

struct NativeSocketServices::Impl {
    using Key = std::tuple<unsigned, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    Configuration configuration;
    std::optional<GuestThreads::WaitDomain> ownership;
    std::map<Key, std::uint64_t> gates;
    std::map<std::int32_t, std::unique_ptr<Socket>> sockets;
    std::map<GuestThreadHandle, std::uint64_t> errnoPages;
    std::uint64_t nextErrnoPage = 0;
    bool gateMapped = false;

    Impl(Machine& m, const std::shared_ptr<GuestThreads>& scheduler, Configuration config)
        : machine(m), threads(scheduler), configuration(config) {
        if (!scheduler) throw std::invalid_argument("Native sockets need a guest scheduler");
        scheduler->CheckIdleOwner();
        // Validate address shape without allocating or claiming any guest page.
        for (const auto address : {config.GateBase, config.ErrnoBase})
            if (!address || (address & (pageSize - 1)) || address > canonicalLimit - pageSize)
                throw std::invalid_argument("Native socket invalid configured page");
        if (config.GateBase == config.ErrnoBase)
            throw std::invalid_argument("Native socket configured pages overlap");
    }
    ~Impl() {
        if (ownership) ownership->Withdraw();
        sockets.clear();
        for (const auto& [thread, page] : errnoPages) machine.Unmap(page, pageSize);
        if (gateMapped) machine.Unmap(configuration.GateBase, pageSize);
    }
    void stopped(GuestThreadHandle thread) {
        const auto found = errnoPages.find(thread);
        if (found == errnoPages.end()) return;
        machine.Unmap(found->second, pageSize);
        errnoPages.erase(found);
        // Socket descriptors have session/process ownership and can be used by
        // another live guest thread; stopping their creator does not close them.
    }
    std::uint64_t errnoAddress() {
        const auto scheduler = threads.lock();
        if (!scheduler) throw std::runtime_error("Native socket guest scheduler expired");
        const auto thread = scheduler->ActiveThread();
        if (auto found = errnoPages.find(thread); found != errnoPages.end()) {
            machine.CheckAccess(found->second, sizeof(std::int32_t), rw);
            return found->second;
        }
        if (nextErrnoPage > (canonicalLimit - pageSize - configuration.ErrnoBase) / pageSize)
            throw std::runtime_error("Native socket guest errno address space exhausted");
        const auto address = configuration.ErrnoBase + nextErrnoPage * pageSize;
        checkVacant(machine, address);
        machine.Map(address, pageSize, rw);
        try {
            const std::int32_t initial = 0;
            machine.Write(address, std::as_bytes(std::span(&initial, 1)));
            errnoPages.emplace(thread, address);
        } catch (...) {
            machine.Unmap(address, pageSize);
            throw;
        }
        ++nextErrnoPage;
        return address;
    }
    void fail(std::int32_t error) {
        const auto address = errnoAddress();
        machine.Write(address, std::as_bytes(std::span(&error, 1)));
        machine.Set(Register::Rax, 0x80410100u | static_cast<std::uint32_t>(error));
    }
    bool boundedName(std::uint64_t name) {
        if (!name) return true;
        // At most 32 bytes read: a NUL at index31 is allowed, 32 non-NUL
        // bytes are ENAMETOOLONG. Readonly guest names are valid input.
        for (unsigned index = 0; index < 32; ++index) {
            if (name > std::numeric_limits<std::uint64_t>::max() - index)
                throw std::runtime_error("Native socket guest name span wraps");
            const auto address = name + index;
            machine.CheckAccess(address, 1, Permission::Read);
            std::byte byte{};
            machine.Read(address, std::span(&byte, 1));
            if (byte == std::byte{0}) return true;
        }
        return false;
    }
    void create() {
        errnoAddress();
        const auto family = scalar(machine.Get(Register::Rsi));
        const auto type = scalar(machine.Get(Register::Rdx));
        const auto protocol = scalar(machine.Get(Register::Rcx));
        // This finite candidate admits only ordinary IPv4 TCP/UDP. The public
        // scalar validation precedes the name access, including invalid names.
        if (family != 2 || (type != 1 && type != 2)) { fail(43); return; }
        const auto naturalProtocol = type == 1 ? 6 : 17;
        if (protocol != 0 && protocol != naturalProtocol) { fail(41); return; }
        if (!boundedName(machine.Get(Register::Rdi))) { fail(63); return; }
        // Success preserves the current errno value, including guest writes.
        // The active thread's binding was checked before scalar/name handling.
        const auto id = reserveDescriptor();
        if (!id) { fail(24); return; }
        const auto fd = ::socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM,
                                 protocol == 0 ? 0 : type == 1 ? IPPROTO_TCP : IPPROTO_UDP);
        if (fd < 0) { const auto error = errno; fail(guestError(error)); return; }
        // The stack guard closes the real descriptor if allocating ownership
        // storage fails. Once inserted, exactly this provider owns the FD.
        Socket guard(fd);
        const auto flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            const auto error = errno; fail(guestError(error)); return;
        }
        const auto descriptorFlags = ::fcntl(fd, F_GETFD);
        if (descriptorFlags < 0 || ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
            const auto error = errno; fail(guestError(error)); return;
        }
#ifdef SO_NOSIGPIPE
        // Match the pinned host source's best-effort SIGPIPE suppression.
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        try {
            auto owned = std::make_unique<Socket>(fd);
            guard.fd = -1;
            sockets.emplace(*id, std::move(owned));
        } catch (const std::bad_alloc&) { fail(12); return; }
        machine.Set(Register::Rax, static_cast<std::uint32_t>(*id));
    }
    void close() {
        const auto id = scalar(machine.Get(Register::Rdi));
        const auto found = sockets.find(id);
        if (found == sockets.end()) { fail(9); return; }
        errnoAddress();
        // Remove ownership before close and never retry EINTR: a host close
        // retry could consume a descriptor reused by unrelated code.
        auto owned = std::move(found->second);
        sockets.erase(found);
        const auto fd = owned->fd;
        owned->fd = -1;
        const auto result = ::close(fd);
        if (result < 0) { const auto error = errno; fail(guestError(error)); return; }
        machine.Set(Register::Rax, 0);
    }
    void dispatch(unsigned operation) {
        if (operation == 0) create();
        else if (operation == 1) close();
        else machine.Set(Register::Rax, errnoAddress());
    }
    void mapGatePage() {
        if (gateMapped) return;
        checkVacant(machine, configuration.GateBase);
        std::array<std::byte, pageSize> page;
        page.fill(std::byte{0xcc});
        machine.Map(configuration.GateBase, pageSize, rw);
        try {
            machine.Write(configuration.GateBase, page);
            machine.Protect(configuration.GateBase, pageSize, Permission::Read | Permission::Execute);
        } catch (...) {
            machine.Unmap(configuration.GateBase, pageSize);
            throw;
        }
        gateMapped = true;
    }
    bool admitsRow(const SceParsedImage& consumer, const SceImport& import,
                   std::uint8_t observedType, std::uint64_t observedSize) const {
        // Import metadata supplied by the resolver must correspond to an actual
        // undefined global function in the parser's retained symbol table.
        const auto& parsed = *consumer.Data;
        bool moduleObserved = false;
        for (const auto& module : parsed.ImportedModules)
            if (module.Name == import.ModuleName && module.Id == import.ModuleId &&
                module.Major == import.ModuleMajor && module.Minor == import.ModuleMinor)
                moduleObserved = true;
        if (!moduleObserved) return false;
        for (const auto& symbol : parsed.Symbols) {
            if (!symbol.Import || *symbol.Import >= consumer.Imports.size() ||
                symbol.Type != observedType || symbol.Size != observedSize ||
                symbol.Section != 0 || symbol.Binding != 1 || symbol.Visibility != 0 ||
                symbol.Value != 0)
                continue;
            const auto& observed = consumer.Imports[*symbol.Import];
            if (observed.Nid == import.Nid && observed.LibraryName == import.LibraryName &&
                observed.LibraryId == import.LibraryId && observed.ModuleName == import.ModuleName &&
                observed.ModuleId == import.ModuleId && observed.LibraryVersion == import.LibraryVersion &&
                observed.ModuleMajor == import.ModuleMajor && observed.ModuleMinor == import.ModuleMinor)
                return true;
        }
        return false;
    }
    bool admits(const SceParsedImage& consumer) const {
        const auto& config = configuration;
        if (!config.EnablePublicFixtureCandidate || !config.PublicFixtureSize || !consumer.Data ||
            consumer.Path.filename() != "NativeSocketGuest.elf" || consumer.SourceContainer != "elf" ||
            consumer.Type != 0xfe10)
            return false;
        // Check the immutable parser product, not an arbitrary path/name or the
        // caller-editable top-level source fields alone. ParseSce retains the
        // original source hash and byte count alongside its validated symbols.
        const auto& parsed = *consumer.Data;
        return consumer.SourceSha256 == parsed.SourceSha256 &&
               consumer.SourceSize == parsed.SourceSize &&
               parsed.SourceSha256 == config.PublicFixtureSha256 &&
               parsed.SourceSize == config.PublicFixtureSize;
    }
};

NativeSocketServices::NativeSocketServices(Machine& machine, const std::shared_ptr<GuestThreads>& threads)
    : NativeSocketServices(machine, threads, Configuration{}) {}
NativeSocketServices::NativeSocketServices(Machine& machine, const std::shared_ptr<GuestThreads>& threads,
                                         Configuration configuration)
    : impl(std::make_shared<Impl>(machine, threads, configuration)) {
    impl->ownership.emplace(threads->CreateWaitDomain(machine,
        [weak = std::weak_ptr<Impl>(impl)](GuestThreadHandle thread) {
            if (auto provider = weak.lock()) provider->stopped(thread);
        }));
}
NativeSocketServices::~NativeSocketServices() = default;
std::optional<std::uint64_t> NativeSocketServices::Resolve(const SceImport& import,
        std::uint8_t type, std::uint64_t size, const SceParsedImage& consumer) {
    unsigned operation = 0;
    while (operation < nids.size() && nids[operation] != import.Nid) ++operation;
    if (operation == nids.size() || !impl->admits(consumer) || type != 2 || size != 0 ||
        import.LibraryName != "libSceNet" || import.ModuleName != "libSceNet" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1 ||
        import.LibraryId != 1 || import.ModuleId != 1 || !impl->admitsRow(consumer, import, type, size))
        return std::nullopt;
    const auto scheduler = impl->threads.lock();
    if (!scheduler) throw std::runtime_error("Native socket guest scheduler expired");
    scheduler->CheckIdleOwner();
    const Impl::Key key{operation, import.LibraryId, import.ModuleId};
    if (auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() >= pageSize / 16)
        throw std::runtime_error("Native socket gate page exhausted");
    impl->mapGatePage();
    const auto address = impl->configuration.GateBase + impl->gates.size() * 16;
    constexpr std::array ret{std::byte{0xc3}};
    impl->machine.Write(address, ret);
    impl->gates.emplace(key, address);
    try {
        impl->machine.AddHostCall(address, [weak = std::weak_ptr<Impl>(impl), operation](Machine& m) {
            const auto provider = weak.lock();
            if (!provider || &m != &provider->machine)
                throw std::runtime_error("Native socket provider expired or wrong Machine");
            provider->dispatch(operation);
        });
    } catch (...) { impl->gates.erase(key); throw; }
    return address;
}
}
