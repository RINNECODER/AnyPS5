/* Public linked x86 guest; qualified libc static shape, no vendor execution. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int posix_mutex_lock(u64*);
extern int posix_mutex_unlock(u64*);
extern int posix_cond_wait(u64*,u64*);
extern int posix_cond_broadcast(u64*);
extern int scePthreadMutexInit(u64*,const u64*,const char*);
extern int scePthreadMutexDestroy(u64*);
extern int scePthreadMutexTrylock(u64*);
extern int scePthreadCondDestroy(u64*);
extern int thread_create(u64*,const u64*,void*(*)(void*),void*,const char*);
extern int thread_join(u64,void**);
extern void thread_yield(void);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
#define FUNC(name) ".type " #name ",@function\n"
__asm__(FUNC(posix_mutex_lock) FUNC(posix_mutex_unlock) FUNC(posix_cond_wait)
 FUNC(posix_cond_broadcast) FUNC(scePthreadMutexInit) FUNC(scePthreadMutexDestroy)
 FUNC(scePthreadMutexTrylock) FUNC(scePthreadCondDestroy) FUNC(thread_create)
 FUNC(thread_join) FUNC(thread_yield) FUNC(thread_self) FUNC(process_exit));
EXPORT volatile u64 KernelStaticMutexReceipt[96];
#define R KernelStaticMutexReceipt
#define CANARY 0x1badc0ffeef00d55ULL
#define PAYLOAD 0x1020304050607080ULL
/* Both actual static slots start as adjacent eight-byte zero BSS words. */
static struct { u64 before, mutex, condition, after; } shared;
static const char label[]="public-libc-static-mutex";
static const char* volatile relocatedLabel=label;
static __thread u64 tlsInitial __attribute__((used,aligned(16)))=PAYLOAD;
static __thread u64 tlsZero[2] __attribute__((used));
static volatile u64 payload;
static void status(int code) { if(code) ++R[4]; }
static void guard(void) { if(shared.before!=CANARY || shared.after!=CANARY) ++R[5]; }
static void* waiter(void* arg) {
 const u64 role=(u64)arg; R[8+role]=thread_self(); R[12]|=1ULL<<role;
 status(posix_mutex_lock(&shared.mutex)); R[13]|=1ULL<<role;
 if(shared.mutex!=R[31] || payload!=PAYLOAD) ++R[6];
 R[14]|=1ULL<<role;
 R[40+role]=(u32)posix_cond_wait(&shared.condition,&shared.mutex);
 if(shared.mutex!=R[31] || shared.condition!=R[32] || payload!=PAYLOAD+1+role) ++R[6];
 R[15]|=1ULL<<role; R[64+R[18]]=role+1; ++R[18]; ++payload;
 R[42+role]=(u32)scePthreadMutexTrylock(&shared.mutex);
 R[44+role]=(u32)posix_mutex_lock(&shared.mutex); /* default nonrecursive self deadlock */
 if(role==0) thread_yield(); /* real owner remains exclusive while parent runs */
 status(posix_mutex_unlock(&shared.mutex)); guard(); return (void*)(0x1200+role);
}
static void identities(void) {
 u64 copied=shared.mutex, forged=0x1122334455667788ULL;
 /* A copied handle value names the same mutex (PLAT-06): relock reports deadlock,
    unlock releases the original, which is then re-owned through its own slot. */
 R[50]=(u32)posix_mutex_lock(&copied); R[51]=(u32)posix_mutex_unlock(&copied);
 R[87]=(u32)posix_mutex_lock(&shared.mutex);
 R[52]=(u32)posix_mutex_lock(&forged); R[53]=(u32)posix_mutex_unlock(&forged);
 R[54]=copied; R[55]=forged;
}
EXPORT int _start(void) {
 u64 ids[2]; R[7]=thread_self(); shared.before=shared.after=CANARY;
 R[30]=(u64)&shared.mutex; R[35]=(u64)&shared.condition;
 R[36]=shared.mutex; R[37]=shared.condition;
 R[38]=(u32)posix_mutex_unlock(&shared.mutex); /* zero unowned must not initialize */
 R[39]=shared.mutex;
 status(posix_mutex_lock(&shared.mutex)); R[31]=shared.mutex; payload=PAYLOAD;
 R[56]=(u32)posix_mutex_lock(&shared.mutex);
 R[57]=(u32)scePthreadMutexTrylock(&shared.mutex);
 R[58]=(u32)scePthreadMutexDestroy(&shared.mutex);
 identities();
 shared.mutex=0; R[77]=(u32)posix_mutex_lock(&shared.mutex);
 R[78]=(u32)posix_mutex_unlock(&shared.mutex); R[79]=shared.mutex; shared.mutex=R[31]; guard();
 if(R[1]==1) posix_mutex_lock((u64*)0);
 if(R[1]==2) posix_mutex_lock((u64*)R[90]);
 if(R[1]==4) posix_mutex_lock((u64*)0x12345000ULL);
 if(R[1]==6) posix_cond_wait(&shared.condition,(u64*)0);
 if(R[1]==7) posix_cond_wait((u64*)R[90],&shared.mutex);
 if(R[1]==9) posix_cond_wait(&shared.condition,(u64*)0x12345000ULL);
 if(R[1]==10) posix_cond_wait((u64*)0,&shared.mutex);
 if(R[1]==8 || R[1]==11) {
  u64 forged=0x1122334455667788ULL;
  if(R[1]==8) status(posix_mutex_unlock(&shared.mutex));
  R[82]=(u32)posix_cond_wait(&shared.condition,R[1]==8 ? &shared.mutex : &forged);
  R[83]=shared.condition; R[84]=shared.mutex;
  if(R[1]==8) status(posix_mutex_lock(&shared.mutex));
  status(posix_mutex_unlock(&shared.mutex)); guard(); R[0]=0x524f4c4c4241434bULL; process_exit(0);
 }
 for(unsigned role=0;role<2;++role) {
  status(thread_create(&ids[role],0,waiter,(void*)(u64)role,relocatedLabel)); thread_yield();
 }
 for(unsigned turn=0;R[12]!=3 && turn<8;++turn) {++R[85];thread_yield();}
 R[16]=(R[12]==3 && R[13]==0 && R[14]==0 && R[15]==0);
 status(posix_mutex_unlock(&shared.mutex));
 /* The reserved owner resumes on its own scheduler turn; observe both real waits. */
 for(unsigned turn=0;R[14]!=3 && turn<8;++turn) {++R[86];thread_yield();}
 R[17]=(R[13]==3 && R[14]==3 && R[15]==0);
 status(posix_mutex_lock(&shared.mutex)); R[32]=shared.condition; R[33]=shared.mutex;
 R[59]=(u32)scePthreadMutexDestroy(&shared.mutex);
 R[60]=(u32)scePthreadCondDestroy(&shared.condition);
 if(R[1]==3) for(;;) ++R[89]; /* bounded host cancellation before wake */
 payload=PAYLOAD+1;
 if(R[1]==5) { /* synthetic held-owner control: wake transfer cannot return yet */
  status(posix_cond_broadcast(&shared.condition)); thread_yield(); R[19]=R[15];
  status(posix_mutex_unlock(&shared.mutex)); thread_yield();
 } else { /* libc release call site unlocks before broadcasting its condition */
  status(posix_mutex_unlock(&shared.mutex)); status(posix_cond_broadcast(&shared.condition));
  thread_yield(); R[19]=R[15];
 }
 R[20]=R[15];
 R[21]=(u32)scePthreadMutexTrylock(&shared.mutex);
 R[22]=(u32)posix_mutex_unlock(&shared.mutex);
 R[23]=(u32)scePthreadMutexDestroy(&shared.mutex);
 for(unsigned role=0;role<2;++role) {
  void* result=(void*)0xccccccccccccccccULL; status(thread_join(ids[role],&result)); R[46+role]=(u64)result;
 }
 R[24]=payload; R[25]=shared.mutex; R[26]=shared.condition;
 {u64 copied=shared.condition; R[80]=(u32)posix_cond_broadcast(&copied); R[81]=copied;}
 R[61]=(u32)posix_cond_wait(&shared.condition,&shared.mutex); /* unowned */
 status(scePthreadCondDestroy(&shared.condition)); R[27]=shared.condition;
 R[62]=(u32)posix_cond_broadcast(&shared.condition);
 shared.condition=0; R[63]=(u32)posix_cond_broadcast(&shared.condition); R[69]=shared.condition;
 status(scePthreadMutexDestroy(&shared.mutex)); R[28]=shared.mutex;
 R[70]=(u32)posix_mutex_lock(&shared.mutex); R[71]=(u32)posix_mutex_unlock(&shared.mutex);
 /* A zeroed slot is a static initializer even where an object was destroyed (PLAT-05). */
 shared.mutex=0; R[72]=(u32)posix_mutex_lock(&shared.mutex); R[73]=shared.mutex;
 R[88]=(u32)posix_mutex_unlock(&shared.mutex);
 /* Explicit init may deliberately recreate a destroyed slot; old token cannot. */
 shared.mutex=2; status(scePthreadMutexInit(&shared.mutex,0,relocatedLabel)); R[34]=shared.mutex;
 shared.mutex=R[31]; R[74]=(u32)posix_mutex_lock(&shared.mutex); R[75]=(u32)posix_mutex_unlock(&shared.mutex);
 R[76]=shared.mutex; shared.mutex=R[34];
 status(posix_mutex_lock(&shared.mutex)); status(posix_mutex_unlock(&shared.mutex));
 status(scePthreadMutexDestroy(&shared.mutex)); R[29]=shared.mutex;
 guard(); R[0]=0x535441544d555458ULL; process_exit(0);
}
