/* Public synthetic linked x86 guest for issue #280 mutex/condition semantics; no vendor/title code. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int scePthreadMutexInit(u64*,const u64*,const char*);
extern int scePthreadMutexLock(u64*);
extern int scePthreadMutexTrylock(u64*);
extern int scePthreadMutexUnlock(u64*);
extern int scePthreadMutexDestroy(u64*);
extern int scePthreadMutexattrInit(u64*);
extern int scePthreadMutexattrSettype(u64*,int);
extern int scePthreadMutexattrDestroy(u64*);
extern int scePthreadCondInit(u64*,const u64*,const char*);
extern int scePthreadCondDestroy(u64*);
extern int scePthreadCondWait(u64*,u64*);
extern int scePthreadCondSignal(u64*);
extern int scePthreadCondBroadcast(u64*);
extern int scePthreadCondattrInit(u64*);
extern int scePthreadCondattrDestroy(u64*);
extern int posix_mutex_lock(u64*);
extern int posix_mutex_unlock(u64*);
extern int scePthreadMutexTimedlock(u64*,u32);
extern int scePthreadCondattrSetclock(u64*,int);
extern int posix_condattr_setclock(u64*,int);
extern int posix_cond_timedwait(u64*,u64*,const volatile u64*);
extern int thread_create(u64*,const u64*,void*(*)(void*),void*,const char*);
extern int thread_join(u64,void**);
extern void thread_yield(void);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
#define FUNC(name) ".type " #name ",@function\n"
__asm__(FUNC(scePthreadMutexInit) FUNC(scePthreadMutexLock) FUNC(scePthreadMutexTrylock)
 FUNC(scePthreadMutexUnlock) FUNC(scePthreadMutexDestroy) FUNC(scePthreadMutexattrInit)
 FUNC(scePthreadMutexattrSettype) FUNC(scePthreadMutexattrDestroy) FUNC(scePthreadCondInit)
 FUNC(scePthreadCondDestroy) FUNC(scePthreadCondWait) FUNC(scePthreadCondSignal)
 FUNC(scePthreadCondBroadcast) FUNC(scePthreadCondattrInit) FUNC(scePthreadCondattrDestroy)
 FUNC(posix_mutex_lock) FUNC(posix_mutex_unlock) FUNC(scePthreadMutexTimedlock)
 FUNC(scePthreadCondattrSetclock) FUNC(posix_condattr_setclock) FUNC(posix_cond_timedwait)
 FUNC(thread_create) FUNC(thread_join)
 FUNC(thread_yield) FUNC(thread_self) FUNC(process_exit));
EXPORT volatile u64 KernelMutexSemanticsReceipt[128];
#define R KernelMutexSemanticsReceipt
#define DONE 0x53454d414e544943ULL
#define PAYLOAD 0x1020304050607080ULL
static const char label[]="public-mutex-semantics";
static const char* volatile relocatedLabel=label;
static __thread u64 tlsInitial __attribute__((used,aligned(16)))=PAYLOAD;
static __thread u64 tlsZero[2] __attribute__((used));
/* Objects live in ordinary guest memory; each mode reuses the same addresses. */
static u64 mutex, condition, attribute, conditionAttribute;
static struct { u64 lock; u64 cond; } object;
static volatile u64 payload, done;
static void status(int code) { if(code) ++R[4]; }
static void settle(volatile u64* flag) { for(unsigned turn=0;!*flag && turn<64;++turn) thread_yield(); }

/* PLAT-05: 0 and 1 are lazy static initializers; only the stored sentinel decides "destroyed". */
static void staticInitializers(void) {
 mutex=0;
 R[10]=(u32)scePthreadMutexLock(&mutex); R[11]=mutex;
 status(scePthreadMutexUnlock(&mutex)); status(scePthreadMutexDestroy(&mutex)); R[12]=mutex;
 mutex=0; /* a fresh zeroed object reuses the destroyed object's address */
 R[13]=(u32)scePthreadMutexTrylock(&mutex); R[14]=mutex;
 status(scePthreadMutexUnlock(&mutex)); status(scePthreadMutexDestroy(&mutex));
 mutex=1; /* adaptive static initializer */
 R[15]=(u32)scePthreadMutexLock(&mutex); R[16]=mutex;
 R[17]=(u32)scePthreadMutexLock(&mutex); /* adaptive self relock reports deadlock */
 status(scePthreadMutexUnlock(&mutex)); status(scePthreadMutexDestroy(&mutex));
 R[18]=(u32)scePthreadMutexLock(&mutex); /* destroyed sentinel 2 */
 R[19]=(u32)scePthreadMutexDestroy(&mutex);
 condition=0;
 R[20]=(u32)scePthreadCondSignal(&condition); R[21]=condition;
 status(scePthreadCondDestroy(&condition)); R[22]=condition;
 condition=0;
 R[23]=(u32)scePthreadCondBroadcast(&condition); R[24]=condition;
 status(scePthreadCondDestroy(&condition));
 R[25]=(u32)scePthreadCondSignal(&condition); /* destroyed sentinel 1 */
}

/* PLAT-04: init overwrites, never EBUSY; the stale attribute type does not leak into the new object. */
static void initOverwrites(void) {
 status(scePthreadMutexattrInit(&attribute)); status(scePthreadMutexattrSettype(&attribute,2));
 R[10]=(u32)scePthreadMutexattrInit(&attribute); /* re-init without destroy */
 status(scePthreadMutexInit(&mutex,&attribute,relocatedLabel)); R[11]=mutex;
 status(scePthreadMutexLock(&mutex));
 R[12]=(u32)scePthreadMutexLock(&mutex); /* default error-check type, not the stale recursive one */
 status(scePthreadMutexUnlock(&mutex));
 R[13]=(u32)scePthreadMutexInit(&mutex,0,relocatedLabel); R[14]=mutex;
 status(scePthreadMutexLock(&mutex)); status(scePthreadMutexUnlock(&mutex));
 status(scePthreadMutexDestroy(&mutex)); status(scePthreadMutexattrDestroy(&attribute));
 status(scePthreadCondInit(&condition,0,relocatedLabel)); R[15]=condition;
 R[16]=(u32)scePthreadCondInit(&condition,0,relocatedLabel); R[17]=condition;
 status(scePthreadCondSignal(&condition)); status(scePthreadCondDestroy(&condition));
 status(scePthreadCondattrInit(&conditionAttribute));
 R[18]=(u32)scePthreadCondattrInit(&conditionAttribute);
 status(scePthreadCondattrDestroy(&conditionAttribute));
}

/* PLAT-06: a copied handle value names the same object (the handle is a pointer on hardware). */
static void* copyContender(void* argument) {
 (void)argument; R[40]=(u32)scePthreadMutexTrylock(&object.lock);
 status(scePthreadMutexLock(&object.lock)); R[41]=1;
 if(payload!=PAYLOAD) ++R[6];
 payload+=1; status(scePthreadMutexUnlock(&object.lock)); return (void*)0x2801;
}
static void* copyWaiter(void* argument) {
 (void)argument; u64 lock=object.lock, cond=object.cond; /* moved/copied struct members */
 status(scePthreadMutexLock(&lock)); R[42]=1;
 while(!done) R[43]=(u32)scePthreadCondWait(&cond,&lock);
 R[44]=(u32)scePthreadMutexTrylock(&object.lock); /* reacquired the shared mutex */
 status(scePthreadMutexUnlock(&lock)); return (void*)0x2802;
}
static void handleIdentity(void) {
 u64 ids[2]; void* result=0;
 status(scePthreadMutexInit(&object.lock,0,relocatedLabel)); status(scePthreadCondInit(&object.cond,0,relocatedLabel));
 u64 copy=object.lock;
 R[10]=(u32)scePthreadMutexLock(&copy);
 R[11]=(u32)scePthreadMutexTrylock(&object.lock); /* same mutex, already ours */
 status(thread_create(&ids[0],0,copyContender,0,relocatedLabel)); settle(&R[40]);
 for(unsigned turn=0;turn<4;++turn) thread_yield();
 R[12]=!R[41]; payload=PAYLOAD; /* contender must still be excluded */
 R[13]=(u32)scePthreadMutexUnlock(&copy);
 status(thread_join(ids[0],&result)); R[14]=(u64)result; R[15]=payload;
 R[16]=(u32)posix_mutex_lock(&copy); R[17]=(u32)posix_mutex_unlock(&copy);
 status(thread_create(&ids[1],0,copyWaiter,0,relocatedLabel)); settle(&R[42]);
 u64 lockCopy=object.lock, condCopy=object.cond;
 status(scePthreadMutexLock(&lockCopy)); done=1;
 R[18]=(u32)scePthreadCondSignal(&condCopy);
 status(scePthreadMutexUnlock(&object.lock));
 status(thread_join(ids[1],&result)); R[19]=(u64)result;
 u64 forged=0x1122334455667788ULL;
 R[20]=(u32)scePthreadMutexLock(&forged); R[21]=(u32)scePthreadCondSignal(&forged);
 status(scePthreadCondDestroy(&condCopy)); R[22]=condCopy; R[23]=object.cond;
 R[24]=(u32)scePthreadCondSignal(&object.cond); /* the original slot now names a destroyed object */
 status(scePthreadMutexDestroy(&object.lock));
}

/* PLAT-07: a thread that exits normally while owning a non-robust mutex leaves it owned. */
static void* exitingOwner(void* argument) {
 (void)argument; R[40]=(u32)scePthreadMutexLock(&mutex); return (void*)0x2803;
}
static void* blockedLocker(void* argument) {
 (void)argument; R[41]=1; R[42]=(u32)scePthreadMutexLock(&mutex); R[43]=1; return 0;
}
static void ownerExit(void) {
 u64 id; void* result=0;
 status(scePthreadMutexInit(&mutex,0,relocatedLabel)); status(scePthreadCondInit(&condition,0,relocatedLabel));
 status(thread_create(&id,0,exitingOwner,0,relocatedLabel)); status(thread_join(id,&result)); R[10]=(u64)result;
 R[11]=(u32)scePthreadMutexTrylock(&mutex);
 R[12]=(u32)scePthreadMutexUnlock(&mutex);
 R[13]=(u32)scePthreadMutexDestroy(&mutex);
 R[14]=(u32)scePthreadCondWait(&condition,&mutex);
 R[15]=(u32)posix_mutex_unlock(&mutex);
 status(thread_create(&id,0,blockedLocker,0,relocatedLabel)); settle(&R[41]);
 for(unsigned turn=0;turn<8;++turn) thread_yield();
 R[16]=!R[43]; /* the locker queues behind the exited owner instead of crashing or acquiring */
 R[17]=mutex;
}

/* PLAT-15: destroying the condition right after broadcast is legal; waiters reacquire the mutex only. */
static void* completionWaiter(void* argument) {
 const u64 role=(u64)argument;
 status(scePthreadMutexLock(&mutex)); R[40+role]=1;
 while(!done) R[42+role]=(u32)scePthreadCondWait(&condition,&mutex);
 R[44+role]=(u32)scePthreadMutexTrylock(&mutex); ++R[46];
 status(scePthreadMutexUnlock(&mutex)); return (void*)(0x2810+role);
}
static void destroyAfterBroadcast(void) {
 u64 ids[2]; void* result=0;
 status(scePthreadMutexInit(&mutex,0,relocatedLabel)); status(scePthreadCondInit(&condition,0,relocatedLabel));
 R[30]=condition;
 for(unsigned role=0;role<2;++role) {
  status(thread_create(&ids[role],0,completionWaiter,(void*)(u64)role,relocatedLabel)); settle(&R[40+role]);
 }
 status(scePthreadMutexLock(&mutex)); done=1;
 R[10]=(u32)scePthreadCondBroadcast(&condition);
 R[11]=(u32)scePthreadCondDestroy(&condition); R[12]=condition;
 /* The completion object's memory is reused while both waiters still need the mutex. */
 R[13]=(u32)scePthreadCondInit(&condition,0,relocatedLabel); R[14]=condition;
 R[15]=R[46];
 status(scePthreadMutexUnlock(&mutex));
 for(unsigned role=0;role<2;++role) {status(thread_join(ids[role],&result)); R[16+role]=(u64)result;}
 R[18]=(u32)scePthreadCondDestroy(&condition);
 /* Signal-one has the same contract. */
 status(scePthreadCondInit(&condition,0,relocatedLabel)); done=0; R[40]=0;
 status(thread_create(&ids[0],0,completionWaiter,0,relocatedLabel)); settle(&R[40]);
 status(scePthreadMutexLock(&mutex)); done=1;
 R[19]=(u32)scePthreadCondSignal(&condition);
 R[20]=(u32)scePthreadCondDestroy(&condition);
 condition=0x5a5a5a5a5a5a5a5aULL; /* freed memory scribbled before the waiter resumes */
 status(scePthreadMutexUnlock(&mutex));
 status(thread_join(ids[0],&result)); R[21]=(u64)result;
 status(scePthreadMutexDestroy(&mutex));
}

/* PLAT-23: a non-null attribute pointer that does not name a live attribute is EINVAL. */
static void badAttribute(void) {
 attribute=0; mutex=0x5eed;
 R[10]=(u32)scePthreadMutexInit(&mutex,&attribute,relocatedLabel); R[11]=mutex;
 status(scePthreadMutexattrInit(&attribute)); const u64 stale=attribute;
 status(scePthreadMutexattrDestroy(&attribute)); R[12]=attribute;
 R[13]=(u32)scePthreadMutexInit(&mutex,&attribute,relocatedLabel); R[14]=mutex;
 attribute=stale;
 R[15]=(u32)scePthreadMutexInit(&mutex,&attribute,relocatedLabel);
 attribute=0x1122334455667788ULL;
 R[16]=(u32)scePthreadMutexInit(&mutex,&attribute,relocatedLabel); R[17]=mutex;
 conditionAttribute=0; condition=0x5eed;
 R[18]=(u32)scePthreadCondInit(&condition,&conditionAttribute,relocatedLabel); R[19]=condition;
 status(scePthreadMutexattrInit(&attribute));
 R[20]=(u32)scePthreadMutexInit(&mutex,&attribute,relocatedLabel); /* live attribute control */
 status(scePthreadMutexDestroy(&mutex)); status(scePthreadMutexattrDestroy(&attribute));
}

/* scePthreadMutexTimedlock: relative microseconds; expiry returns SCE ETIMEDOUT, a release grants it. */
static void* timedHolder(void* argument) {
 (void)argument; status(scePthreadMutexLock(&mutex)); R[40]=1;
 while(!R[41]) thread_yield(); /* hold until the parent's timed lock has expired */
 status(scePthreadMutexUnlock(&mutex)); return (void*)0x2820;
}
static void* timedWaiter(void* argument) {
 (void)argument; R[42]=1;
 R[43]=(u32)scePthreadMutexTimedlock(&mutex,10000000u); R[45]=1; /* granted long before 10 s */
 R[44]=(u32)scePthreadMutexTrylock(&mutex); status(scePthreadMutexUnlock(&mutex)); return (void*)0x2821;
}
static void timedLock(void) {
 u64 id; void* result=0;
 status(scePthreadMutexInit(&mutex,0,relocatedLabel));
 R[10]=(u32)scePthreadMutexTimedlock(&mutex,1000);
 R[11]=(u32)scePthreadMutexTimedlock(&mutex,1000); /* error-check self relock */
 status(scePthreadMutexUnlock(&mutex));
 status(thread_create(&id,0,timedHolder,0,relocatedLabel)); settle(&R[40]);
 R[12]=(u32)scePthreadMutexTimedlock(&mutex,300000); /* 300 ms */
 R[13]=(u32)scePthreadMutexTimedlock(&mutex,0);
 R[41]=1; status(thread_join(id,&result)); R[14]=(u64)result;
 status(scePthreadMutexLock(&mutex));
 status(thread_create(&id,0,timedWaiter,0,relocatedLabel)); settle(&R[42]);
 for(unsigned turn=0;turn<4;++turn) thread_yield();
 R[15]=!R[45];
 status(scePthreadMutexUnlock(&mutex)); status(thread_join(id,&result)); R[16]=(u64)result;
 status(scePthreadMutexDestroy(&mutex));
 mutex=0; R[17]=(u32)scePthreadMutexTimedlock(&mutex,0); R[18]=mutex; /* lazy static init */
 status(scePthreadMutexUnlock(&mutex)); status(scePthreadMutexDestroy(&mutex));
}

/* Condition attribute clocks: absolute timed waits are measured on the condition's clock. */
static volatile u64 deadline[2];
static void* clockWaiter(void* argument) {
 (void)argument; status(scePthreadMutexLock(&mutex)); R[40]=1;
 R[41]=(u32)posix_cond_timedwait(&condition,&mutex,deadline); R[42]=1;
 status(scePthreadMutexUnlock(&mutex)); return (void*)0x2830;
}
static void conditionClock(void) {
 u64 id, realtime=0; void* result=0;
 status(scePthreadMutexInit(&mutex,0,relocatedLabel));
 status(scePthreadCondattrInit(&conditionAttribute));
 R[10]=(u32)scePthreadCondattrSetclock(&conditionAttribute,1); /* CPU-time clock cannot bound a wait */
 R[11]=(u32)posix_condattr_setclock(&conditionAttribute,99);
 R[19]=(u32)scePthreadCondattrSetclock(&conditionAttribute,5); /* CLOCK_UPTIME: libthr refuses it */
 R[12]=(u32)scePthreadCondattrSetclock(&conditionAttribute,4); /* CLOCK_MONOTONIC */
 status(scePthreadCondInit(&condition,&conditionAttribute,relocatedLabel));
 status(scePthreadCondattrDestroy(&conditionAttribute));
 /* The host seeds R[100..101] with its monotonic now: one hour later is in the
    future on the monotonic clock and long past on the realtime clock. */
 deadline[0]=R[100]+3600; deadline[1]=R[101];
 status(thread_create(&id,0,clockWaiter,0,relocatedLabel)); settle(&R[40]);
 for(unsigned turn=0;turn<8;++turn) thread_yield();
 R[13]=!R[42];
 status(scePthreadMutexLock(&mutex)); status(scePthreadCondSignal(&condition)); status(scePthreadMutexUnlock(&mutex));
 status(thread_join(id,&result)); R[14]=(u64)result;
 status(scePthreadMutexLock(&mutex));
 deadline[0]=R[100]; deadline[1]=R[101];
 R[15]=(u32)posix_cond_timedwait(&condition,&mutex,deadline); /* already past on the monotonic clock */
 deadline[0]=R[100]+3600;
 R[16]=(u32)posix_cond_timedwait(&realtime,&mutex,deadline); /* default realtime condition */
 R[17]=(u32)scePthreadMutexTrylock(&mutex);
 status(scePthreadMutexUnlock(&mutex));
 status(scePthreadCondDestroy(&condition)); status(scePthreadCondDestroy(&realtime));
 status(scePthreadMutexDestroy(&mutex));
 status(scePthreadCondattrInit(&conditionAttribute));
 R[18]=(u32)posix_condattr_setclock(&conditionAttribute,0);
 status(scePthreadCondattrDestroy(&conditionAttribute));
}

EXPORT int _start(void) {
 R[7]=thread_self();
 switch(R[1]) {
 case 1: staticInitializers(); break;
 case 2: initOverwrites(); break;
 case 3: handleIdentity(); break;
 case 4: ownerExit(); break;
 case 5: destroyAfterBroadcast(); break;
 case 6: badAttribute(); break;
 case 7: timedLock(); break;
 case 8: conditionClock(); break;
 default: ++R[4];
 }
 R[0]=DONE; process_exit(0);
}
