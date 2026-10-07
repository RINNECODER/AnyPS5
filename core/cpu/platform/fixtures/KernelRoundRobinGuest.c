/* Public synthetic x86 guest: no vendor binaries or scheduling certification. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int scePthreadMutexInit(u64*, const u64*, const char*);
extern int scePthreadMutexUnlock(u64*);
extern int scePthreadMutexDestroy(u64*);
extern int scePthreadMutexattrInit(u64*);
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
__asm__(".type scePthreadMutexInit,@function\n.type scePthreadMutexUnlock,@function\n"
 ".type scePthreadMutexDestroy,@function\n.type scePthreadMutexattrInit,@function\n"
 ".type scePthreadMutexattrDestroy,@function\n.type scePthreadMutexattrSetprotocol,@function\n"
 ".type scePthreadMutexLock,@function\n.type attr_init,@function\n.type attr_destroy,@function\n"
 ".type attr_priority,@function\n.type attr_getpriority,@function\n.type attr_inherit,@function\n"
 ".type attr_policy,@function\n.type thread_create,@function\n.type thread_yield,@function\n"
 ".type thread_join,@function\n.type thread_self,@function\n.type process_exit,@function\n");
EXPORT volatile u64 KernelRoundRobinReceipt[128];
#define R KernelRoundRobinReceipt
#define CANARY 0x1badc0ffeef00d55ULL
#define ITERATIONS 100000ULL
struct Slot { u64 before, value, after; };
static struct Slot gate, donated, ownerGate, mediumGate, mutexAttr, threadAttr;
static const char label[] = "public-rr-fixture";
static const char* volatile relocatedLabel = label;
static __thread u64 tlsInitial __attribute__((used,aligned(16))) = 0x1020304050607080ULL;
static __thread u64 tlsZero[2] __attribute__((used));
static void status(int value) { if(value) ++R[4]; }
static void trace(u64 value) { u64 n=R[5]; if(n>=48) {++R[4];return;} R[64+n]=value;R[5]=n+1; }
static void init_slot(struct Slot* slot) { slot->before=slot->after=CANARY; }
static void guards(struct Slot* slot,u64 destroyedValue) {
 if(slot->before!=CANARY || slot->after!=CANARY || slot->value!=destroyedValue) ++R[6];
}
static u64 spawn(void* (*entry)(void*), int priority, int policy, int inherit, u64 arg) {
 u64 id=0; status(attr_init(&threadAttr.value));
 status(attr_policy(&threadAttr.value,policy));
 status(attr_priority(&threadAttr.value,&priority));
 status(attr_inherit(&threadAttr.value,inherit));
 status(thread_create(&id,&threadAttr.value,entry,(void*)arg,relocatedLabel));
 status(attr_destroy(&threadAttr.value));return id;
}
static void joined(u64 id,unsigned index) {
 void* returned=(void*)0xccccccccccccccccULL;
 status(thread_join(id,&returned));R[index]=(u64)returned;
}
static void* worker(void* arg) {
 const u64 role=(u64)arg;
 R[8+role]=thread_self();
 status(scePthreadMutexLock(&gate.value));status(scePthreadMutexUnlock(&gate.value));
 trace(10+role);
 /* Deliberately no host call or yield in this actual guest loop. */
 while(R[16+role]<ITERATIONS || (R[1]==9 && !R[3])) {
  ++R[16+role];
  if(R[16+role]==ITERATIONS/2) R[20+role]=R[17-role];
 }
 R[22+role]=R[17-role];trace(20+role);return (void*)(0x2000+role);
}
static void pair(int policy,int inherited) {
 u64 ids[2]={0,0};status(scePthreadMutexLock(&gate.value));
 for(unsigned role=0;role!=2;++role) {
  if(inherited==1) status(thread_create(&ids[role],0,worker,(void*)(u64)role,relocatedLabel));
  else ids[role]=spawn(worker,inherited==2?700:300,policy,inherited==2?4:0,role);
 }
 status(scePthreadMutexUnlock(&gate.value));
 joined(ids[0],48);joined(ids[1],49);
}
static void* rr_parent(void* arg) {
 /* Mode6's explicit attr is FIFO700 but inherit4 must copy parent RR300. */
 R[10]=thread_self();pair(1,R[1]==5?1:(R[1]==6?2:0));return arg;
}
/* Preserve an actual blocked PLT frame, its return word, stack and saved GPRs. */
__attribute__((visibility("hidden"))) void rr_wait_abi(u64*,volatile u64*);
__asm__(".text\n.hidden rr_wait_abi\n.type rr_wait_abi,@function\nrr_wait_abi:\n"
 "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $24,%rsp\n"
 "mov %rsi,(%rsp)\nmovabs $0x1234567890abcdef,%rax\n"
 "mov %rax,%rbx\nmov %rax,%rbp\nmov %rax,%r12\nmov %rax,%r13\nmov %rax,%r14\nmov %rax,%r15\n"
 "mov %rax,16(%rsp)\nlea -8(%rsp),%rax\nmov %rax,320(%rsi)\n"
 "lea 1f(%rip),%rax\nmov %rax,328(%rsi)\ncall scePthreadMutexLock@PLT\n1:\n"
 "mov (%rsp),%rdi\nmov %eax,%eax\nmov %rax,336(%rdi)\n"
 "movabs $0x1234567890abcdef,%rdx\nxor %eax,%eax\n"
 "mov %rbx,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rbp,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r12,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r13,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r14,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r15,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov 16(%rsp),%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rax,344(%rdi)\n"
 "mov %rsp,%rax\nsub 320(%rdi),%rax\nsub $8,%rax\nmov %rax,352(%rdi)\n"
 "add $24,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
 ".size rr_wait_abi,.-rr_wait_abi\n");
static void* low(void* arg) {
 R[8]=thread_self();status(scePthreadMutexLock(&donated.value));R[30]=1;
 status(scePthreadMutexLock(&ownerGate.value));status(scePthreadMutexUnlock(&ownerGate.value));
 trace(10);R[36]=0x1234432112344321ULL;status(scePthreadMutexUnlock(&donated.value));
 while(R[16]<ITERATIONS) ++R[16];trace(11);return arg;
}
static void* high(void* arg) {
 R[9]=thread_self();R[31]=1;rr_wait_abi(&donated.value,R);
 if(R[36]!=0x1234432112344321ULL) ++R[4];trace(20);R[35]=1;
 status(scePthreadMutexUnlock(&donated.value));return arg;
}
static void* medium(void* arg) {
 R[10]=thread_self();R[32]=1;status(scePthreadMutexLock(&mediumGate.value));
 status(scePthreadMutexUnlock(&mediumGate.value));trace(40);return arg;
}
static void donation(void) {
 u64 ids[3];status(scePthreadMutexLock(&ownerGate.value));status(scePthreadMutexLock(&mediumGate.value));
 ids[0]=spawn(low,700,3,0,0x3000);while(!R[30]) thread_yield();
 ids[1]=spawn(medium,500,3,0,0x3001);ids[2]=spawn(high,300,3,0,0x3002);
 R[2]=1;if(R[1]==8) while(!R[3]) thread_yield();
 status(scePthreadMutexUnlock(&mediumGate.value));status(scePthreadMutexUnlock(&ownerGate.value));
 for(unsigned i=0;i!=3;++i) joined(ids[i],48+i);
}
static void attribute_controls(void) {
 int parameter=256;u64 output=0xfeedbeef00000000ULL;
 status(attr_init(&threadAttr.value));status(attr_priority(&threadAttr.value,&parameter));
 status(attr_policy(&threadAttr.value,3));status(attr_getpriority(&threadAttr.value,(int*)&output));R[52]=output;
 parameter=767;status(attr_priority(&threadAttr.value,&parameter));status(attr_policy(&threadAttr.value,1));
 output=0xfeedbeef00000000ULL;status(attr_getpriority(&threadAttr.value,(int*)&output));R[53]=output;
 status(attr_policy(&threadAttr.value,3));parameter=300;status(attr_priority(&threadAttr.value,&parameter));
 output=0xfeedbeef00000000ULL;status(attr_getpriority(&threadAttr.value,(int*)&output));R[54]=output;
 status(attr_destroy(&threadAttr.value));
}
static void burn_near_turn(void) {
 /* Independent x86 instruction oracle: bounded below the logical4096 turn. */
 __asm__ volatile(".rept 3900\n nop\n .endr\n" ::: "memory");
}
static void* wake_first(void* arg) {
 burn_near_turn();R[30]=1;
 status(scePthreadMutexLock(&ownerGate.value));
 /* No further host import/yield until the full-turn oracle is published. */
 burn_near_turn();R[34]=1;
 status(scePthreadMutexUnlock(&ownerGate.value));return arg;
}
static void* wake_second(void* arg) {
 R[31]=1;status(scePthreadMutexLock(&mediumGate.value));
 R[35]=R[34];status(scePthreadMutexUnlock(&mediumGate.value));return arg;
}
static void wake_turn(void) {
 status(scePthreadMutexLock(&ownerGate.value));status(scePthreadMutexLock(&mediumGate.value));
 const u64 a=spawn(wake_first,700,3,0,0x5000);
 while(!R[30]) thread_yield();
 const u64 b=spawn(wake_second,700,3,0,0x5001);
 while(!R[31]) thread_yield();
 R[8]=a;R[9]=b;R[2]=1;while(!R[3]) thread_yield();
 status(scePthreadMutexUnlock(&ownerGate.value));status(scePthreadMutexUnlock(&mediumGate.value));
 joined(a,48);joined(b,49);
}
EXPORT int _start(void) {
 R[12]=thread_self();
 init_slot(&gate);init_slot(&donated);init_slot(&ownerGate);init_slot(&mediumGate);
 init_slot(&mutexAttr);init_slot(&threadAttr);attribute_controls();
 status(scePthreadMutexattrInit(&mutexAttr.value));status(scePthreadMutexattrSetprotocol(&mutexAttr.value,0));
 status(scePthreadMutexInit(&gate.value,&mutexAttr.value,relocatedLabel));
 status(scePthreadMutexInit(&ownerGate.value,&mutexAttr.value,relocatedLabel));
 status(scePthreadMutexInit(&mediumGate.value,&mutexAttr.value,relocatedLabel));
 status(scePthreadMutexattrSetprotocol(&mutexAttr.value,1));
 status(scePthreadMutexInit(&donated.value,&mutexAttr.value,relocatedLabel));
 if(R[1]==10) wake_turn();
 else if(R[1]==4 || R[1]==8) donation();
 else if(R[1]>=5 && R[1]<=7) joined(spawn(rr_parent,300,3,0,0x4000),50);
 else if(R[1]==2) {
  status(scePthreadMutexLock(&gate.value));
  u64 a=spawn(worker,300,3,0,0),b=spawn(worker,500,3,0,1);
  status(scePthreadMutexUnlock(&gate.value));joined(a,48);joined(b,49);
 } else pair(R[1]==1?1:3,0);
 status(scePthreadMutexDestroy(&gate.value));status(scePthreadMutexDestroy(&donated.value));
 status(scePthreadMutexDestroy(&ownerGate.value));status(scePthreadMutexDestroy(&mediumGate.value));
 status(scePthreadMutexattrDestroy(&mutexAttr.value));
 /* Merged mutex lifetime poisons destroyed slots with2; attrs clear their slots. */
 guards(&gate,2);guards(&donated,2);guards(&ownerGate,2);guards(&mediumGate,2);
 guards(&mutexAttr,0);guards(&threadAttr,0);
 R[0]=0x524f554e44524f42ULL;process_exit(0);
}
