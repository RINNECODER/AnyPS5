/* Public synthetic guest for libkernel sync (#279): semaphore FIFO/PRIO attrs,
   Poll, Cancel and timed waits; event flags; equeue USER, TIMER and HRTIMER
   filters with the GetEvent* accessors. Real linked PLT imports, TLS and guest
   threads. Every expectation is recorded in the receipt; the host harness
   asserts the receipt, the failure counter and the first failing slot. */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef long long s64;
#define EXPORT __attribute__((visibility("protected")))
struct Event { u64 ident; short filter; unsigned short flags; u32 fflags; s64 data; u64 udata; };
struct Timespec { s64 sec, nsec; };
extern int sema_create(u64*, const char*, u32, int, int, const void*);
extern int sema_delete(u64);
extern int sema_wait(u64, int, u32*);
extern int sema_signal(u64, int);
extern int sema_poll(u64, int);
extern int sema_cancel(u64, int, int*);
extern int evf_create(u64*, const char*, u32, u64, const void*);
extern int evf_delete(u64);
extern int evf_set(u64, u64);
extern int evf_clear(u64, u64);
extern int evf_wait(u64, u64, u32, u64*, u32*);
extern int evf_poll(u64, u64, u32, u64*);
extern int evf_cancel(u64, u64, int*);
extern int queue_create(u64*, const char*);
extern int queue_delete(u64);
extern int queue_wait(u64, struct Event*, int, int*, u32*);
extern int user_add(u64, int);
extern int user_add_edge(u64, int);
extern int user_trigger(u64, int, void*);
extern int user_delete(u64, int);
extern int timer_add(u64, int, u32, void*);
extern int timer_delete(u64, int);
extern int hrtimer_add(u64, int, const struct Timespec*, void*);
extern int hrtimer_delete(u64, int);
extern u64 event_id(const struct Event*);
extern int event_filter(const struct Event*);
extern s64 event_data(const struct Event*);
extern u64 event_udata(const struct Event*);
extern u64 event_fflags(const struct Event*);
extern int thread_create(u64*, const u64*, void*(*)(void*), void*, const char*);
extern void thread_yield(void);
extern int thread_join(u64, void**);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
#define FUNC(n) ".type " #n ",@function\n"
__asm__(FUNC(sema_create) FUNC(sema_delete) FUNC(sema_wait) FUNC(sema_signal) FUNC(sema_poll)
 FUNC(sema_cancel) FUNC(evf_create) FUNC(evf_delete) FUNC(evf_set) FUNC(evf_clear) FUNC(evf_wait)
 FUNC(evf_poll) FUNC(evf_cancel) FUNC(queue_create) FUNC(queue_delete) FUNC(queue_wait)
 FUNC(user_add) FUNC(user_add_edge) FUNC(user_trigger) FUNC(user_delete) FUNC(timer_add)
 FUNC(timer_delete) FUNC(hrtimer_add) FUNC(hrtimer_delete) FUNC(event_id) FUNC(event_filter)
 FUNC(event_data) FUNC(event_udata) FUNC(event_fflags) FUNC(thread_create) FUNC(thread_yield)
 FUNC(thread_join) FUNC(thread_self) FUNC(process_exit));

#define EPERM 0x80020001u
#define ESRCH 0x80020003u
#define ENOENT 0x80020002u
#define EBADF 0x80020009u
#define EACCES 0x8002000du
#define EFAULT 0x8002000eu
#define EBUSY 0x80020010u
#define EINVAL 0x80020016u
#define ETIMEDOUT 0x8002003cu
#define ECANCELED 0x80020055u
#define AND 0x01u
#define OR 0x02u
#define CLEAR_ALL 0x10u
#define CLEAR_PAT 0x20u

EXPORT volatile u64 KernelSyncReceipt[128];
#define R KernelSyncReceipt
static const char label[] = "public-sync-fixture";
static const char* volatile relocatedLabel = label;
static __thread u64 tlsInitial __attribute__((used, aligned(16))) = 0x1020304050607080ULL;
static __thread u64 tlsZero[2] __attribute__((used));

/* R[3] keeps the first failing slot, R[4] counts failures. */
static void expect(unsigned slot, u64 value, u64 want) {
 R[slot] = value;
 if (value != want) { if (!R[3]) R[3] = slot; ++R[4]; }
}
static void check(unsigned slot, int ok) { if (!ok) { if (!R[3]) R[3] = slot; ++R[4]; } }
static void settle(void) { for (int i = 0; i < 16; ++i) thread_yield(); }
static void await(volatile u64* flag) { while (!*flag) thread_yield(); settle(); }
static u64 spawn(void* (*entry)(void*), void* argument) {
 u64 id = 0; check(5, thread_create(&id, 0, entry, argument, relocatedLabel) == 0); return id;
}
static void join(u64 id) { void* ignored; check(6, thread_join(id, &ignored) == 0); }

/* ---- semaphores ---- */
static u64 sema;
static void* sema_timed(void* unused) {
 (void)unused; R[30] = 1; u32 t = 30000000; R[31] = (u32)sema_wait(sema, 1, &t); R[32] = t; R[33] = 1; return 0;
}
static void* sema_cancelled(void* need) {
 const u64 n = (u64)need; R[38 + n] = 1; R[40 + n] = (u32)sema_wait(sema, (int)n, 0); R[42 + n] = 1; return 0;
}
static void* sema_fifo(void* unused) {
 (void)unused; R[52] = 1; R[53] = (u32)sema_wait(sema, 2, 0); R[54] = 1; return 0;
}
static void* sema_expiring(void* unused) {
 (void)unused; R[60] = 1; u32 t = 20000; R[61] = (u32)sema_wait(sema, 1, &t); R[62] = 1; return 0;
}
static void semaphores(void) {
 u64 prio = 0, invalid = 0x77;
 expect(10, (u32)sema_create(&sema, relocatedLabel, 1, 0, 4, 0), 0);
 expect(11, (u32)sema_create(&prio, relocatedLabel, 2, 0, 4, 0), 0);
 expect(12, (u32)sema_create(&invalid, relocatedLabel, 3, 0, 4, 0), EINVAL);
 expect(13, (u32)sema_poll(sema, 1), EBUSY);
 expect(14, (u32)sema_signal(sema, 2), 0);
 expect(15, (u32)sema_poll(sema, 1), 0);
 expect(16, (u32)sema_poll(sema, 2), EBUSY);
 expect(17, (u32)sema_poll(sema, 0), EINVAL);
 expect(18, (u32)sema_poll(sema, 5), EINVAL);
 expect(19, (u32)sema_poll(sema, 1), 0);
 u32 t = 2000;
 expect(20, (u32)sema_wait(sema, 1, &t), ETIMEDOUT); expect(21, t, 0);
 t = 0; expect(22, (u32)sema_wait(sema, 1, &t), ETIMEDOUT);
 check(23, sema_signal(sema, 1) == 0);
 t = 5000; expect(24, (u32)sema_wait(sema, 1, &t), 0); R[25] = t; check(25, t > 0 && t <= 5000);
 /* A peer Signal wakes a timed waiter before its deadline. */
 u64 a = spawn(sema_timed, 0); await(&R[30]);
 expect(34, R[33], 0); expect(35, (u32)sema_signal(sema, 1), 0); join(a);
 expect(31, R[31], 0); check(32, R[32] > 0 && R[32] < 30000000);
 /* Cancel wakes both blocked waiters, reports them and resets the count. */
 u64 b = spawn(sema_cancelled, (void*)1), c = spawn(sema_cancelled, (void*)2);
 await(&R[39]); await(&R[40]);
 int n = -1; expect(46, (u32)sema_cancel(sema, 3, &n), 0); expect(47, (u32)n, 2);
 join(b); join(c);
 expect(41, R[41], ECANCELED); expect(42, R[42], ECANCELED);
 expect(48, (u32)sema_poll(sema, 3), 0);
 expect(49, (u32)sema_cancel(sema, 5, &n), EINVAL);
 check(58, sema_signal(sema, 2) == 0);
 expect(50, (u32)sema_cancel(sema, -1, 0), 0);
 expect(51, (u32)sema_poll(sema, 1), EBUSY);
 /* FIFO: a newcomer cannot take tokens ahead of a queued waiter. */
 u64 d = spawn(sema_fifo, 0); await(&R[52]);
 check(59, sema_signal(sema, 1) == 0); settle();
 expect(55, (u32)sema_poll(sema, 1), EBUSY); expect(56, R[54], 0);
 check(68, sema_signal(sema, 1) == 0); join(d);
 expect(53, R[53], 0); expect(57, (u32)sema_poll(sema, 1), EBUSY);
 /* A timed wait does not stall the other guest thread. */
 u64 e = spawn(sema_expiring, 0); await(&R[60]);
 while (!R[62]) { ++R[63]; thread_yield(); }
 join(e); expect(61, R[61], ETIMEDOUT); check(63, R[63] > 0);
 expect(64, (u32)sema_wait(sema, 1, (u32*)1), EFAULT);
 expect(65, (u32)sema_delete(sema), 0); expect(66, (u32)sema_delete(prio), 0);
 expect(67, (u32)sema_poll(sema, 1), ESRCH);
}

/* ---- event flags ---- */
static u64 flag, single;
static void* evf_and(void* unused) {
 (void)unused; R[20] = 1; u64 r = 0; R[21] = (u32)evf_wait(flag, 0x6, AND | CLEAR_PAT, &r, 0); R[22] = r; R[23] = 1; return 0;
}
static void* evf_or(void* slot) {
 const u64 s = (u64)slot; R[s] = 1; u64 r = 0; R[s + 1] = (u32)evf_wait(flag, 0x10, OR, &r, 0); R[s + 2] = r; return 0;
}
static void* evf_clear_all(void* unused) {
 (void)unused; R[40] = 1; u64 r = 0; R[41] = (u32)evf_wait(flag, 0x20, OR | CLEAR_ALL, &r, 0); R[42] = r; return 0;
}
static void* evf_cancelled(void* unused) {
 (void)unused; R[50] = 1; u64 r = 0; R[51] = (u32)evf_wait(flag, 0x100, OR, &r, 0); R[52] = r; return 0;
}
static void* evf_single(void* unused) {
 (void)unused; R[56] = 1; R[57] = (u32)evf_wait(single, 0x1, OR, 0, 0); return 0;
}
static void* evf_expiring(void* unused) {
 (void)unused; R[66] = 1; u32 t = 20000; R[67] = (u32)evf_wait(flag, 0x4000, OR, 0, &t); R[68] = 1; return 0;
}
static void eventflags(void) {
 u64 r = 0, invalid = 0x77;
 expect(10, (u32)evf_create(&invalid, relocatedLabel, 0x30, 0, 0), EINVAL);
 expect(11, (u32)evf_create(&invalid, 0, 0, 0, 0), EINVAL);
 expect(12, (u32)evf_create(&flag, relocatedLabel, 0x21, 0x1, 0), 0);
 expect(13, (u32)evf_poll(flag, 0x1, OR, &r), 0); expect(14, r, 0x1);
 expect(15, (u32)evf_poll(flag, 0x3, AND, &r), EBUSY); expect(16, r, 0x1);
 expect(17, (u32)evf_poll(flag, 0, OR, &r), EINVAL);
 expect(18, (u32)evf_poll(flag, 1, AND | OR, &r), EINVAL);
 expect(19, (u32)evf_poll(flag, 1, OR | CLEAR_ALL | CLEAR_PAT, &r), EINVAL);
 /* AND waits for every bit; CLEAR_PAT clears only the waited bits. */
 u64 a = spawn(evf_and, 0); await(&R[20]);
 expect(24, (u32)evf_set(flag, 0x2), 0); settle(); expect(25, R[23], 0);
 expect(26, (u32)evf_set(flag, 0x4), 0); join(a);
 expect(21, R[21], 0); expect(22, R[22], 0x7);
 expect(27, (u32)evf_poll(flag, ~0ULL, OR, &r), 0); expect(28, r, 0x1);
 /* MULTI: one Set wakes both OR waiters without clearing. */
 u64 b = spawn(evf_or, (void*)30), c = spawn(evf_or, (void*)33); await(&R[30]); await(&R[33]);
 expect(36, (u32)evf_set(flag, 0x10), 0); join(b); join(c);
 expect(31, R[31], 0); expect(32, R[32], 0x11); expect(34, R[34], 0); expect(35, R[35], 0x11);
 expect(37, (u32)evf_clear(flag, ~0x10ULL), 0);
 expect(38, (u32)evf_poll(flag, ~0ULL, OR, &r), 0); expect(39, r, 0x1);
 /* CLEAR_ALL clears the whole pattern after a satisfied wait. */
 u64 d = spawn(evf_clear_all, 0); await(&R[40]);
 expect(43, (u32)evf_set(flag, 0x20), 0); join(d);
 expect(41, R[41], 0); expect(42, R[42], 0x21);
 expect(44, (u32)evf_poll(flag, 0x1, OR, &r), EBUSY); expect(45, r, 0);
 u32 t = 2000;
 expect(46, (u32)evf_wait(flag, 0x80, OR, &r, &t), ETIMEDOUT); expect(47, t, 0); expect(48, r, 0);
 t = 0; expect(49, (u32)evf_wait(flag, 0x80, OR, &r, &t), ETIMEDOUT);
 /* Cancel releases the waiter with ECANCELED and installs the new pattern. */
 u64 e = spawn(evf_cancelled, 0); await(&R[50]);
 int n = -1; expect(53, (u32)evf_cancel(flag, 0x55, &n), 0); expect(54, (u32)n, 1); join(e);
 expect(51, R[51], ECANCELED); expect(52, R[52], 0x55);
 expect(55, (u32)evf_poll(flag, 0x55, AND, &r), 0);
 /* Default attributes allow one waiter; Delete releases it with EACCES. */
 expect(58, (u32)evf_create(&single, relocatedLabel, 0, 0, 0), 0);
 u64 f = spawn(evf_single, 0); await(&R[56]);
 t = 0; expect(59, (u32)evf_wait(single, 0x1, OR, 0, &t), EPERM);
 expect(60, (u32)evf_poll(single, 0x1, OR, 0), EPERM);
 expect(61, (u32)evf_delete(single), 0); join(f); expect(57, R[57], EACCES);
 expect(62, (u32)evf_set(single, 1), ESRCH); expect(63, (u32)evf_delete(single), ESRCH);
 /* A timed wait does not stall the other guest thread. */
 u64 g = spawn(evf_expiring, 0); await(&R[66]);
 while (!R[68]) { ++R[69]; thread_yield(); }
 join(g); expect(67, R[67], ETIMEDOUT); check(69, R[69] > 0);
 expect(70, (u32)evf_wait(flag, 1, OR, (u64*)1, 0), EFAULT);
 expect(71, (u32)evf_delete(flag), 0);
}

/* ---- equeue USER / TIMER / HRTIMER ---- */
static u64 queue;
static struct Event peer[4];
static void* queue_waiter(void* unused) {
 (void)unused; int out = -1; R[60] = 1; R[61] = (u32)queue_wait(queue, peer, 4, &out, 0); R[62] = (u32)out; R[63] = 1; return 0;
}
static void equeue(void) {
 struct Event ev[4]; int out = -1; u32 t = 0;
 expect(10, (u32)queue_create(&queue, relocatedLabel), 0);
 expect(11, (u32)user_add(queue, 7), 0);
 expect(12, (u32)user_trigger(queue, 9, (void*)1), ENOENT);
 expect(13, (u32)user_trigger(queue, 7, (void*)0x1234), 0);
 t = 0; expect(14, (u32)queue_wait(queue, ev, 4, &out, &t), 0); expect(15, (u32)out, 1);
 expect(16, event_id(&ev[0]), 7); expect(17, (u32)event_filter(&ev[0]), (u32)-11);
 expect(18, (u64)event_data(&ev[0]), 0x1234); expect(19, event_udata(&ev[0]), 0x1234);
 expect(20, event_fflags(&ev[0]), 0); expect(21, ev[0].flags, 0x1);
 /* Level-triggered: still reported until deleted. */
 t = 0; out = -1; expect(22, (u32)queue_wait(queue, ev, 4, &out, &t), 0); expect(23, (u32)out, 1);
 expect(24, (u32)user_delete(queue, 7), 0); expect(25, (u32)user_delete(queue, 7), ENOENT);
 t = 0; out = -1; expect(26, (u32)queue_wait(queue, ev, 4, &out, &t), ETIMEDOUT); expect(27, (u32)out, 0);
 /* Edge (EV_CLEAR): reported once per trigger. */
 expect(28, (u32)user_add_edge(queue, 8), 0);
 expect(29, (u32)user_trigger(queue, 8, (void*)0x55), 0);
 t = 0; out = -1; expect(30, (u32)queue_wait(queue, ev, 4, &out, &t), 0); expect(31, (u32)out, 1);
 expect(32, ev[0].flags, 0x21); expect(33, ev[0].udata, 0x55);
 t = 0; out = -1; expect(34, (u32)queue_wait(queue, ev, 4, &out, &t), ETIMEDOUT);
 /* A guest thread blocked on the queue wakes on another thread's trigger. */
 u64 a = spawn(queue_waiter, 0); await(&R[60]); expect(64, R[63], 0);
 expect(65, (u32)user_trigger(queue, 8, (void*)0x77), 0); join(a);
 expect(61, R[61], 0); expect(62, R[62], 1); expect(66, event_id(&peer[0]), 8); expect(67, event_udata(&peer[0]), 0x77);
 expect(68, (u32)user_delete(queue, 8), 0);
 /* TIMER: periodic, EV_CLEAR, data counts expirations. */
 expect(35, (u32)timer_add(queue, 3, 1000, (void*)0xabc), 0);
 out = -1; expect(36, (u32)queue_wait(queue, ev, 4, &out, 0), 0); expect(37, (u32)out, 1);
 expect(38, event_id(&ev[0]), 3); expect(39, (u32)event_filter(&ev[0]), (u32)-7);
 R[40] = (u64)event_data(&ev[0]); check(40, event_data(&ev[0]) >= 1); expect(41, event_udata(&ev[0]), 0xabc);
 expect(42, (u32)timer_delete(queue, 3), 0); expect(43, (u32)timer_delete(queue, 3), ENOENT);
 /* HRTIMER: one shot, removed once delivered. */
 const struct Timespec ts = {0, 1000000}, bad = {0, 1000000000};
 expect(44, (u32)hrtimer_add(queue, 4, &bad, 0), EINVAL);
 expect(45, (u32)hrtimer_add(queue, 4, 0, 0), EFAULT);
 expect(46, (u32)hrtimer_add(queue, 4, &ts, (void*)0xdef), 0);
 out = -1; expect(47, (u32)queue_wait(queue, ev, 4, &out, 0), 0); expect(48, (u32)out, 1);
 expect(49, event_id(&ev[0]), 4); expect(50, (u32)event_filter(&ev[0]), (u32)-15); expect(51, event_udata(&ev[0]), 0xdef);
 expect(52, (u32)hrtimer_delete(queue, 4), ENOENT);
 t = 2000; out = -1; expect(53, (u32)queue_wait(queue, ev, 4, &out, &t), ETIMEDOUT); expect(54, (u32)out, 0);
 expect(55, (u32)user_add(0x0c09ffffffffff00ULL, 1), EBADF);
 expect(56, (u32)queue_delete(queue), 0);
}

/* ---- stop requests: a blocked wait never returns once the host stops ---- */
static void stopping(void) {
 u64 object = 0;
 if (R[2] == 0) {
  check(10, evf_create(&object, relocatedLabel, 0, 0, 0) == 0);
  R[11] = 1; R[12] = (u32)evf_wait(object, 1, OR, 0, 0);
 } else if (R[2] == 1) {
  struct Event ev[1]; int out = -1;
  check(10, queue_create(&object, relocatedLabel) == 0);
  R[11] = 1; R[12] = (u32)queue_wait(object, ev, 1, &out, 0);
 } else {
  check(10, sema_create(&object, relocatedLabel, 1, 0, 1, 0) == 0);
  u32 t = 60000000; R[11] = 1; R[12] = (u32)sema_wait(object, 1, &t);
 }
 R[13] = 1;
}

EXPORT __attribute__((noreturn)) void _start(void) {
 R[8] = thread_self();
 if (R[1] == 1) semaphores();
 else if (R[1] == 2) eventflags();
 else if (R[1] == 3) equeue();
 else if (R[1] == 4) stopping();
 else check(7, 0);
 R[0] = 0x53594e4352454350ULL;
 process_exit(R[4] ? 1 : 0);
}
