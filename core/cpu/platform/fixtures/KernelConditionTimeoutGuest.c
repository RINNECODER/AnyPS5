/* Public synthetic linked x86 timed-wait contract; no private/title execution. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
#ifdef RELATIVE_TIMEOUT
extern int timed_wait(u64*,u64*,u32);
#define TIME(role) ((u32)R[80+(role)*2])
/* Deliberately retain raw upper32 in assembly to test the selected u32 ABI. */
#define ABI_TIME(role) R[80+(role)*2]
#else
extern int timed_wait(u64*,u64*,const volatile u64*);
#define TIME(role) (&R[80+(role)*2])
#define ABI_TIME(role) ((u64)TIME(role))
#endif
extern int scePthreadMutexInit(u64*,const u64*,const char*);
extern int scePthreadMutexLock(u64*);
extern int scePthreadMutexUnlock(u64*);
extern int scePthreadMutexTrylock(u64*);
extern int scePthreadMutexDestroy(u64*);
extern int scePthreadMutexattrInit(u64*);
extern int scePthreadMutexattrSettype(u64*,int);
extern int scePthreadMutexattrDestroy(u64*);
extern int scePthreadCondInit(u64*,const u64*,const char*);
extern int scePthreadCondSignal(u64*);
extern int scePthreadCondDestroy(u64*);
extern int thread_create(u64*,const u64*,void*(*)(void*),void*,const char*);
extern int thread_join(u64,void**);
extern void thread_yield(void);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
#define FUNC(name) ".type " #name ",@function\n"
__asm__(FUNC(timed_wait) FUNC(scePthreadMutexInit) FUNC(scePthreadMutexLock)
 FUNC(scePthreadMutexUnlock) FUNC(scePthreadMutexTrylock) FUNC(scePthreadMutexDestroy)
 FUNC(scePthreadMutexattrInit) FUNC(scePthreadMutexattrSettype) FUNC(scePthreadMutexattrDestroy)
 FUNC(scePthreadCondInit) FUNC(scePthreadCondSignal) FUNC(scePthreadCondDestroy)
 FUNC(thread_create) FUNC(thread_join) FUNC(thread_yield) FUNC(thread_self) FUNC(process_exit));
EXPORT volatile u64 KernelConditionTimeoutReceipt[128];
#define R KernelConditionTimeoutReceipt
#define CANARY 0x1badc0ffeef00d55ULL
#define PAYLOAD 0x1020304050607080ULL
struct Slot { u64 before,value,after; };
static struct Slot mutex,condition,attribute;
static const char label[]="public-condition-timeout";
static const char* volatile relocatedLabel=label;
static __thread u64 initial __attribute__((used,aligned(16)))=PAYLOAD;
static __thread u64 zeros[2] __attribute__((used));
static volatile u64 payload;
static void status(int result) { if(result) ++R[4]; }
static void guard(const struct Slot* s) { if(s->before!=CANARY || s->after!=CANARY) ++R[5]; }
/* A true timed PLT call retains third arg and suspended return/stack/callee-saved ABI. */
__attribute__((visibility("hidden"))) void timed_wait_abi(u64*,u64*,u64,volatile u64*);
__asm__(".text\n.hidden timed_wait_abi\n.type timed_wait_abi,@function\ntimed_wait_abi:\n"
 "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $24,%rsp\n"
 "mov %rcx,(%rsp)\nmovabs $0x1234567890abcdef,%rax\n"
 "mov %rax,%rbx\nmov %rax,%rbp\nmov %rax,%r12\nmov %rax,%r13\nmov %rax,%r14\nmov %rax,%r15\n"
 "mov %rax,16(%rsp)\nlea -8(%rsp),%rax\nmov %rax,320(%rcx)\n"
 "lea 1f(%rip),%rax\nmov %rax,328(%rcx)\ncall timed_wait@PLT\n1:\n"
 "mov (%rsp),%rdi\nmov %eax,%eax\nmov %rax,336(%rdi)\n"
 "movabs $0x1234567890abcdef,%rdx\nxor %eax,%eax\n"
 "mov %rbx,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rbp,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r12,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r13,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r14,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r15,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov 16(%rsp),%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rax,344(%rdi)\n"
 "mov %rsp,%rax\nsub 320(%rdi),%rax\nsub $8,%rax\nmov %rax,352(%rdi)\n"
 "add $24,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
 ".size timed_wait_abi,.-timed_wait_abi\n");
static void* waiter(void* argument) {
 const u64 role=(u64)argument;R[8+role]=thread_self();
 status(scePthreadMutexLock(&mutex.value));
 if(R[1]==2) status(scePthreadMutexLock(&mutex.value));
 R[12]|=1ULL<<role;
 if(!role) timed_wait_abi(&condition.value,&mutex.value,ABI_TIME(role),R);
 else R[46+role]=(u32)timed_wait(&condition.value,&mutex.value,TIME(role));
 ++R[16+role];R[13]|=1ULL<<role;
 R[48+role]=(u32)scePthreadMutexTrylock(&mutex.value);
 if(R[1]==2) {
  /* Trylock itself adds one recursive level; undo it, then one original level. */
  status(scePthreadMutexUnlock(&mutex.value));status(scePthreadMutexUnlock(&mutex.value));
  R[104]=1;thread_yield();R[52]=(u32)scePthreadMutexTrylock(&mutex.value);
  status(scePthreadMutexUnlock(&mutex.value));
 }
 if((R[1]==1 || R[1]==3 || R[1]==4) && payload!=PAYLOAD) ++R[6];
 status(scePthreadMutexUnlock(&mutex.value));guard(&mutex);guard(&condition);
 return (void*)(0x1300+role);
}
#ifndef RELATIVE_TIMEOUT
static void* expiry_contender(void* argument) {
 (void)argument;status(scePthreadMutexLock(&mutex.value));R[106]=1;
 R[107]=(u32)scePthreadMutexTrylock(&mutex.value);
 status(scePthreadMutexUnlock(&mutex.value));return (void*)0x1370;
}
#endif
EXPORT int _start(void) {
 R[7]=thread_self();mutex.before=mutex.after=condition.before=condition.after=attribute.before=attribute.after=CANARY;
 status(scePthreadMutexattrInit(&attribute.value));
 status(scePthreadMutexattrSettype(&attribute.value,R[1]==2 ? 2 : 3));
 status(scePthreadMutexInit(&mutex.value,&attribute.value,relocatedLabel));
 R[30]=(u64)&condition.value;R[32]=(u64)&mutex.value;R[33]=mutex.value;
 if(R[1]==6 || R[1]==7) {
  status(scePthreadMutexLock(&mutex.value));
  struct Slot copy={CANARY,0,CANARY};u64 old=0;
  if(R[90]>=2 && R[90]<=4) {
   status(scePthreadCondInit(&condition.value,0,relocatedLabel));old=condition.value;
   copy.value=R[90]==3 ? 0x1122334455667788ULL : old;
   if(R[90]==4) {status(scePthreadCondDestroy(&condition.value));status(scePthreadCondInit(&condition.value,0,relocatedLabel));old=condition.value;condition.value=copy.value;}
  }
  R[31]=old ? old : condition.value;
  u64* cp=R[90]==1 || R[90]==4 ? &condition.value : (R[90]==2 || R[90]==3 ? &copy.value : (u64*)R[90]);
  u64 mutexCopy=R[91]==3 ? 0x1122334455667788ULL : mutex.value;
  u64* mp=R[91]==1 ? &mutex.value : (R[91]==2 || R[91]==3 ? &mutexCopy : (u64*)R[91]);
  if(R[92]==1) R[92]=(u64)&R[80];
#ifdef RELATIVE_TIMEOUT
  R[60]=(u32)timed_wait(cp,mp,(u32)R[80]);
#else
  if(R[1]==7) {
   u64 id=0;void* result=0;
   status(thread_create(&id,0,expiry_contender,0,relocatedLabel));thread_yield();R[108]=!R[106];
   timed_wait_abi(cp,mp,R[92],R);R[60]=R[42];
   /* Settle the real contender after a wrong early return; host still sees the
      original result and missing condition publication for a matching fault. */
   if(R[60]!=60) status(scePthreadMutexUnlock(&mutex.value));
   status(thread_join(id,&result));R[109]=(u64)result;
   if(R[60]!=60) status(scePthreadMutexLock(&mutex.value));
  } else R[60]=(u32)timed_wait(cp,mp,(const volatile u64*)R[92]);
#endif
  if(R[90]==4) condition.value=old;guard(&copy);
  R[61]=condition.value;R[62]=mutex.value;R[63]=(u32)scePthreadMutexTrylock(&mutex.value);
  status(scePthreadMutexUnlock(&mutex.value));
 } else {
  status(scePthreadCondInit(&condition.value,0,relocatedLabel));R[31]=condition.value;
  const unsigned count=R[1]==3 ? 2 : 1;u64 ids[2];
  status(scePthreadCondSignal(&condition.value)); /* no token for a future wait */
  for(unsigned role=0;role<count;++role) {
   status(thread_create(&ids[role],0,waiter,(void*)(u64)role,relocatedLabel));thread_yield();
  }
  R[14]=R[12]==((1ULL<<count)-1) && !R[13];
  status(scePthreadMutexLock(&mutex.value));R[15]=!R[13];payload=PAYLOAD;
  R[20]=(u32)scePthreadCondDestroy(&condition.value);R[21]=(u32)scePthreadMutexDestroy(&mutex.value);
  if(R[1]==5) for(;;) ++R[101];
  if(R[1]==1 || R[1]==3) { status(scePthreadCondSignal(&condition.value));thread_yield();R[18]=R[13]; }
  if(R[1]==4) { while(!R[100]) thread_yield();R[18]=R[13];R[22]=(u32)scePthreadCondDestroy(&condition.value); }
  status(scePthreadMutexUnlock(&mutex.value));
  if(R[1]==2) {while(!R[104]) thread_yield();R[53]=(u32)scePthreadMutexTrylock(&mutex.value);R[54]=(u32)scePthreadMutexUnlock(&mutex.value);}
  for(unsigned role=0;role<count;++role) {void* result=0;status(thread_join(ids[role],&result));R[56+role]=(u64)result;}
  R[23]=condition.value;R[24]=mutex.value;
 }
 status(scePthreadCondDestroy(&condition.value));R[34]=condition.value;
 status(scePthreadMutexDestroy(&mutex.value));R[35]=mutex.value;
 status(scePthreadMutexattrDestroy(&attribute.value));guard(&mutex);guard(&condition);guard(&attribute);
 R[0]=0x54494d4544434f4eULL;process_exit(0);
}
