/* Public linked x86 guest: synthetic scheduler contract, no vendor/title execution. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int scePthreadCondattrInit(u64*);
extern int scePthreadCondattrDestroy(u64*);
extern int posix_cond_wait(u64*,u64*);
extern int posix_cond_signal(u64*);
extern int posix_cond_broadcast(u64*);
extern int scePthreadCondInit(u64*, const u64*, const char*);
extern int scePthreadCondDestroy(u64*);
extern int scePthreadCondWait(u64*, u64*);
extern int scePthreadCondSignal(u64*);
extern int scePthreadCondBroadcast(u64*);
extern int scePthreadMutexInit(u64*, const u64*, const char*);
extern int scePthreadMutexTrylock(u64*);
extern int scePthreadMutexLock(u64*);
extern int scePthreadMutexUnlock(u64*);
extern int scePthreadMutexDestroy(u64*);
extern int scePthreadMutexattrInit(u64*);
extern int scePthreadMutexattrSettype(u64*, int);
extern int scePthreadMutexattrSetprotocol(u64*, int);
extern int scePthreadMutexattrDestroy(u64*);
extern int attr_init(u64*);
extern int attr_destroy(u64*);
extern int attr_priority(u64*, const int*);
extern int attr_inherit(u64*, int);
extern int attr_policy(u64*, int);
extern int thread_create(u64*, const u64*, void* (*)(void*), void*, const char*);
extern void thread_yield(void);
extern int thread_join(u64, void**);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
#define FUNC(name) ".type " #name ",@function\n"
__asm__(FUNC(scePthreadCondInit) FUNC(scePthreadCondDestroy) FUNC(scePthreadCondWait)
 FUNC(posix_cond_wait) FUNC(posix_cond_signal) FUNC(posix_cond_broadcast) FUNC(scePthreadCondSignal) FUNC(scePthreadCondBroadcast) FUNC(scePthreadCondattrInit) FUNC(scePthreadCondattrDestroy) FUNC(scePthreadMutexInit)
 FUNC(scePthreadMutexTrylock) FUNC(scePthreadMutexLock) FUNC(scePthreadMutexUnlock)
 FUNC(scePthreadMutexDestroy) FUNC(scePthreadMutexattrInit) FUNC(scePthreadMutexattrSettype)
 FUNC(scePthreadMutexattrSetprotocol) FUNC(scePthreadMutexattrDestroy)
 FUNC(attr_init) FUNC(attr_destroy) FUNC(attr_priority) FUNC(attr_inherit) FUNC(attr_policy)
 FUNC(thread_create) FUNC(thread_yield) FUNC(thread_join) FUNC(thread_self) FUNC(process_exit));
EXPORT volatile u64 KernelConditionReceipt[128];
#define R KernelConditionReceipt
#define CANARY 0x1badc0ffeef00d55ULL
struct Slot { u64 before, value, after; };
static struct Slot mutex, condition, mutexAttr, threadAttr, conditionAttr;
static const char label[] = "public-condition-fixture";
static const char* volatile relocatedLabel = label;
static __thread u64 tlsInitial __attribute__((used,aligned(16))) = 0x1020304050607080ULL;
static __thread u64 tlsZero[2] __attribute__((used));
static volatile u64 epoch;
static void status(int code) { if(code) ++R[4]; }
static void guards(const struct Slot* slot) { if(slot->before != CANARY || slot->after != CANARY) ++R[5]; }
/* Preserve real suspended wait's return word, stack and six callee-saved GPRs. */
__attribute__((visibility("hidden"))) void condition_wait_abi(u64*, u64*, volatile u64*);
__asm__(".text\n.hidden condition_wait_abi\n.type condition_wait_abi,@function\ncondition_wait_abi:\n"
 "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $24,%rsp\n"
 "mov %rdx,(%rsp)\nmovabs $0x1234567890abcdef,%rax\n"
 "mov %rax,%rbx\nmov %rax,%rbp\nmov %rax,%r12\nmov %rax,%r13\nmov %rax,%r14\nmov %rax,%r15\n"
 "mov %rax,16(%rsp)\nlea -8(%rsp),%rax\nmov %rax,320(%rdx)\n"
 "lea 1f(%rip),%rax\nmov %rax,328(%rdx)\ncall scePthreadCondWait@PLT\n1:\n"
 "mov (%rsp),%rdi\nmov %eax,%eax\nmov %rax,336(%rdi)\n"
 "movabs $0x1234567890abcdef,%rdx\nxor %eax,%eax\n"
 "mov %rbx,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rbp,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r12,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r13,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r14,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r15,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov 16(%rsp),%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rax,344(%rdi)\n"
 "mov %rsp,%rax\nsub 320(%rdi),%rax\nsub $8,%rax\nmov %rax,352(%rdi)\n"
 "add $24,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
 ".size condition_wait_abi,.-condition_wait_abi\n");
static void* waiter(void* argument) {
 const u64 role=(u64)argument;
 R[8+role]=thread_self(); status(scePthreadMutexLock(&mutex.value));
 if(R[1]==7) status(scePthreadMutexLock(&mutex.value));
 R[12] |= 1ULL << role;
 if(role==0) condition_wait_abi(&condition.value,&mutex.value,R);
 else R[45+role]=(u32)(R[1]==8 || R[1]==6 ? posix_cond_wait(&condition.value,&mutex.value) : scePthreadCondWait(&condition.value,&mutex.value));
 if(epoch != 0x1020304050607080ULL) ++R[6];
 const u64 n=R[15]; R[64+n]=role+1; R[15]=n+1; R[13] |= 1ULL << role;
 /* Wait must have reacquired: trylock reports error-checking self deadlock/busy,
    and parent must be unable to acquire or unlock while this thread yields. */
 R[48+role]=R[1]==7 ? 0 : (u32)scePthreadMutexTrylock(&mutex.value);
 if((R[1] == 0 || R[1]==7) && role==0) {
  if(R[1]==7) status(scePthreadMutexUnlock(&mutex.value));
  thread_yield();
 }
 if(R[1]==7 && role!=0) status(scePthreadMutexUnlock(&mutex.value));
 status(scePthreadMutexUnlock(&mutex.value));
 if(R[1] == 6) {
  while(R[80+role] < 100000) { ++R[80+role]; if(R[80+role] == 50000) R[84+role]=R[80+(role+1)%3]; }
 }
 return (void*)(0x3000+role);
}
static u64 spawn(unsigned role) {
 u64 id=0; int priority=(R[1]==11 ? 500 : (R[1]==5 ? (role==0 ? 500 : 300) : (R[1]==8 ? (role==0 ? 500 : (role==1 ? 300 : 400)) : 700)));
 status(attr_init(&threadAttr.value)); status(attr_policy(&threadAttr.value,R[1]==6 ? 3 : 1));
 status(attr_priority(&threadAttr.value,&priority)); status(attr_inherit(&threadAttr.value,0));
 status(thread_create(&id,&threadAttr.value,waiter,(void*)(u64)role,relocatedLabel));
 status(attr_destroy(&threadAttr.value)); return id;
}
static void joined(u64 id,unsigned role) {
 void* result=(void*)0xccccccccccccccccULL;
 status(thread_join(id,&result)); R[52+role]=(u64)result;
}
static void controls(void) {
 struct Slot copied={CANARY,condition.value,CANARY};
 R[90]=(u32)scePthreadCondSignal(&copied.value); /* a copied handle names the live condition */
 R[92]=(u32)scePthreadCondWait(&condition.value,&mutex.value); /* unowned */
 const u64 stale=condition.value;
 status(scePthreadCondDestroy(&condition.value));
 R[93]=(u32)scePthreadCondSignal(&condition.value);
 R[91]=(u32)scePthreadCondDestroy(&copied.value); /* copy of a destroyed condition */
 status(scePthreadCondInit(&condition.value,0,relocatedLabel));
 if(condition.value==stale) ++R[6];
 R[94]=(u32)scePthreadCondBroadcast(&copied.value);
 R[97]=(u32)posix_cond_broadcast(&copied.value); guards(&copied);
}
EXPORT int _start(void) {
 u64 ids[3]; const unsigned count=(R[1]==2 || R[1]==4 || R[1]==5 || R[1]==11 ? (R[1]==5 ? 2 : 1) : 3);
 R[7]=thread_self();
 mutex.before=mutex.after=condition.before=condition.after=CANARY;
 mutexAttr.before=mutexAttr.after=threadAttr.before=threadAttr.after=CANARY;
 conditionAttr.before=conditionAttr.after=CANARY;
 status(scePthreadMutexattrInit(&mutexAttr.value));
 status(scePthreadMutexattrSettype(&mutexAttr.value,R[1]==7 ? 2 : 3));
 status(scePthreadMutexattrSetprotocol(&mutexAttr.value,1));
 status(scePthreadMutexInit(&mutex.value,&mutexAttr.value,relocatedLabel));
 if(R[1]==3) scePthreadCondInit((u64*)0,0,relocatedLabel);
 if(R[1]==9) scePthreadCondInit((u64*)R[96],0,relocatedLabel);
 /* Source-backed static zero lazy initialization is exercised by actual signal. */
 status(scePthreadCondSignal(&condition.value)); R[95]=condition.value;
 status(scePthreadCondDestroy(&condition.value));
 status(scePthreadCondattrInit(&conditionAttr.value));
 status(scePthreadCondInit(&condition.value,&conditionAttr.value,relocatedLabel));
 status(scePthreadCondattrDestroy(&conditionAttr.value));
 controls(); R[30]=(u64)&condition.value; R[31]=condition.value; R[32]=(u64)&mutex.value; R[33]=mutex.value;
 /* Empty signal must not accumulate a token that returns a later wait. */
 status(posix_cond_signal(&condition.value)); status(scePthreadCondBroadcast(&condition.value));
 for(unsigned role=0;role<count;++role) { ids[role]=spawn(role); thread_yield(); }
 R[16]=(R[12] == ((1ULL<<count)-1) && R[13] == 0);
 R[17]=(u32)scePthreadCondDestroy(&condition.value);
 R[18]=(u32)scePthreadMutexDestroy(&mutex.value);
 status(scePthreadMutexLock(&mutex.value)); /* proves atomic release to actual owner */
 R[19]=R[13]==0; epoch=0x1020304050607080ULL;
 if(R[1]==2) for(;;) ++R[20]; /* bounded host budget permits provider cancellation */
 if(R[1]==11) {
  status(scePthreadCondSignal(&condition.value)); R[21]=R[13]==0; R[100]=1;
  for(;;) ++R[101]; /* owner700 must inherit waiter500 after condition transfer */
 }
 if(R[1]==4) *(volatile u64*)R[40]=0x1122334455667788ULL;
 if(R[1]==0 || R[1]==7 || R[1]==5 || R[1]==4) {
  status(scePthreadCondSignal(&condition.value)); thread_yield();
  R[21]=R[13]==0; R[22]=(u32)scePthreadCondDestroy(&condition.value);
  status(scePthreadMutexUnlock(&mutex.value)); thread_yield();
  if(R[1]==0 || R[1]==7) {
   R[23]=R[13]; R[24]=(u32)scePthreadMutexTrylock(&mutex.value);
   R[25]=(u32)scePthreadMutexUnlock(&mutex.value); R[26]=(u32)scePthreadMutexDestroy(&mutex.value);
  }
  joined(ids[0],0); R[27]=R[13];
  status(scePthreadMutexLock(&mutex.value));
 }
 status(R[1]==8 || R[1]==6 ? posix_cond_broadcast(&condition.value) : scePthreadCondBroadcast(&condition.value)); thread_yield();
 /* Every waiter now waits only for the mutex, so destroy succeeds before they reacquire. */
 R[28]=R[13]; R[29]=(u32)scePthreadCondDestroy(&condition.value);
 status(scePthreadMutexUnlock(&mutex.value));
 for(unsigned role=(R[1]==0 || R[1]==7 || R[1]==5 ? 1 : 0);role<count;++role) joined(ids[role],role);
 if(R[29]) status(scePthreadCondDestroy(&condition.value));
 R[34]=condition.value;
 status(scePthreadMutexDestroy(&mutex.value)); R[35]=mutex.value;
 status(scePthreadMutexattrDestroy(&mutexAttr.value));
 guards(&condition);guards(&mutex);guards(&mutexAttr);guards(&threadAttr);guards(&conditionAttr);
 R[0]=0x434f4e4457414954ULL;process_exit(0);
}
