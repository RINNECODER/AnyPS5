/* Public synthetic guest: linked x86 calls exercise runtime contracts, not vendor semantics. */
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
extern int thread_create(u64*, const u64*, void* (*)(void*), void*, const char*);
extern void thread_yield(void);
extern int thread_join(u64, void**);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
__asm__(".type scePthreadMutexInit,@function\n.type scePthreadMutexTrylock,@function\n"
        ".type scePthreadMutexUnlock,@function\n.type scePthreadMutexDestroy,@function\n"
        ".type scePthreadMutexattrInit,@function\n.type scePthreadMutexattrSettype,@function\n"
        ".type scePthreadMutexattrDestroy,@function\n.type scePthreadMutexattrSetprotocol,@function\n"
        ".type scePthreadMutexLock,@function\n.type thread_create,@function\n"
        ".type thread_yield,@function\n.type thread_join,@function\n.type thread_self,@function\n"
        ".type process_exit,@function\n");
EXPORT volatile u64 KernelMutexReceipt[64];
static volatile u64 mutex, attribute;
static const char label[] = "public-mutex-fixture";
static const char* volatile relocated_label = label;
/* Actual compiler TLS gives each created guest the existing immutable TLS factory. */
static __thread u64 tls_initial __attribute__((used, aligned(16))) = 0x1020304050607080ULL;
static __thread u64 tls_zero[2] __attribute__((used));

/* Independently verify the exact continuation's return value, stack and all ABI-preserved
 * integer registers across a blocked lock. The saved return word is real guest memory. */
__attribute__((visibility("hidden"))) void wait_abi(u64*, volatile u64*);
__asm__(".text\n.hidden wait_abi\n.type wait_abi,@function\nwait_abi:\n"
    "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $24,%rsp\n"
    "mov %rsi,(%rsp)\nmov %rdi,8(%rsp)\nmovabs $0x1234567890abcdef,%rax\n"
    "mov %rax,%rbx\nmov %rax,%rbp\nmov %rax,%r12\nmov %rax,%r13\nmov %rax,%r14\nmov %rax,%r15\n"
    "mov %rax,16(%rsp)\nlea -8(%rsp),%rax\nmov %rax,256(%rsi)\n"
    "lea 1f(%rip),%rax\nmov %rax,264(%rsi)\ncall scePthreadMutexLock@PLT\n1:\n"
    "mov (%rsp),%rdi\nmov %eax,%eax\nmov %rax,104(%rdi)\n"
    "movabs $0x1234567890abcdef,%rdx\nxor %eax,%eax\n"
    "mov %rbx,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %rbp,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r12,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r13,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r14,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r15,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov 16(%rsp),%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rax,112(%rdi)\n"
    "mov %rsp,%rax\nsub 256(%rdi),%rax\nsub $8,%rax\nmov %rax,120(%rdi)\n"
    "add $24,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
    ".size wait_abi,.-wait_abi\n");

static void* worker(void* argument) {
    volatile u64* r = KernelMutexReceipt;
    r[2] = thread_self();
    if (r[63] == 4) {
        r[10] = (u32)scePthreadMutexLock((u64*)&mutex);
        r[4] = 1;
        thread_yield(); /* Parent now blocks; exiting while owning is unsupported. */
        r[35] = 1;
        return argument;
    }
    r[10] = (u32)scePthreadMutexTrylock((u64*)&mutex);
    r[11] = (u32)scePthreadMutexUnlock((u64*)&mutex);
    r[12] = (u32)scePthreadMutexDestroy((u64*)&mutex);
    r[4] = 1;
    wait_abi((u64*)&mutex, r);
    r[16] = 1;
    if (r[13] || !r[5] || r[6] != 0x1020304050607080ULL) ++r[7];
    r[6] += 0x1111111111111111ULL;
    r[17] = 1;
    thread_yield(); /* Parent must be unable to acquire or unlock our ownership. */
    if (r[6] != 0x2131415161718191ULL) ++r[7];
    r[6] ^= 0xa55aa55aa55aa55aULL;
    r[36] = (u32)scePthreadMutexUnlock((u64*)&mutex);
    return argument;
}

EXPORT int _start(void) {
    volatile u64* r = KernelMutexReceipt;
    u64 child = 0; void* result = (void*)0xccccccccccccccccULL;
    const u64 mode = r[63];
    r[1] = thread_self();
    r[37] = (u32)scePthreadMutexattrInit((u64*)&attribute);
    r[38] = (u32)scePthreadMutexattrSettype((u64*)&attribute, mode == 1 ? 2 : 3);
    r[39] = (u32)scePthreadMutexattrSetprotocol((u64*)&attribute, 0);
    r[40] = (u32)scePthreadMutexInit((u64*)&mutex, (u64*)&attribute, relocated_label);
    r[41] = mutex; r[42] = (u64)&mutex;
    if (mode != 4) {
        r[3] = (u32)scePthreadMutexLock((u64*)&mutex);
        if (mode == 1) {
            r[43] = (u32)scePthreadMutexLock((u64*)&mutex);
            r[44] = (u32)scePthreadMutexTrylock((u64*)&mutex);
            r[45] = (u32)scePthreadMutexUnlock((u64*)&mutex);
        }
    }
    r[6] = 0x1020304050607080ULL;
    r[46] = (u32)thread_create(&child, (const u64*)0, worker, (void*)0x5566778899aabbccULL, relocated_label);
    r[47] = child;
    thread_yield();
    if (mode == 4) { wait_abi((u64*)&mutex, r); r[16] = 1; process_exit(4); }
    r[18] = r[4] == 1 && r[16] == 0 && r[6] == 0x1020304050607080ULL;
    if (mode == 1) {
        r[48] = (u32)scePthreadMutexUnlock((u64*)&mutex);
        thread_yield();
        r[19] = r[16] == 0 && r[6] == 0x1020304050607080ULL;
    }
    if (mode == 2) *(volatile u64*)r[32] = 0x1122334455667788ULL;
    if (mode == 3) for (;;) ++r[49]; /* Host bounded budget parks owner and waiter. */
    r[5] = 1;
    r[50] = (u32)scePthreadMutexUnlock((u64*)&mutex);
    thread_yield();
    r[51] = r[17] == 1 && r[6] == 0x2131415161718191ULL;
    r[20] = (u32)scePthreadMutexTrylock((u64*)&mutex);
    r[21] = (u32)scePthreadMutexUnlock((u64*)&mutex);
    r[22] = (u32)scePthreadMutexDestroy((u64*)&mutex);
    thread_yield();
    r[23] = (u32)thread_join(child, &result);
    r[24] = (u64)result;
    r[25] = (u32)scePthreadMutexTrylock((u64*)&mutex);
    r[26] = (u32)scePthreadMutexUnlock((u64*)&mutex);
    r[27] = (u32)scePthreadMutexDestroy((u64*)&mutex);
    r[28] = (u32)scePthreadMutexattrDestroy((u64*)&attribute);
    r[29] = mutex; r[30] = attribute;
    r[0] = 0x4d5554455857414bULL;
    process_exit(0);
}
