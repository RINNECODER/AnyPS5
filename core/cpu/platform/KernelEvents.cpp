#include "KernelEvents.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace Cpu::Platform {
namespace {
// Order matches KernelEventContract.
constexpr std::array<KernelEventImport, 17> inventory{{
    {"D0OdFMjp46I", "sceKernelCreateEqueue"}, {"jpFjmgAC5AE", "sceKernelDeleteEqueue"},
    {"fzyMKs9kim0", "sceKernelWaitEqueue"},
    {"4R6-OvI2cEA", "sceKernelAddUserEvent"}, {"WDszmSbWuDk", "sceKernelAddUserEventEdge"},
    {"F6e0kwo4cnk", "sceKernelTriggerUserEvent"}, {"LJDwdSNTnDg", "sceKernelDeleteUserEvent"},
    {"57ZK+ODEXWY", "sceKernelAddTimerEvent"}, {"YWQFUyXIVdU", "sceKernelDeleteTimerEvent"},
    {"R74tt43xP6k", "sceKernelAddHRTimerEvent"}, {"J+LF6LwObXU", "sceKernelDeleteHRTimerEvent"},
    {"mJ7aghmgvfc", "sceKernelGetEventId"}, {"23CPPI1tyBY", "sceKernelGetEventFilter"},
    {"kwGyyjohI50", "sceKernelGetEventData"}, {"vz+pg2zdopI", "sceKernelGetEventUserData"},
    {"Q0qr9AyqJSk", "sceKernelGetEventFflags"}, {"Uu-iDFC9aUc", "sceKernelGetEventError"}}};
enum Op : unsigned {
    CreateQueue, DeleteQueue, WaitQueue, AddUser, AddUserEdge, TriggerUser, DeleteUser,
    AddTimer, DeleteTimer, AddHrTimer, DeleteHrTimer,
    GetId, GetFilter, GetData, GetUserData, GetFflags, GetError };
constexpr std::uint32_t Enoent = 0x80020002u, Badf = 0x80020009u, Nomem = 0x8002000cu, Fault = 0x8002000eu,
                        Invalid = 0x80020016u, Timedout = 0x8002003cu;
// kqueue filters and flags as used by the public upstream Equeue.cpp.
constexpr std::int16_t FilterTimer = -7, FilterUser = -11, FilterGraphics = -14, FilterHrTimer = -15;
constexpr std::uint16_t EvAdd = 0x0001, EvOneshot = 0x0010, EvClear = 0x0020;
constexpr std::size_t MaxQueues = 1024, MaxSubscriptions = 4096;
// Process-wide nonrecycled numeric identities keep a foreign provider's queue
// and a removed/recreated queue distinct, independently of host allocations.
std::atomic<std::uint64_t> nextQueue{0x0c09000000000001ULL};
std::uint64_t queueIdentity() {
    auto candidate = nextQueue.load();
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (;;) {
        if (!candidate || candidate >= limit) throw std::runtime_error("Kernel event numeric identity exhausted");
        if (nextQueue.compare_exchange_weak(candidate, candidate + 1)) return candidate;
    }
}
using Clock = std::chrono::steady_clock;
void checked(Machine& m, std::uint64_t address, std::uint64_t size, Permission p) {
    if (!address || !size || address > std::numeric_limits<std::uint64_t>::max() - size)
        throw std::runtime_error("Invalid kernel event guest span");
    m.CheckAccess(address, size, p);
}
template<class T> T read(Machine& m, std::uint64_t address) {
    checked(m, address, sizeof(T), Permission::Read);
    T value; m.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
}
template<class T> void write(Machine& m, std::uint64_t address, const T& value) {
    m.Write(address, std::as_bytes(std::span(&value, 1)));
}
void name(Machine& m, std::uint64_t address) {
    for (unsigned i = 0; i < 4096; ++i) {
        if (address > std::numeric_limits<std::uint64_t>::max() - i)
            throw std::runtime_error("Invalid kernel event name span");
        if (read<std::uint8_t>(m, address + i) == 0) return;
    }
    throw std::runtime_error("Kernel event name exceeds bounded source scan");
}
unsigned operation(const SceImport& import) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    return op;
}
}
std::span<const KernelEventImport> KernelEventInventory() { return inventory; }

struct KernelEvents::Impl : std::enable_shared_from_this<KernelEvents::Impl> {
    struct Queue;
    struct Subscription {
        std::weak_ptr<Queue> queue;
        std::int32_t id;
        std::uint64_t udata;
        std::int64_t count = 0; // event data: EOP count, user data or timer expirations
        std::uint64_t reservedKey = 0;
        bool live = true; // producer reads only under mailbox lock
        std::int16_t filter = FilterGraphics;
        std::uint16_t flags = EvAdd | EvClear;
        bool triggered = false; // USER/TIMER/HRTIMER readiness; graphics uses count
        std::optional<Clock::time_point> deadline;
        Clock::duration interval{};
        bool ready() const { return filter == FilterGraphics ? count != 0 : triggered; }
        KernelEventRecord record() const {
            return {static_cast<std::uint64_t>(static_cast<std::int64_t>(id)), filter, flags, 0, count, udata};
        }
    };
    struct Wait {
        GuestThreadHandle thread;
        std::uint64_t key, events, output;
        std::int32_t capacity;
        std::optional<Clock::time_point> deadline;
        std::vector<std::shared_ptr<Subscription>> reserved;
        bool queued = false;
    };
    struct Queue {
        std::uint64_t token;
        bool live = true;
        std::vector<std::shared_ptr<Subscription>> subscriptions;
        std::deque<std::shared_ptr<Wait>> waiting;
    };
    struct Receipt {
        std::shared_ptr<Queue> queue;
        std::shared_ptr<Subscription> subscription;
        std::int64_t count;
    };
    struct Mailbox {
        std::mutex mutex;
        bool live = true, overflow = false;
        std::vector<std::shared_ptr<Subscription>> subscriptions;
        std::vector<Receipt> receipts;
        Mailbox() { subscriptions.reserve(MaxSubscriptions); receipts.reserve(MaxSubscriptions); }
    };
    using Key = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    std::optional<GuestThreads::WaitDomain> waits;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::map<std::uint64_t, std::shared_ptr<Queue>> queues;
    std::map<Key, std::uint64_t> gates;
    std::uint64_t base, nextWait = 1;
    bool live = true;
    Impl(Machine& m, const std::shared_ptr<GuestThreads>& t, std::uint64_t b)
        : machine(m), threads(t), base(b) {
        if (!t) throw std::invalid_argument("Kernel events require a persistent guest scheduler");
        t->CheckIdleOwner();
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("Invalid kernel event gate page");
        for (const auto& mapping : m.Mappings())
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("Kernel event gate page already mapped");
        std::array<std::byte, 4096> bytes; bytes.fill(std::byte{0xcc});
        m.Map(base, bytes.size(), Permission::Read | Permission::Write);
        try { m.Write(base, bytes); m.Protect(base, bytes.size(), Permission::Read | Permission::Execute); }
        catch (...) { m.Unmap(base, bytes.size()); throw; }
    }
    ~Impl() { machine.Unmap(base, 4096); }
    std::shared_ptr<GuestThreads> scheduler() const {
        auto t = threads.lock();
        if (!t) throw std::runtime_error("Kernel event scheduler expired");
        return t;
    }
    void owner(bool idle) const {
        auto t = scheduler();
        if (idle) t->CheckIdleOwner();
        else (void)t->ActiveThread(); // also checks the persistent owner
        if (!live) throw std::runtime_error("Kernel event provider was shut down");
    }
    void stopped(GuestThreadHandle id) {
        for (auto& [handle, q] : queues) {
            for (const auto& w : q->waiting) if (w->thread == id)
                for (auto& s : w->reserved) if (s->reservedKey == w->key) s->reservedKey = 0;
            std::erase_if(q->waiting, [&](const auto& w) { return w->thread == id; });
        }
    }
    std::vector<std::shared_ptr<Subscription>> available(const Queue& q, std::int32_t capacity) {
        std::vector<std::shared_ptr<Subscription>> result;
        for (const auto& s : q.subscriptions) {
            if (s->live && s->ready() && !s->reservedKey) result.push_back(s);
            if (result.size() == static_cast<std::size_t>(capacity)) break;
        }
        return result;
    }
    // Arms TIMER/HRTIMER subscriptions whose deadline passed. A periodic timer
    // counts every whole interval that elapsed in its event data.
    static void expire(Queue& q, Clock::time_point now) {
        for (const auto& s : q.subscriptions) {
            if (!s->live || !s->deadline || now < *s->deadline) continue;
            if (s->filter == FilterTimer && s->interval > Clock::duration::zero()) {
                const auto periods = 1 + (now - *s->deadline) / s->interval;
                s->count = periods > std::numeric_limits<std::int64_t>::max() - s->count
                    ? std::numeric_limits<std::int64_t>::max() : s->count + periods;
                *s->deadline += periods * s->interval;
            } else if (s->filter == FilterTimer) {
                // A zero period stays periodic: it is due again on every check.
                if (s->count < std::numeric_limits<std::int64_t>::max()) ++s->count;
                s->deadline = now;
            } else s->deadline.reset(); // HRTIMER is one shot
            s->triggered = true;
        }
    }
    // Applies EV_CLEAR / EV_ONESHOT after an event was reported to the guest.
    void consume(Queue& q, const std::shared_ptr<Subscription>& s) {
        s->reservedKey = 0;
        if (s->filter == FilterGraphics) { s->count = 0; return; }
        if (s->flags & EvOneshot) {
            s->live = false;
            std::erase(q.subscriptions, s);
            return;
        }
        if (s->flags & EvClear) { s->triggered = false; s->count = 0; }
    }
    void outputs(std::uint64_t events, std::int32_t capacity, std::uint64_t out) {
        checked(machine, events, static_cast<std::uint64_t>(capacity) * sizeof(KernelEventRecord), Permission::Write);
        checked(machine, out, sizeof(std::int32_t), Permission::Write);
    }
    std::uint32_t deliver(const std::shared_ptr<Queue>& q, const std::shared_ptr<Wait>& w) {
        owner(true);
        if (!q->live) {
            checked(machine, w->output, sizeof(std::int32_t), Permission::Write);
            write(machine, w->output, static_cast<std::int32_t>(Badf));
            std::erase(q->waiting, w);
            return Badf; // exact selected source Close/WaitEqueue result
        }
        std::vector<KernelEventRecord> records;
        for (const auto& s : w->reserved) {
            if (!s->live || (w->queued && s->reservedKey != w->key) || !s->ready())
                throw std::runtime_error("Deleted or invalid kernel event reservation");
            records.push_back(s->record());
        }
        // Revalidate the entire original capacity and count before any write.
        // For parked calls the scheduler already validated the return token.
        outputs(w->events, w->capacity, w->output);
        if (!records.empty()) machine.Write(w->events, std::as_bytes(std::span(records)));
        const auto count = static_cast<std::int32_t>(records.size());
        write(machine, w->output, count);
        for (const auto& s : w->reserved) consume(*q, s);
        std::erase(q->waiting, w);
        return count ? 0 : Timedout;
    }
    std::shared_ptr<Subscription> find(const Queue& q, std::int16_t filter, std::int32_t id) const {
        for (const auto& s : q.subscriptions) if (s->live && s->filter == filter && s->id == id) return s;
        return {};
    }
    std::uint32_t add(Queue& q, const std::shared_ptr<Queue>& owner, std::int16_t filter, std::int32_t id,
                      std::uint16_t flags, std::uint64_t udata, std::optional<Clock::time_point> deadline,
                      Clock::duration interval) {
        if (const auto s = find(q, filter, id)) {
            // EV_ADD on an existing user event keeps its state.
            if (filter == FilterUser) return 0;
            // Re-adding a timer replaces it. A wake already reserved on the old
            // subscription is retracted and reselected by pump().
            s->live = false;
            std::erase(q.subscriptions, s);
        }
        if (q.subscriptions.size() >= MaxSubscriptions) return Nomem;
        auto s = std::make_shared<Subscription>();
        s->queue = owner; s->id = id; s->udata = udata; s->filter = filter; s->flags = flags;
        s->deadline = deadline; s->interval = interval;
        q.subscriptions.push_back(std::move(s));
        return 0;
    }
    std::uint32_t remove(Queue& q, std::int16_t filter, std::int32_t id) {
        const auto s = find(q, filter, id);
        if (!s) return Enoent;
        // A wake already reserved on it is retracted and reselected by pump().
        s->live = false;
        std::erase(q.subscriptions, s);
        return 0;
    }
    std::uint64_t accessor(unsigned op, std::uint64_t address) {
        KernelEventRecord event{};
        checked(machine, address, sizeof(event), Permission::Read);
        machine.Read(address, std::as_writable_bytes(std::span(&event, 1)));
        switch (op) {
        case GetId: return event.Ident;
        case GetFilter: return static_cast<std::uint32_t>(static_cast<std::int32_t>(event.Filter));
        case GetData: return static_cast<std::uint64_t>(event.Data);
        case GetUserData: return event.Udata;
        case GetFflags: return event.Fflags;
        default: return (event.Flags & 0x4000) ? static_cast<std::uint32_t>(event.Data) : 0; // EV_ERROR
        }
    }
    std::uint64_t invoke(unsigned op, Machine& m) {
        owner(false);
        const auto first = m.Get(Register::Rdi);
        if (op >= GetId) return accessor(op, first);
        if (op == CreateQueue) {
            const auto label = m.Get(Register::Rsi);
            if (!first || !label) return Invalid;
            checked(m, first, 8, Permission::Write); name(m, label);
            if (queues.size() >= MaxQueues) throw std::runtime_error("Kernel event owned queue bound exhausted");
            const auto token = queueIdentity();
            auto q = std::make_shared<Queue>(); q->token = token;
            queues.emplace(token, q);
            try { write(m, first, token); } catch (...) { queues.erase(token); throw; }
            return 0;
        }
        const auto found = queues.find(first);
        if (found == queues.end()) return Badf;
        const auto q = found->second;
        if (op == DeleteQueue) {
            {
                std::lock_guard lock(mailbox->mutex);
                q->live = false;
                for (auto& s : q->subscriptions) s->live = false;
                std::erase_if(mailbox->subscriptions, [&](const auto& s) { return s->queue.lock() == q; });
                std::erase_if(mailbox->receipts, [&](const auto& r) { return r.queue == q; });
            }
            // Source Close returns EBADF to blocked WaitForEvents; the wrapper
            // publishes that signed32 count then returns the same error. Already
            // queued completions observe live=false before publishing records.
            const auto pending = q->waiting;
            for (const auto& w : pending) if (!w->queued) {
                w->queued = true;
                if (!waits->Wake(w->thread, w->key, [state = shared_from_this(), q, w] { return state->deliver(q, w); }))
                    std::erase(q->waiting, w);
            }
            queues.erase(first);
            return 0;
        }
        const auto id = static_cast<std::int32_t>(m.Get(Register::Rsi));
        switch (op) {
        case AddUser: return add(*q, q, FilterUser, id, EvAdd, 0, {}, {});
        case AddUserEdge: return add(*q, q, FilterUser, id, EvAdd | EvClear, 0, {}, {});
        case TriggerUser: {
            const auto s = find(*q, FilterUser, id);
            if (!s) return Enoent;
            // The trigger argument is reported both as data and user data.
            const auto data = m.Get(Register::Rdx);
            s->triggered = true; s->udata = data; s->count = static_cast<std::int64_t>(data);
            return 0;
        }
        case DeleteUser: return remove(*q, FilterUser, id);
        case AddTimer: {
            const auto interval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::microseconds(static_cast<std::uint32_t>(m.Get(Register::Rdx))));
            return add(*q, q, FilterTimer, id, EvAdd | EvClear, m.Get(Register::Rcx), Clock::now() + interval, interval);
        }
        case DeleteTimer: return remove(*q, FilterTimer, id);
        case AddHrTimer: {
            const auto spec = m.Get(Register::Rdx);
            if (!spec) return Fault;
            std::array<std::int64_t, 2> time{};
            checked(m, spec, sizeof(time), Permission::Read);
            m.Read(spec, std::as_writable_bytes(std::span(time)));
            if (time[0] < 0 || time[1] < 0 || time[1] >= 1000000000) return Invalid;
            // Saturate far deadlines instead of overflowing the steady clock.
            const auto limit = std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max()).count() / 4;
            const auto delay = std::chrono::duration_cast<Clock::duration>(
                std::chrono::seconds(std::min(time[0], limit)) + std::chrono::nanoseconds(time[1]));
            return add(*q, q, FilterHrTimer, id, EvAdd | EvOneshot, m.Get(Register::Rcx), Clock::now() + delay, {});
        }
        case DeleteHrTimer: return remove(*q, FilterHrTimer, id);
        default: break;
        }
        const auto events = m.Get(Register::Rsi), out = m.Get(Register::Rcx), timeout = m.Get(Register::R8);
        const auto capacity = static_cast<std::int32_t>(m.Get(Register::Rdx));
        if (!events) return Fault;
        if (capacity < 1 || !out) return Invalid;
        outputs(events, capacity, out);
        std::optional<std::uint32_t> micros;
        if (timeout) micros = read<std::uint32_t>(m, timeout);
        auto w = std::make_shared<Wait>();
        w->thread = waits->ActiveThread(); w->events = events; w->output = out; w->capacity = capacity;
        expire(*q, Clock::now());
        w->reserved = available(*q, capacity);
        if (!w->reserved.empty()) {
            // Host calls already own Machine execution; deliver's idle check is
            // intentionally reserved for deferred continuations, so write here.
            std::vector<KernelEventRecord> records;
            for (const auto& s : w->reserved) records.push_back(s->record());
            m.Write(events, std::as_bytes(std::span(records)));
            write(m, out, static_cast<std::int32_t>(records.size()));
            for (const auto& s : w->reserved) consume(*q, s);
            return 0;
        }
        if (micros && *micros == 0) { write(m, out, std::int32_t{0}); return Timedout; }
        if (!w->thread) throw std::runtime_error("Kernel event wait requires an active guest thread");
        if (nextWait == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Kernel event wait identity exhausted");
        w->key = nextWait++;
        if (micros) w->deadline = Clock::now() + std::chrono::microseconds(*micros);
        q->waiting.push_back(w);
        try { waits->BlockFromHostCall(w->key); } catch (...) { q->waiting.pop_back(); throw; }
        return 0; // scheduler alone completes the parked import
    }
    bool pump() {
        owner(true);
        auto state = shared_from_this();
        std::vector<Impl::Receipt> receipts;
        {
            std::lock_guard lock(state->mailbox->mutex);
            if (state->mailbox->overflow) throw std::runtime_error("Kernel event producer counter overflow");
            receipts.swap(state->mailbox->receipts);
        }
        for (const auto& r : receipts) {
            if (!r.queue->live || !r.subscription->live) continue;
            auto& count = r.subscription->count;
            if (count > std::numeric_limits<std::int64_t>::max() - r.count)
                throw std::runtime_error("Kernel event pending counter overflow");
            count += r.count;
        }
        bool external = false;
        const auto now = Clock::now();
        for (const auto& [handle, q] : state->queues) {
            expire(*q, now);
            const auto waiting = q->waiting;
            for (const auto& w : waiting) {
                if (w->queued) {
                    if (std::none_of(w->reserved.begin(), w->reserved.end(), [](const auto& s) { return !s->live; }))
                        continue;
                    // DeleteEvent removes only its subscription. Retract the
                    // exact not-yet-delivered wake, retain the suspended call,
                    // then reselect live pending events or its original timeout.
                    const bool retracted = state->waits->RetractWake(w->thread, w->key);
                    for (auto& s : w->reserved) if (s->reservedKey == w->key) s->reservedKey = 0;
                    w->reserved.clear();
                    w->queued = false;
                    if (!retracted) { state->stopped(w->thread); continue; }
                }
                if (!state->waits->IsWaiting(w->thread, w->key)) { state->stopped(w->thread); continue; }
                external = true;
                w->reserved = state->available(*q, w->capacity);
                const bool expired = w->deadline && Clock::now() >= *w->deadline;
                if (w->reserved.empty() && !expired) continue;
                for (auto& s : w->reserved) s->reservedKey = w->key;
                w->queued = true;
                if (!state->waits->Wake(w->thread, w->key, [state, q, w] { return state->deliver(q, w); })) {
                    w->queued = false;
                    for (auto& s : w->reserved) s->reservedKey = 0;
                    state->stopped(w->thread);
                }
            }
        }
        return external;
    }
};

KernelEvents::KernelEvents(Machine& m, const std::shared_ptr<GuestThreads>& threads, std::uint64_t base)
    : impl(std::make_shared<Impl>(m, threads, base)) {
    impl->waits.emplace(threads->CreateWaitDomain(m, [weak = std::weak_ptr<Impl>(impl)](GuestThreadHandle id) {
        if (const auto state = weak.lock()) state->stopped(id);
    }));
    impl->waits->SetOwnerPump([weak = std::weak_ptr<Impl>(impl)] {
        const auto state = weak.lock();
        return state && state->live ? state->pump() : false;
    });
}
KernelEvents::~KernelEvents() { Shutdown(); }
std::optional<std::uint64_t> KernelEvents::Resolve(const SceImport& import, std::uint8_t type, std::uint64_t size) {
    impl->owner(true);
    const auto op = operation(import);
    if (op == inventory.size()) return std::nullopt;
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1 || type != 2 || size != 0)
        throw std::runtime_error("Unsupported kernel event scope/version/type/size: " + import.Nid);
    const Impl::Key key{import.Nid, import.LibraryId, import.ModuleId};
    if (const auto it = impl->gates.find(key); it != impl->gates.end()) return it->second;
    if (impl->gates.size() >= 256) throw std::runtime_error("Kernel event gate page exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    constexpr std::array ret{std::byte{0xc3}}; impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [weak = std::weak_ptr<Impl>(impl), op](Machine& m) {
        auto state = weak.lock(); if (!state) throw std::runtime_error("Kernel event provider expired");
        m.Set(Register::Rax, state->invoke(op, m));
    });
    impl->gates.emplace(key, gate); return gate;
}
std::int32_t KernelEvents::AddGraphicsEvent(std::uint64_t handle, std::int32_t id, std::uint64_t udata) {
    impl->owner(false);
    const auto found = impl->queues.find(handle);
    if (found == impl->queues.end()) throw std::runtime_error("AGC graphics subscription has invalid event queue");
    const auto q = found->second;
    std::lock_guard lock(impl->mailbox->mutex);
    for (const auto& s : q->subscriptions) if (s->filter == FilterGraphics && s->id == id) { s->udata = udata; return 0; }
    if (impl->mailbox->subscriptions.size() >= MaxSubscriptions)
        throw std::runtime_error("Kernel event owned subscription bound exhausted");
    auto s = std::make_shared<Impl::Subscription>(); s->queue = q; s->id = id; s->udata = udata;
    q->subscriptions.push_back(s); impl->mailbox->subscriptions.push_back(std::move(s)); return 0;
}
std::int32_t KernelEvents::DeleteGraphicsEvent(std::uint64_t handle, std::int32_t id) {
    impl->owner(false);
    const auto found = impl->queues.find(handle);
    if (found == impl->queues.end()) throw std::runtime_error("AGC graphics deletion has invalid event queue");
    const auto q = found->second;
    const auto it = std::find_if(q->subscriptions.begin(), q->subscriptions.end(), [&](const auto& s) { return s->filter == FilterGraphics && s->id == id; });
    if (it == q->subscriptions.end()) throw std::runtime_error("AGC graphics event is not registered");
    const auto s = *it;
    {
        std::lock_guard lock(impl->mailbox->mutex);
        s->live = false;
        std::erase(impl->mailbox->subscriptions, s);
        std::erase_if(impl->mailbox->receipts, [&](const auto& r) { return r.subscription == s; });
    }
    q->subscriptions.erase(it);
    return 0;
}
std::function<void(std::uint32_t)> KernelEvents::EopPublisher() const {
    const auto box = impl->mailbox;
    return [box](std::uint32_t queueId) {
        std::lock_guard lock(box->mutex);
        if (!box->live) return;
        for (const auto& s : box->subscriptions) {
            if (!s->live || static_cast<std::uint32_t>(s->id) != queueId) continue;
            const auto q = s->queue.lock(); if (!q || !q->live) continue;
            const auto found = std::find_if(box->receipts.begin(), box->receipts.end(), [&](const auto& r) { return r.subscription == s; });
            if (found != box->receipts.end()) {
                if (found->count == std::numeric_limits<std::int64_t>::max()) box->overflow = true;
                else ++found->count;
            } else {
                if (box->receipts.size() >= MaxSubscriptions) { box->overflow = true; return; }
                box->receipts.push_back({q, s, 1});
            }
        }
    };
}
void KernelEvents::Shutdown() {
    if (!impl || !impl->live) return;
    // Machine's persistent execution owner survives scheduler withdrawal. This
    // keeps teardown safe after GuestThreads::Withdraw without a new owner.
    auto idle = impl->machine.CaptureContext();
    // Withdraw validates scheduler lifecycle before mutating the mailbox. An
    // owner hook inside an active drive may request stop, but cannot destroy its
    // own domain; the executor must return before this shutdown boundary.
    impl->waits->Withdraw();
    {
        std::lock_guard lock(impl->mailbox->mutex);
        impl->mailbox->live = false;
        for (auto& [handle, q] : impl->queues) {
            q->live = false; for (auto& s : q->subscriptions) s->live = false;
        }
        impl->mailbox->subscriptions.clear(); impl->mailbox->receipts.clear();
    }
    impl->queues.clear(); impl->live = false;
}

std::span<const KernelEventAdmission> TargetKernelEventAdmissions() {
    // Title-agnostic: each contract is the public source behaviour, validated
    // per import by NID, scope, type and size.
    static constexpr std::array<KernelEventAdmission, 17> admissions{{
        {KernelEventContract::CreateEqueue, "CPU09: public AnyPS5 00901fba CreateEqueue + exact eboot import339/caller0x96645a"},
        {KernelEventContract::DeleteEqueue, "CPU09: public AnyPS5 00901fba DeleteEqueue + exact eboot import374/caller0x966753"},
        {KernelEventContract::WaitEqueue, "CPU09: public AnyPS5 00901fba WaitEqueue + exact eboot import345/callers0x8f728e,0x8fdaa8"},
        {KernelEventContract::AddUserEvent, "#279: public upstream Equeue.cpp EVFILT_USER level"},
        {KernelEventContract::AddUserEventEdge, "#279: public upstream Equeue.cpp EVFILT_USER EV_CLEAR"},
        {KernelEventContract::TriggerUserEvent, "#279: public upstream Equeue.cpp TriggerUserEvent data+udata"},
        {KernelEventContract::DeleteUserEvent, "#279: public upstream Equeue.cpp DeleteUserEvent ENOENT"},
        {KernelEventContract::AddTimerEvent, "#279: public upstream Equeue.cpp EVFILT_TIMER periodic EV_CLEAR"},
        {KernelEventContract::DeleteTimerEvent, "#279: public upstream Equeue.cpp DeleteTimerEvent ENOENT"},
        {KernelEventContract::AddHRTimerEvent, "#279: public upstream Equeue.cpp EVFILT_HRTIMER EV_ONESHOT"},
        {KernelEventContract::DeleteHRTimerEvent, "#279: public upstream Equeue.cpp DeleteHRTimerEvent ENOENT"},
        {KernelEventContract::GetEventId, "#279: public upstream EventAccessors.cpp ident"},
        {KernelEventContract::GetEventFilter, "#279: public upstream EventAccessors.cpp filter"},
        {KernelEventContract::GetEventData, "#279: public upstream EventAccessors.cpp data"},
        {KernelEventContract::GetEventUserData, "#279: public upstream EventAccessors.cpp udata"},
        {KernelEventContract::GetEventFflags, "#279: public upstream EventAccessors.cpp fflags"},
        {KernelEventContract::GetEventError, "#279: EV_ERROR data, else 0"}}};
    return admissions;
}
TargetKernelEvents::TargetKernelEvents(Machine& m, const std::shared_ptr<GuestThreads>& threads,
                                     std::span<const KernelEventAdmission> selected)
    : provider(m, threads), admissions(selected.begin(), selected.end()) {}
std::optional<std::uint64_t> TargetKernelEvents::Resolve(const SceImport& import, std::uint8_t type,
                                                       std::uint64_t size) {
    const auto op = operation(import);
    if (op == inventory.size()) return std::nullopt;
    const auto contract = static_cast<KernelEventContract>(op);
    if (std::none_of(admissions.begin(), admissions.end(), [&](const auto& a) { return a.Contract == contract && !a.Evidence.empty(); }))
        throw std::runtime_error("Target kernel event contract was not selected");
    return provider.Resolve(import, type, size);
}
}
