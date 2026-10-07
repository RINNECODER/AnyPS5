/* Public synthetic x86 guest; the receipt is an independent scheduling/data oracle. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int scePthreadMutexInit(u64*, const u64*, const char*);
extern int scePthreadMutexTrylock(u64*);
extern int scePthreadMutexUnlock(u64*);
extern int scePthreadMutexDestroy(u64*);
extern int scePthreadMutexattrInit(u64*);
extern int scePthreadMutexattrSettype(u64*, int);
extern int scePthreadMutexattrDestroy(u64*);
extern int scePthreadMutexattrSetprotocol(u64*, int);
extern int scePthreadMutexLock(u64*);
extern int attr_init(u64*);
extern int attr_destroy(u64*);
extern int attr_priority(u64*, const int*);
extern int attr_getpriority(u64*, int*);
extern int attr_inherit(u64*, int);
extern int attr_policy(u64*, int);
extern int thread_create(u64*, const u64*, void* (*)(void*), void*, const char*);
extern void thread_yield(void);
extern int thread_join(u64, void**);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
__asm__(".type scePthreadMutexInit,@function\n.type scePthreadMutexTrylock,@function\n"
 ".type scePthreadMutexUnlock,@function\n.type scePthreadMutexDestroy,@function\n"
 ".type scePthreadMutexattrInit,@function\n.type scePthreadMutexattrSettype,@function\n"
 ".type scePthreadMutexattrDestroy,@function\n.type scePthreadMutexattrSetprotocol,@function\n"
 ".type scePthreadMutexLock,@function\n.type attr_init,@function\n.type attr_destroy,@function\n"
 ".type attr_priority,@function\n.type attr_getpriority,@function\n.type attr_inherit,@function\n"
 ".type attr_policy,@function\n.type thread_create,@function\n.type thread_yield,@function\n"
 ".type thread_join,@function\n.type thread_self,@function\n.type process_exit,@function\n");
EXPORT volatile u64 KernelPriorityReceipt[128];
struct Slot { u64 before; u64 value; u64 after; };
static struct Slot slots[6], threadAttr;
static const char label[] = "public-priority-fixture";
static const char* volatile relocatedLabel = label;
static __thread u64 tlsInitial __attribute__((used,aligned(16))) = 0x1020304050607080ULL;
static __thread u64 tlsZero[2] __attribute__((used));
static volatile u64 payload;
#define A (&slots[0].value)
#define B (&slots[1].value)
#define OWNER_GATE (&slots[2].value)
#define MEDIUM_GATE (&slots[3].value)
#define MA (&slots[4].value)
#define FAST_GATE (&slots[5].value)
#define CANARY 0x1badc0ffeef00d55ULL
static void status(int value) { if (value) ++KernelPriorityReceipt[4]; }
static void trace(u64 value) {
 volatile u64* r = KernelPriorityReceipt;
 u64 n = r[5]; if (n >= 48) { ++r[4]; return; } r[64+n] = value; r[5] = n+1;
}
/* This real blocked PLT frame checks the preserved ABI, stack and return word. */
__attribute__((visibility("hidden"))) void priority_wait_abi(u64*, volatile u64*);
__asm__(".text\n.hidden priority_wait_abi\n.type priority_wait_abi,@function\npriority_wait_abi:\n"
 "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $24,%rsp\n"
 "mov %rsi,(%rsp)\nmov %rdi,8(%rsp)\nmovabs $0x1234567890abcdef,%rax\n"
 "mov %rax,%rbx\nmov %rax,%rbp\nmov %rax,%r12\nmov %rax,%r13\nmov %rax,%r14\nmov %rax,%r15\n"
 "mov %rax,16(%rsp)\nlea -8(%rsp),%rax\nmov %rax,256(%rsi)\n"
 "lea 1f(%rip),%rax\nmov %rax,264(%rsi)\ncall scePthreadMutexLock@PLT\n1:\n"
 "mov (%rsp),%rdi\nmov %eax,%eax\nmov %rax,272(%rdi)\n"
 "movabs $0x1234567890abcdef,%rdx\nxor %eax,%eax\n"
 "mov %rbx,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %rbp,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r12,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r13,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r14,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r15,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov 16(%rsp),%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rax,280(%rdi)\n"
 "mov %rsp,%rax\nsub 256(%rdi),%rax\nsub $8,%rax\nmov %rax,288(%rdi)\n"
 "add $24,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
 ".size priority_wait_abi,.-priority_wait_abi\n");

static void* low(void* arg) {
 volatile u64* r = KernelPriorityReceipt; const u64 mode = r[1];
 r[9] = thread_self(); status(scePthreadMutexLock(A));
 if (mode == 2) status(scePthreadMutexLock(A));
 if (mode == 3) status(scePthreadMutexLock(B));
 r[41] = (u64)A; r[42] = *A; r[16] = 1;
 status(scePthreadMutexLock(OWNER_GATE)); status(scePthreadMutexUnlock(OWNER_GATE));
 trace(10); payload = 0x1020304050607080ULL;
 if (mode == 2) {
  status(scePthreadMutexUnlock(A)); thread_yield();
  r[40] = r[17] == 0; trace(11);
 }
 status(scePthreadMutexUnlock(A));
 r[38] = (u32)scePthreadMutexTrylock(A); r[39] = (u32)scePthreadMutexUnlock(A);
 thread_yield();
 if (mode == 3) { trace(12); status(scePthreadMutexUnlock(B)); thread_yield(); }
 trace(13); r[37] = payload; r[6] |= 1; return arg;
}
static void* high(void* arg) {
 volatile u64* r = KernelPriorityReceipt; r[10] = thread_self(); r[18] = 1;
 priority_wait_abi(r[1] == 1 ? B : A, r);
 r[17] = 1; if (payload != 0x1020304050607080ULL) ++r[43];
 payload = 0x2131415161718191ULL; trace(20);
 status(scePthreadMutexUnlock(r[1] == 1 ? B : A)); r[6] |= 2; return arg;
}
static void* middle(void* arg) {
 volatile u64* r = KernelPriorityReceipt; r[11] = thread_self();
 status(scePthreadMutexLock(B)); r[19] = 1; status(scePthreadMutexLock(A));
 trace(30); status(scePthreadMutexUnlock(A)); status(scePthreadMutexUnlock(B));
 r[6] |= 4; return arg;
}
static void* medium(void* arg) {
 volatile u64* r = KernelPriorityReceipt; r[12] = thread_self(); r[20] = 1;
 status(scePthreadMutexLock(MEDIUM_GATE)); status(scePthreadMutexUnlock(MEDIUM_GATE));
 trace(40); r[6] |= 8; return arg;
}
static void* fast_medium(void* arg) {
 volatile u64* r = KernelPriorityReceipt; r[14] = thread_self();
 status(scePthreadMutexLock(FAST_GATE)); status(scePthreadMutexUnlock(FAST_GATE));
 trace(45); r[6] |= 32; return arg;
}
static void* second(void* arg) {
 volatile u64* r = KernelPriorityReceipt; r[13] = thread_self(); r[21] = 1;
 status(scePthreadMutexLock(r[1] == 3 ? B : A));
 trace(50); status(scePthreadMutexUnlock(r[1] == 3 ? B : A)); r[6] |= 16; return arg;
}
static u64 spawn(void* (*entry)(void*), int priority, u64 result) {
 volatile u64* r = KernelPriorityReceipt; u64 id = 0;
 status(attr_init(&threadAttr.value));
 int requested = priority, read = -1;
 status(attr_policy(&threadAttr.value, 1));
 status(attr_priority(&threadAttr.value, &requested));
 status(attr_getpriority(&threadAttr.value, &read));
 if (read != priority) ++r[4];
 status(attr_inherit(&threadAttr.value, 0));
 status(thread_create(&id, &threadAttr.value, entry, (void*)result, relocatedLabel));
 status(attr_destroy(&threadAttr.value)); return id;
}
static void* fifo_worker(void* arg) {
 volatile u64* r = KernelPriorityReceipt; const u64 role = (u64)arg - 0x2000;
 r[9+role] = thread_self(); trace(60+role); thread_yield(); trace(70+role); return arg;
}
static void attribute_controls(void) {
 volatile u64* r = KernelPriorityReceipt;
 struct Slot control = {CANARY, 0, CANARY}, copied = {CANARY, 0, CANARY};
 u64 parameter = 0xabcdef0100000000ULL, output = 0xfeedbeef00000000ULL;
 status(attr_init(&control.value)); status(attr_getpriority(&control.value, (int*)&output));
 if (output != 0xfeedbeef000002bcULL) ++r[4]; /* default700, exactly four output bytes */
 parameter |= 256; status(attr_priority(&control.value, (const int*)&parameter));
 output = 0xfeedbeef00000000ULL; status(attr_getpriority(&control.value, (int*)&output));
 if (output != 0xfeedbeef00000100ULL) ++r[4];
 /* Selected pinned SCE/FreeBSD contract: parameter then FIFO resets to700;
    FIFO then parameter selects256. Both orders execute actual guest PLT calls. */
 status(attr_policy(&control.value, 1));
 output = 0xfeedbeef00000000ULL; status(attr_getpriority(&control.value, (int*)&output));
 if (output != 0xfeedbeef000002bcULL) ++r[4];
 status(attr_priority(&control.value, (const int*)&parameter));
 output = 0xfeedbeef00000000ULL; status(attr_getpriority(&control.value, (int*)&output));
 if (output != 0xfeedbeef00000100ULL) ++r[4];
 parameter = 767; status(attr_priority(&control.value, (const int*)&parameter));
 parameter = 255; r[112] = (u32)attr_priority(&control.value, (const int*)&parameter);
 parameter = 768; r[113] = (u32)attr_priority(&control.value, (const int*)&parameter);
 r[114] = (u32)attr_priority(&control.value, (const int*)0);
 r[115] = (u32)attr_inherit(&control.value, 1);
 r[116] = (u32)attr_policy(&control.value, 0);
 output = 0xfeedbeef00000000ULL; status(attr_getpriority(&control.value, (int*)&output));
 if (output != 0xfeedbeef000002ffULL) ++r[4]; /* invalid setters preserved767 */
 copied.value = control.value;
 r[117] = (u32)attr_getpriority(&copied.value, (int*)&output);
 r[118] = output; r[119] = (u32)attr_destroy(&copied.value);
 const u64 old = control.value; status(attr_destroy(&control.value));
 r[120] = (u32)attr_getpriority(&control.value, (int*)&output);
 status(attr_init(&control.value)); if (control.value == old) ++r[4];
 r[121] = (u32)attr_getpriority(&copied.value, (int*)&output);
 r[122] = (u32)attr_getpriority(&control.value, (int*)0);
 status(attr_destroy(&control.value));
 if (control.before != CANARY || control.after != CANARY || copied.before != CANARY || copied.after != CANARY)
  ++r[44];
 if (r[1] == 11 || r[1] == 12) {
  status(attr_init(&threadAttr.value)); parameter = 300;
  status(attr_priority(&threadAttr.value, (const int*)&parameter)); r[55] = (u64)&threadAttr.value;
  r[56] = threadAttr.value;
  status(attr_policy(&threadAttr.value, r[1] == 11 ? 2 : 3));
 }
}
EXPORT int _start(void) {
 volatile u64* r = KernelPriorityReceipt; const u64 mode = r[1];
 u64 ids[5]; unsigned count = 0; void* returned;
 r[8] = thread_self(); attribute_controls();
 if (mode == 9) {
  threadAttr.before = threadAttr.after = CANARY;
  for (unsigned i = 0; i != 3; ++i) ids[i] = spawn(fifo_worker, 300, 0x2000+i);
  for (unsigned i = 0; i != 3; ++i) {
   returned = (void*)0xccccccccccccccccULL;
   status(thread_join(ids[i], &returned)); r[48+i] = (u64)returned;
  }
  if (threadAttr.before != CANARY || threadAttr.after != CANARY || threadAttr.value) ++r[44];
  r[0] = 0x5052494f52495459ULL; process_exit(0);
 }
 for (unsigned i = 0; i != 6; ++i) slots[i].before = slots[i].after = CANARY;
 threadAttr.before = threadAttr.after = CANARY;
 status(scePthreadMutexattrInit(MA));
 status(scePthreadMutexattrSettype(MA, mode == 2 ? 2 : 3));
 status(scePthreadMutexattrSetprotocol(MA, mode == 10 ? 2 : (mode == 5 ? 0 : 1)));
 status(scePthreadMutexInit(A, MA, relocatedLabel));
 status(scePthreadMutexInit(B, MA, relocatedLabel));
 status(scePthreadMutexattrSetprotocol(MA, 0));
 status(scePthreadMutexInit(OWNER_GATE, MA, relocatedLabel));
 status(scePthreadMutexInit(MEDIUM_GATE, MA, relocatedLabel));
 status(scePthreadMutexInit(FAST_GATE, MA, relocatedLabel));
 status(scePthreadMutexLock(OWNER_GATE)); status(scePthreadMutexLock(MEDIUM_GATE));
 status(scePthreadMutexLock(FAST_GATE));
 ids[count++] = spawn(low, 700, 0x1001);
 while (!r[16]) thread_yield();
 if (mode == 1) ids[count++] = spawn(middle, 600, 0x1002);
 ids[count++] = spawn(medium, 500, 0x1003);
 if (mode == 3) ids[count++] = spawn(fast_medium, 350, 0x1006);
 if (mode == 4) {
  /* First waiter has priority400; later priority300 must win despite FIFO arrival. */
  ids[count++] = spawn(second, 400, 0x1004);
  ids[count++] = spawn(high, 300, 0x1005);
 } else {
  ids[count++] = spawn(high, 300, 0x1005);
  if (mode == 3) ids[count++] = spawn(second, 400, 0x1004);
  if (mode == 8) ids[count++] = spawn(second, 300, 0x1004);
 }
 r[2] = 1;
 if (mode == 6 || mode == 7) while (!r[3]) thread_yield();
 status(scePthreadMutexUnlock(MEDIUM_GATE)); status(scePthreadMutexUnlock(FAST_GATE));
 status(scePthreadMutexUnlock(OWNER_GATE));
 for (unsigned i = 0; i != count; ++i) {
  returned = (void*)0xccccccccccccccccULL;
  status(thread_join(ids[i], &returned)); r[48+i] = (u64)returned;
  if ((u64)returned < 0x1001 || (u64)returned > 0x1006) ++r[4];
 }
 for (unsigned i = 0; i != 4; ++i) status(scePthreadMutexDestroy(&slots[i].value));
 status(scePthreadMutexDestroy(FAST_GATE));
 status(scePthreadMutexattrDestroy(MA));
 for (unsigned i = 0; i != 6; ++i)
  if (slots[i].before != CANARY || slots[i].after != CANARY) ++r[44];
 if (threadAttr.before != CANARY || threadAttr.after != CANARY || threadAttr.value) ++r[44];
 r[0] = 0x5052494f52495459ULL; process_exit(0);
}
