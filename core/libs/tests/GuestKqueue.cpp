#include "SceTypes.hpp"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <thread>

extern "C" {
int APS5_VABI kqueue_nid_postfix();
int APS5_VABI kevent_nid_postfix(int, const KernelEvent*, int, KernelEvent*, int, const KernelTimespec*);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI dup_nid_postfix(int);
int APS5_VABI pipe_nid_postfix(int*);
int APS5_VABI socketpair_nid_postfix(int, int, int, int*);
std::int64_t APS5_VABI send_nid_postfix(int, const void*, std::uint64_t, int);
std::int64_t APS5_VABI recv_nid_postfix(int, void*, std::uint64_t, int);
int* APS5_VABI __error_nid_postfix();
}

static constexpr int GuestEnoent = 2;
static constexpr int GuestEbadf = 9;
static constexpr int GuestEinval = 22;
static constexpr int Unix = 1;
static constexpr int Stream = 1;
static constexpr int FirstSocket = 0x10000000;
static constexpr std::int16_t EvfiltRead = -1;
static constexpr std::int16_t EvfiltWrite = -2;
static constexpr std::int16_t EvfiltProc = -5;
static constexpr std::int16_t EvfiltTimer = -7;
static constexpr std::int16_t EvfiltUser = -11;
static constexpr std::uint16_t EvAdd = 0x0001;
static constexpr std::uint16_t EvDelete = 0x0002;
static constexpr std::uint16_t EvEnable = 0x0004;
static constexpr std::uint16_t EvDisable = 0x0008;
static constexpr std::uint16_t EvOneshot = 0x0010;
static constexpr std::uint16_t EvClear = 0x0020;
static constexpr std::uint16_t EvReceipt = 0x0040;
static constexpr std::uint16_t EvError = 0x4000;
static constexpr std::uint32_t NoteTrigger = 0x01000000;
static constexpr std::uint32_t NoteFfcopy = 0xc0000000;
static constexpr std::uint32_t NoteFfor = 0x80000000;
static constexpr std::uint32_t NoteUseconds = 0x0004;

static void Require(bool value) { if (!value) std::abort(); }

static KernelEvent Change(std::uintptr_t ident, std::int16_t filter, std::uint16_t flags, std::uint32_t fflags = 0, std::intptr_t data = 0, void* udata = nullptr) {
    KernelEvent change{};
    change.ident = ident;
    change.filter = filter;
    change.flags = flags;
    change.fflags = fflags;
    change.data = data;
    change.udata = udata;
    return change;
}

static int Apply(int kq, const KernelEvent& change) {
    return kevent_nid_postfix(kq, &change, 1, nullptr, 0, nullptr);
}

static void RequireFailure(int result, int error) {
    Require(result == -1);
    Require(*__error_nid_postfix() == error);
}

static int Poll(int kq, KernelEvent* events, int count) {
    const KernelTimespec zero{0, 0};
    return kevent_nid_postfix(kq, nullptr, 0, events, count, &zero);
}

static int Wait(int kq, KernelEvent* events, int count) {
    const KernelTimespec second{1, 0};
    return kevent_nid_postfix(kq, nullptr, 0, events, count, &second);
}

template <typename TCall> static bool Throws(TCall call) {
    try {
        call();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

static void CreateAndClose() {
    const int kq = kqueue_nid_postfix();
    Require(kq >= 0 && kq < FirstSocket);
    const int other = kqueue_nid_postfix();
    Require(other >= 0 && other != kq);
    KernelEvent event{};
    Require(Poll(kq, &event, 1) == 0);
    Require(Throws([kq] { dup_nid_postfix(kq); }));
    Require(close_nid_postfix(kq) == 0);
    RequireFailure(Poll(kq, &event, 1), GuestEbadf);
    Require(close_nid_postfix(other) == 0);
    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    RequireFailure(Poll(pipe[0], &event, 1), GuestEbadf);
    Require(close_nid_postfix(pipe[0]) == 0 && close_nid_postfix(pipe[1]) == 0);
}

static void Timers() {
    const int kq = kqueue_nid_postfix();
    int marker = 0;
    KernelEvent events[4]{};
    Require(Apply(kq, Change(1, EvfiltTimer, EvAdd, 0, 10, &marker)) == 0);
    Require(Poll(kq, events, 4) == 0);
    Require(Wait(kq, events, 4) == 1);
    Require(events[0].ident == 1 && events[0].filter == EvfiltTimer && events[0].data >= 1 && events[0].udata == &marker);
    Require((events[0].flags & EvClear) != 0);
    Require(Wait(kq, events, 4) == 1 && events[0].ident == 1);
    Require(Apply(kq, Change(1, EvfiltTimer, EvDelete)) == 0);
    RequireFailure(Apply(kq, Change(1, EvfiltTimer, EvDelete)), GuestEnoent);

    Require(Apply(kq, Change(2, EvfiltTimer, EvAdd | EvOneshot, NoteUseconds, 0)) == 0);
    Require(Wait(kq, events, 4) == 1 && events[0].ident == 2 && events[0].data == 1);
    Require(Poll(kq, events, 4) == 0);
    RequireFailure(Apply(kq, Change(2, EvfiltTimer, EvDelete)), GuestEnoent);

    RequireFailure(Apply(kq, Change(3, EvfiltTimer, EvAdd, 0, -1)), GuestEinval);
    RequireFailure(Apply(kq, Change(3, EvfiltTimer, EvAdd, 0x10, 5)), GuestEinval);
    RequireFailure(Apply(kq, Change(3, EvfiltTimer, EvAdd, NoteUseconds | 0x1, 5)), GuestEinval);
    Require(Apply(kq, Change(4, EvfiltTimer, EvAdd, 0, 100000)) == 0);
    Require(Throws([kq] { Apply(kq, Change(4, EvfiltTimer, EvAdd, 0, 5)); }));
    Require(close_nid_postfix(kq) == 0);
}

static void UserEvents() {
    const int kq = kqueue_nid_postfix();
    int marker = 0;
    KernelEvent events[4]{};
    Require(Apply(kq, Change(5, EvfiltUser, EvAdd | EvClear, 0, 0, &marker)) == 0);
    Require(Poll(kq, events, 4) == 0);
    std::thread trigger([kq, &marker] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(Apply(kq, Change(5, EvfiltUser, 0, NoteTrigger | NoteFfcopy | 0x123, 77, &marker)) == 0);
    });
    Require(kevent_nid_postfix(kq, nullptr, 0, events, 4, nullptr) == 1);
    trigger.join();
    Require(events[0].ident == 5 && events[0].filter == EvfiltUser && events[0].fflags == 0x123 && events[0].data == 77);
    Require(events[0].udata == &marker && events[0].flags == EvClear);
    Require(Poll(kq, events, 4) == 0);
    Require(Apply(kq, Change(5, EvfiltUser, 0, NoteTrigger | NoteFfor | 0x400, 78)) == 0);
    Require(Poll(kq, events, 4) == 1 && events[0].fflags == 0x523 && events[0].data == 78);

    Require(Apply(kq, Change(6, EvfiltUser, EvAdd)) == 0);
    Require(Apply(kq, Change(6, EvfiltUser, 0, NoteTrigger)) == 0);
    for (int i = 0; i < 2; ++i) {
        Require(Poll(kq, events, 4) == 1 && events[0].ident == 6);
    }
    Require(Apply(kq, Change(6, EvfiltUser, EvClear)) == 0);
    Require(Poll(kq, events, 4) == 0);
    Require(Apply(kq, Change(6, EvfiltUser, EvDelete)) == 0);

    KernelEvent receipt[2]{};
    const KernelEvent change = Change(7, EvfiltUser, EvAdd | EvReceipt, 0x55, 9, &marker);
    Require(kevent_nid_postfix(kq, &change, 1, receipt, 2, nullptr) == 1);
    Require(receipt[0].ident == 7 && receipt[0].flags == EvError && receipt[0].data == 0 && receipt[0].fflags == 0);
    Require(Apply(kq, Change(7, EvfiltUser, 0, NoteTrigger)) == 0);
    Require(Poll(kq, events, 4) == 1 && events[0].fflags == 0x55 && events[0].data == 0 && events[0].flags == EvReceipt);
    Require(close_nid_postfix(kq) == 0);
}

static void SocketPair() {
    const int kq = kqueue_nid_postfix();
    int pair[2];
    Require(socketpair_nid_postfix(Unix, Stream, 0, pair) == 0);
    int marker = 0;
    KernelEvent events[4]{};
    Require(Apply(kq, Change(static_cast<std::uintptr_t>(pair[0]), EvfiltRead, EvAdd, 0, 0, &marker)) == 0);
    Require(Poll(kq, events, 4) == 0);
    Require(send_nid_postfix(pair[1], "abc", 3, 0) == 3);
    Require(Wait(kq, events, 4) == 1);
    Require(events[0].ident == static_cast<std::uintptr_t>(pair[0]) && events[0].filter == EvfiltRead && events[0].data == 3 && events[0].udata == &marker);
    Require(Poll(kq, events, 4) == 1);
    char buffer[4]{};
    Require(recv_nid_postfix(pair[0], buffer, sizeof(buffer), 0) == 3);
    Require(Poll(kq, events, 4) == 0);
    Require(Apply(kq, Change(static_cast<std::uintptr_t>(pair[1]), EvfiltWrite, EvAdd | EvOneshot)) == 0);
    Require(Wait(kq, events, 4) == 1 && events[0].filter == EvfiltWrite);
    RequireFailure(Apply(kq, Change(static_cast<std::uintptr_t>(pair[1]), EvfiltWrite, EvDelete)), GuestEnoent);
    Require(Throws([kq, &pair] { Apply(kq, Change(static_cast<std::uintptr_t>(pair[1]), EvfiltRead, EvAdd | EvClear)); }));
    Require(close_nid_postfix(pair[0]) == 0);
    RequireFailure(Apply(kq, Change(static_cast<std::uintptr_t>(pair[0]), EvfiltRead, EvDelete)), GuestEbadf);
    Require(close_nid_postfix(pair[1]) == 0);
    Require(close_nid_postfix(kq) == 0);
}

static void Errors() {
    const int kq = kqueue_nid_postfix();
    KernelEvent events[4]{};
    const KernelEvent changes[3] = {
        Change(FirstSocket + 4000, EvfiltRead, EvAdd),
        Change(8, EvfiltUser, EvDelete),
        Change(9, EvfiltUser, EvAdd | EvEnable | EvDisable),
    };
    Require(kevent_nid_postfix(kq, changes, 3, events, 4, nullptr) == 3);
    Require(events[0].flags == EvError && events[0].data == GuestEbadf && events[0].ident == FirstSocket + 4000);
    Require(events[1].flags == EvError && events[1].data == GuestEnoent);
    Require(events[2].flags == EvError && events[2].data == GuestEinval);
    RequireFailure(kevent_nid_postfix(kq, changes, 3, events, 1, nullptr), GuestEnoent);
    Require(events[0].flags == EvError && events[0].data == GuestEbadf);
    RequireFailure(kevent_nid_postfix(kq, changes, 3, nullptr, 0, nullptr), GuestEbadf);
    RequireFailure(Apply(kq, Change(static_cast<std::uintptr_t>(kq), EvfiltRead, EvAdd)), GuestEinval);
    RequireFailure(Apply(kq, Change(1, 1, EvAdd)), GuestEinval);
    Require(Throws([kq] { Apply(kq, Change(1, EvfiltProc, EvAdd)); }));
    Require(Throws([kq] { Apply(kq, Change(1, EvfiltUser, EvAdd | EvDisable)); }));
    int pipe[2];
    Require(pipe_nid_postfix(pipe) == 0);
    Require(Throws([kq, &pipe] { Apply(kq, Change(static_cast<std::uintptr_t>(pipe[0]), EvfiltRead, EvAdd)); }));
    Require(close_nid_postfix(pipe[0]) == 0 && close_nid_postfix(pipe[1]) == 0);
    const KernelEvent skipped = Change(1, 0, EvAdd);
    const KernelTimespec zero{0, 0};
    Require(kevent_nid_postfix(kq, &skipped, 1, events, 4, &zero) == 0);
    const KernelTimespec invalid{0, 1000000000};
    RequireFailure(kevent_nid_postfix(kq, nullptr, 0, events, 4, &invalid), GuestEinval);
    const KernelTimespec negative{-1, 0};
    RequireFailure(kevent_nid_postfix(kq, nullptr, 0, events, 4, &negative), GuestEinval);
    Require(close_nid_postfix(kq) == 0);
}

static void Timeout() {
    const int kq = kqueue_nid_postfix();
    KernelEvent event{};
    const KernelTimespec timeout{0, 30000000};
    const auto start = std::chrono::steady_clock::now();
    Require(kevent_nid_postfix(kq, nullptr, 0, &event, 1, &timeout) == 0);
    Require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(29));
    Require(kevent_nid_postfix(kq, nullptr, 0, &event, 0, nullptr) == 0);
    Require(close_nid_postfix(kq) == 0);
}

int main() {
    CreateAndClose();
    Timers();
    UserEvents();
    SocketPair();
    Errors();
    Timeout();
    return 0;
}
