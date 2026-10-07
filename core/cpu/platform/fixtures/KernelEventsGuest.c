/* Public synthetic linked x86 guest; event records use independently specified bytes. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int queue_create(u64*, const char*);
extern int queue_delete(u64);
extern int graphics_delete(u64, int);
extern int queue_wait(u64, void*, int, int*, const u32*);
extern int thread_create(u64*, const u64*, void* (*)(void*), void*, const char*);
extern void thread_yield(void);
extern int thread_join(u64, void**);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
__asm__(".type queue_create,@function\n.type queue_delete,@function\n.type queue_wait,@function\n.type graphics_delete,@function\n"
        ".type thread_create,@function\n.type thread_yield,@function\n.type thread_join,@function\n"
        ".type thread_self,@function\n.type process_exit,@function\n");
EXPORT volatile u64 KernelEventsReceipt[160];
static const char name[] = "public-event-queue";
static const char* volatile relocated_name = name;
static __thread u64 tls_initial __attribute__((used, aligned(16))) = 0x1020304050607080ULL;
static __thread u64 tls_zero[2] __attribute__((used));

/* Record the actual suspended PLT return frame and independently test preserved
 * registers, stack and EAX width. The host corrupt-return oracle edits this frame. */
__attribute__((visibility("hidden"))) void wait_abi(u64, void*, int, int*, const u32*, volatile u64*);
__asm__(".text\n.hidden wait_abi\n.type wait_abi,@function\nwait_abi:\n"
    "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $24,%rsp\n"
    "mov %r9,(%rsp)\nmovabs $0x1234567890abcdef,%rax\n"
    "mov %rax,%rbx\nmov %rax,%rbp\nmov %rax,%r12\nmov %rax,%r13\nmov %rax,%r14\nmov %rax,%r15\n"
    "mov %rax,16(%rsp)\nlea -8(%rsp),%rax\nmov %rax,256(%r9)\n"
    "lea 1f(%rip),%rax\nmov %rax,264(%r9)\ncall queue_wait@PLT\n1:\n"
    "mov (%rsp),%rdi\nmov %rax,80(%rdi)\n"
    "movabs $0x1234567890abcdef,%rdx\nxor %eax,%eax\n"
    "mov %rbx,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %rbp,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r12,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r13,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r14,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov %r15,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
    "mov 16(%rsp),%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rax,88(%rdi)\n"
    "mov %rsp,%rax\nsub 256(%rdi),%rax\nsub $8,%rax\nmov %rax,96(%rdi)\n"
    "add $24,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
    ".size wait_abi,.-wait_abi\n");

static void* worker(void* payload) {
    volatile u64* r = KernelEventsReceipt;
    r[9] = thread_self();
    r[8] = 1;
    if(r[1] == 10 || r[1] == 15) {
        r[45] = (u32)queue_wait(r[44], (void*)(r[6]+256), 1, (int*)(r[7]+256), 0);
        r[43] = r[1] == 15 ? (u32)graphics_delete(r[2], 0x20) : (u32)queue_delete(r[2]);
        r[46] = (u32)queue_delete(r[44]);
        r[8] = 2;
        return payload;
    }
    thread_yield();
    r[8] = 2;
    if(r[1] == 7) r[43] = (u32)queue_delete(r[2]);
    return payload;
}
static void copy_records(unsigned slot, unsigned count) {
    volatile u64* r = KernelEventsReceipt;
    volatile u64* out = (volatile u64*)r[6];
    for (unsigned i = 0; i < count * 4; ++i) r[slot+i] = out[i];
}
EXPORT int _start(void) {
    volatile u64* r = KernelEventsReceipt;
    void* out = (void*)r[6]; int* count = (int*)r[7];
    u64 child = 0; void* payload = 0;
    r[4] = thread_self();
    r[16] = (u32)queue_create((u64*)&r[2], relocated_name);
    r[3] = 1;
    thread_yield(); /* Real owner boundary registers subscriptions after Create. */
    if(r[1] == 12) {
        r[49] = (u32)queue_wait(r[2], (void*)(r[6]+4080), 2, count, (const u32*)&r[22]);
        process_exit(12);
    }
    if (r[1] == 1) {
        r[17] = (u32)queue_wait(r[2], out, 2, count, (const u32*)&r[20]);
        r[18] = *(u32*)count;
        r[20] = 0xfeedbeef00001388ULL; /* 5000 microseconds, upper guard immutable. */
        wait_abi(r[2], out, 2, count, (const u32*)&r[20], r);
        r[19] = *(u32*)count;
        r[21] = (u32)queue_wait(r[2], out, 0, count, (const u32*)&r[22]);
        r[23] = (u32)queue_wait(r[2], out, -1, count, (const u32*)&r[22]);
    } else if (r[1] == 2) {
        wait_abi(r[2], out, 2, count, 0, r);
        r[24] = *(u32*)count; copy_records(64, (unsigned)r[24]);
        r[25] = (u32)queue_wait(r[2], out, 2, count, (const u32*)&r[22]);
        r[26] = *(u32*)count; copy_records(72, (unsigned)r[26]);
        r[27] = (u32)queue_wait(r[2], out, 2, count, (const u32*)&r[22]);
        r[28] = *(u32*)count;
        r[3] = 2; thread_yield();
        r[29] = (u32)queue_wait(r[2], out, 2, count, 0);
        r[30] = *(u32*)count; copy_records(76, (unsigned)r[30]);
    } else {
        if(r[1] == 10 || r[1] == 15) r[47] = (u32)queue_create((u64*)&r[44], relocated_name);
        r[31] = (u32)thread_create(&child, (r[1] == 10 || r[1] == 15) ? (const u64*)&r[120] : 0,
                                  worker, (void*)0x5566778899aabbccULL, relocated_name);
        wait_abi(r[2], out, 2, count, 0, r);
        r[5] = 1;
        r[34] = *(u32*)count;
        if(r[10] == 0) copy_records(64, (unsigned)r[34]);
        r[35] = (u32)thread_join(child, &payload); r[36] = (u64)payload;
        if(r[1] == 0 || r[1] == 14) {
            r[50] = (u32)queue_wait(r[2], out, 2, count, (const u32*)&r[22]);
            r[51] = *(u32*)count;
            copy_records(84,1); /* Empty poll preserves the prior output record. */
            if(r[1] == 0) {
                r[3] = 3; thread_yield();
                r[52] = (u32)queue_wait(r[2], out, 2, count, 0);
                r[53] = *(u32*)count;copy_records(80, (unsigned)r[53]);
            }
        }
    }
    r[37] = (u32)queue_delete(r[2]);
    r[38] = (u32)queue_wait(r[2], out, 2, count, (const u32*)&r[22]);
    r[39] = (u32)queue_delete(r[2]);
    r[40] = (u32)queue_create((u64*)&r[41], relocated_name);
    r[42] = (u32)queue_delete(r[41]);
    r[0] = 0x455155455545574bULL;
    process_exit(0);
}
