/* Public synthetic guest. Real linked libkernel imports for rwlocks, once, TSD,
   SyncOnAddress, thread attributes/controls, module information and debug. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int thread_create(u64*,const u64*,void*(*)(void*),void*,const char*);
extern void thread_yield(void);
extern int thread_join(u64,void**);
extern u64 thread_self(void);

extern __attribute__((noreturn)) void process_exit(int);
extern int attr_init(u64*);
extern int attr_destroy(u64*);
extern int attr_policy(u64*,int);
extern int rw_init(u64*,const u64*,const char*);
extern int rw_destroy(u64*);
extern int rw_wrlock(u64*);
extern int rw_rdlock(u64*);
extern int rw_trywrlock(u64*);
extern int rw_tryrdlock(u64*);
extern int rw_timedwrlock(u64*,u32);
extern int rw_timedrdlock(u64*,u32);
extern int rw_unlock(u64*);
extern int once(u64*,void(*)(void));
extern int key_create(int*,void(*)(void*));
extern int key_delete(int);
extern int set_specific(int,const void*);
extern void* get_specific(int);
extern int sync_wait(volatile u32*,u32,const u32*,const char*);
extern int sync_wait32(volatile u32*,u32,const u32*,const char*);
extern int sync_wait64(volatile u64*,u64,const u32*,const char*);
extern int sync_wake(volatile void*,int);
extern int attr_setstacksize(u64*,u64);
extern int attr_getstacksize(const u64*,u64*);
extern int attr_setguardsize(u64*,u64);
extern int attr_getguardsize(const u64*,u64*);
extern int attr_setdetachstate(u64*,int);
extern int attr_getdetachstate(const u64*,int*);
extern int attr_setaffinity(u64*,u64);
extern int attr_getaffinity(const u64*,u64*);
extern int attr_getschedpolicy(const u64*,int*);
extern int attr_getinheritsched(const u64*,int*);
extern int attr_setstack(u64*,void*,u64);
extern int attr_get(u64,u64*);
extern int thread_detach(u64);
extern int thread_getname(u64,char*);
extern int thread_rename(u64,const char*);
extern int thread_setprio(u64,int);
extern int thread_getprio(u64,int*);
extern int thread_setaffinity(u64,u64);
extern int thread_getaffinity(u64,u64*);
extern int thread_cancel(u64);
extern void thread_testcancel(void);
extern int thread_getthreadid(void);
extern int module_from_addr(u64,int,void*);
extern int module_for_unwind(u64,int,void*);
extern int debug_raise(u32,u32);
extern int debug_raise_release(u32,u32);
extern int debug_cpp_info(const void*);
extern int load_start(const char*,u64,const void*,u32,const void*,int*);
extern int stop_unload(int,u64,const void*,u32,const void*,int*);
extern int module_dlsym(int,const char*,u64*);
extern int is_trinity(void);
extern int uuid_create(u32*);
extern int atexit_inc(const void*);
extern int atexit_dec(const void*);
extern int get_pid(void);
extern int psem_init(u64*,int,u32);
extern int psem_post(u64*);
extern int psem_wait(u64*);
extern int psem_destroy(u64*);
extern int psem_timedwait(u64*,const long*);
extern int get_rusage(int,void*);
extern u64 sig_set(int,u64);
extern int sig_mask(int,const u32*,u32*);
extern int phdr_match(const void*,const void*);
extern int* err_loc(void);
extern int clock_now(int,long*);
#define FUNC(n) ".type " #n ",@function\n"
__asm__(FUNC(thread_create) FUNC(thread_yield) FUNC(thread_join) FUNC(thread_self) FUNC(process_exit)
 FUNC(attr_init) FUNC(attr_destroy) FUNC(attr_policy) FUNC(rw_init) FUNC(rw_destroy) FUNC(rw_wrlock) FUNC(rw_rdlock)
 FUNC(rw_trywrlock) FUNC(rw_tryrdlock) FUNC(rw_timedwrlock) FUNC(rw_timedrdlock) FUNC(rw_unlock) FUNC(once)
 FUNC(key_create) FUNC(key_delete) FUNC(set_specific) FUNC(get_specific) FUNC(sync_wait) FUNC(sync_wait32)
 FUNC(sync_wait64) FUNC(sync_wake) FUNC(attr_setstacksize) FUNC(attr_getstacksize) FUNC(attr_setguardsize)
 FUNC(attr_getguardsize) FUNC(attr_setdetachstate) FUNC(attr_getdetachstate) FUNC(attr_setaffinity)
 FUNC(attr_getaffinity) FUNC(attr_getschedpolicy) FUNC(attr_getinheritsched) FUNC(attr_setstack) FUNC(attr_get)
 FUNC(thread_detach) FUNC(thread_getname) FUNC(thread_rename) FUNC(thread_setprio) FUNC(thread_getprio)
 FUNC(thread_setaffinity) FUNC(thread_getaffinity) FUNC(thread_cancel) FUNC(thread_testcancel)
 FUNC(thread_getthreadid) FUNC(module_from_addr) FUNC(module_for_unwind) FUNC(debug_raise)
 FUNC(debug_raise_release) FUNC(debug_cpp_info) FUNC(load_start) FUNC(stop_unload) FUNC(module_dlsym)
 FUNC(is_trinity) FUNC(uuid_create) FUNC(atexit_inc) FUNC(atexit_dec) FUNC(get_pid) FUNC(psem_init) FUNC(psem_post)
 FUNC(psem_wait) FUNC(psem_destroy) FUNC(psem_timedwait) FUNC(get_rusage) FUNC(sig_set) FUNC(sig_mask)
 FUNC(phdr_match) FUNC(err_loc) FUNC(clock_now));
EXPORT volatile u64 KernelThreadExtrasReceipt[128];
#define R KernelThreadExtrasReceipt
static const char label[]="public-thread-extras-fixture";
static const char* volatile relocatedLabel=label;
static __thread u64 tlsInitial __attribute__((used,aligned(16)))=0x1020304050607080ULL;
static __thread u64 tlsZero[2] __attribute__((used));
static void status(int code) {if(code) ++R[4];}
static void trace(u64 value) {u64 n=R[5];if(n<16){R[100+n]=value;R[5]=n+1;}else ++R[4];}
static u64 spawn(void*(*entry)(void*),void* argument,const u64* attr,const char* name) {
 u64 id=0;status(thread_create(&id,attr,entry,argument,name));return id;
}
static void settle(unsigned count) {for(unsigned i=0;i<count;++i)thread_yield();}
static __attribute__((noreturn)) void finish(void) {R[0]=0x5854524153505448ULL;process_exit(R[4]?1:0);}

/* ---- mode 1: rwlock ---- */
static u64 rw, rw2;
static void* writer(void* unused) {(void)unused;R[30]=1;status(rw_wrlock(&rw));trace(3);status(rw_unlock(&rw));return 0;}
static void* reader(void* unused) {(void)unused;R[31]=1;status(rw_rdlock(&rw));trace(4);status(rw_unlock(&rw));return 0;}
static void* timed(void* unused) {
 (void)unused;R[23]=(u32)rw_timedrdlock(&rw,2000);R[24]=(u32)rw_timedwrlock(&rw,2000);return 0;
}
static void rwlocks(void) {
 rw=0;
 R[10]=(u32)rw_rdlock(&rw);R[11]=(u32)rw_rdlock(&rw);R[12]=(u32)rw_trywrlock(&rw);
 R[13]=(u32)rw_unlock(&rw);R[14]=(u32)rw_unlock(&rw);R[15]=(u32)rw_unlock(&rw);
 R[16]=(u32)rw_wrlock(&rw);R[17]=(u32)rw_rdlock(&rw);R[18]=(u32)rw_tryrdlock(&rw);R[19]=(u32)rw_wrlock(&rw);
 R[20]=(u32)rw_destroy(&rw);R[21]=(u32)rw_unlock(&rw);R[9]=rw;
 status(rw_rdlock(&rw));
 const u64 w=spawn(writer,0,0,relocatedLabel);while(!R[30])thread_yield();
 const u64 r=spawn(reader,0,0,relocatedLabel);while(!R[31])thread_yield();
 settle(2);
 R[22]=(u32)rw_tryrdlock(&rw);status(rw_unlock(&rw));
 trace(2);status(rw_unlock(&rw));
 status(thread_join(w,0));status(thread_join(r,0));
 status(rw_wrlock(&rw));
 const u64 t=spawn(timed,0,0,relocatedLabel);status(thread_join(t,0));
 status(rw_unlock(&rw));
 R[25]=(u32)rw_destroy(&rw);R[26]=rw;R[27]=(u32)rw_rdlock(&rw);
 R[28]=(u32)rw_init(&rw2,0,relocatedLabel);R[29]=(u32)rw_destroy(&rw2);
}

/* ---- mode 2: once ---- */
static u64 onceControl[2], onceInvalid[2];
static volatile u64 initCalls;
static void initialize(void) {++initCalls;trace(10);settle(2);trace(11);}
static void* onceWorker(void* unused) {(void)unused;R[40]=(u32)once(onceControl,initialize);trace(12);return 0;}
static void onces(void) {
 const u64 w=spawn(onceWorker,0,0,relocatedLabel);
 thread_yield();
 R[41]=(u32)once(onceControl,initialize);trace(13);
 status(thread_join(w,0));
 R[42]=(u32)once(onceControl,initialize);
 R[43]=(u32)once(0,initialize);R[44]=(u32)once(onceInvalid,0);
 R[45]=initCalls;R[46]=(u32)onceControl[0];
}

/* ---- mode 3: TSD ---- */
static int key1, key2, key3;
static volatile u64 calls1, calls3;
static void destructor1(void* value) {R[50+calls1]=(u64)value;if(++calls1==1)status(set_specific(key1,(void*)0x2222));}
static void destructor3(void* value) {(void)value;++calls3;status(set_specific(key3,(void*)0x4444));}
static void* tsdWorker(void* unused) {
 (void)unused;status(set_specific(key1,(void*)0x1111));status(set_specific(key2,(void*)0x3333));
 R[60]=(u64)get_specific(key1);R[61]=(u64)get_specific(key2);return 0;
}
static void* tsdLooper(void* unused) {(void)unused;status(set_specific(key3,(void*)0x4444));return 0;}
static void keys(void) {
 R[10]=(u32)key_create(&key1,destructor1);R[11]=(u32)key_create(&key2,0);R[12]=(u32)key1;R[13]=(u32)key2;
 R[14]=(u32)set_specific(key1,(void*)0xabc);R[15]=(u64)get_specific(key1);
 status(thread_join(spawn(tsdWorker,0,0,relocatedLabel),0));
 R[16]=(u64)get_specific(key1);R[62]=calls1;
 R[17]=(u32)key_delete(key1);R[18]=(u64)get_specific(key1);R[19]=(u32)set_specific(key1,(void*)1);
 R[20]=(u32)key_delete(key1);
 int created[256];unsigned count=0;int code=0;
 for(;count<256;++count){code=key_create(&created[count],0);if(code)break;}
 R[21]=count;R[22]=(u32)code;
 for(unsigned i=0;i<count;++i)status(key_delete(created[i]));
 status(key_create(&key3,destructor3));
 status(thread_join(spawn(tsdLooper,0,0,relocatedLabel),0));R[23]=calls3;
 R[24]=(u32)key_delete(0);R[25]=(u64)get_specific(9999);
}

/* ---- mode 4: SyncOnAddress ---- */
static volatile u32 word;
static volatile u64 wide;
static void* addressWaiter(void* unused) {(void)unused;R[30]=1;R[11]=(u32)sync_wait(&word,0,0,relocatedLabel);trace(2);return 0;}
static void* wideWaiter(void* unused) {(void)unused;R[31]=1;R[15]=(u32)sync_wait64(&wide,0,0,0);trace(4);return 0;}
static void addresses(void) {
 R[10]=(u32)sync_wait(&word,5,0,0);
 const u64 w=spawn(addressWaiter,0,0,relocatedLabel);while(!R[30])thread_yield();settle(1);
 word=7;trace(1);R[12]=(u32)sync_wake(&word,1);status(thread_join(w,0));
 const u32 timeout=2000;R[13]=(u32)sync_wait32(&word,7,&timeout,0);
 R[14]=(u32)sync_wait64(&wide,1,0,0);
 const u64 v=spawn(wideWaiter,0,0,relocatedLabel);while(!R[31])thread_yield();settle(1);
 trace(3);status(sync_wake(&wide,0x7fffffff));status(thread_join(v,0));
 R[16]=(u32)sync_wake(&word,-1);R[17]=(u32)sync_wait((volatile u32*)((volatile char*)&word+1),0,0,0);
}

/* ---- mode 5: thread attributes and controls ---- */
static u64 attr, detachedAttr, getAttr, stackAttr, garbageAttr;
static char ownStack[65536] __attribute__((aligned(16)));
static void* deep(void* unused) {
 (void)unused;volatile char big[3<<20];big[0]=1;big[sizeof(big)-1]=2;R[30]=big[0]+big[sizeof(big)-1];
 R[31]=1;thread_yield();R[32]=1;return 0;
}
static void* parked(void* unused) {(void)unused;R[33]=1;while(!R[39])thread_yield();return 0;}
static void* canceled(void* unused) {(void)unused;for(;;){thread_yield();thread_testcancel();}}
static void* ownStackWorker(void* unused) {
 (void)unused;volatile char local=1;const u64 at=(u64)&local;
 R[48]=at>=(u64)ownStack && at<(u64)ownStack+sizeof(ownStack);R[49]=(u64)thread_getthreadid();return 0;
}
static int same(const char* a,const char* b) {for(;*a||*b;++a,++b)if(*a!=*b)return 0;return 1;}
static void controls(void) {
 status(attr_init(&attr));
 R[10]=(u32)attr_setstacksize(&attr,4<<20);status(attr_getstacksize(&attr,(u64*)&R[11]));
 R[12]=(u32)attr_setstacksize(&attr,100);
 status(attr_setguardsize(&attr,8192));status(attr_getguardsize(&attr,(u64*)&R[13]));
 int value=-1;status(attr_setdetachstate(&attr,1));status(attr_getdetachstate(&attr,&value));R[14]=(u32)value;
 R[15]=(u32)attr_setdetachstate(&attr,5);
 status(attr_setaffinity(&attr,3));status(attr_getaffinity(&attr,(u64*)&R[16]));
 R[17]=(u32)attr_policy(&attr,2);value=-1;status(attr_getschedpolicy(&attr,&value));R[18]=(u32)value;
 value=-1;status(attr_getinheritsched(&attr,&value));R[19]=(u32)value;
 R[20]=(u32)attr_init(&attr);status(attr_destroy(&attr));
 /* Detached thread with a 4 MiB stack touches 3 MiB below its top. */
 status(attr_init(&detachedAttr));status(attr_setdetachstate(&detachedAttr,1));
 status(attr_setstacksize(&detachedAttr,4<<20));
 const u64 d=spawn(deep,0,&detachedAttr,relocatedLabel);status(attr_destroy(&detachedAttr));
 while(!R[31])thread_yield();
 R[21]=(u32)thread_join(d,0);
 while(!R[32])thread_yield();settle(2);
 R[22]=(u32)thread_join(d,0);
 /* Joinable thread: detach, name, priority, affinity, attr_get. */
 const u64 j=spawn(parked,0,0,"extras-worker");while(!R[33])thread_yield();
 char name[32];R[25]=(u32)thread_getname(j,name);R[26]=same(name,"extras-worker");
 status(thread_rename(j,"renamed"));status(thread_getname(j,name));R[27]=same(name,"renamed");
 R[28]=(u32)thread_setprio(j,500);value=0;status(thread_getprio(j,&value));R[29]=(u32)value;
 R[34]=(u32)thread_setprio(j,5);
 status(thread_setaffinity(j,5));status(thread_getaffinity(j,(u64*)&R[35]));
 R[23]=(u32)thread_detach(j);R[24]=(u32)thread_detach(j);
 status(attr_init(&getAttr));R[36]=(u32)attr_get(j,&getAttr);
 value=-1;status(attr_getdetachstate(&getAttr,&value));R[37]=(u32)value;
 status(attr_getaffinity(&getAttr,(u64*)&R[38]));status(attr_getstacksize(&getAttr,(u64*)&R[50]));
 status(attr_destroy(&getAttr));R[39]=1;settle(3);
 R[40]=(u32)thread_getthreadid();
 R[41]=(u32)thread_setprio(0x1234,500);R[42]=(u32)thread_join(0x1234,0);R[51]=(u32)thread_join(thread_self(),0);
 garbageAttr=0x5151515151515151ULL;u64 unused=0;R[43]=(u32)thread_create(&unused,&garbageAttr,parked,0,0);
 const u64 c=spawn(canceled,0,0,relocatedLabel);settle(2);
 R[44]=(u32)thread_cancel(c);void* result=0;status(thread_join(c,&result));R[45]=(u64)result;
 status(attr_init(&stackAttr));R[46]=(u32)attr_setstack(&stackAttr,ownStack,sizeof(ownStack));
 const u64 s=spawn(ownStackWorker,0,&stackAttr,relocatedLabel);status(thread_join(s,0));
 status(attr_destroy(&stackAttr));
}

/* ---- mode 6: module information ---- */
static u64 moduleInfo[0x1a8/8], unwindInfo[0x130/8];
static void modules(void) {
 moduleInfo[0]=0x1a8;
 R[10]=(u32)module_from_addr((u64)&finish,2,moduleInfo);
 R[11]=moduleInfo[0x148/8];R[12]=moduleInfo[0x150/8];R[13]=(u32)moduleInfo[0x1a0/8];
 R[14]=moduleInfo[0x160/8];R[15]=moduleInfo[1];R[16]=moduleInfo[2];R[19]=moduleInfo[0x158/8];
 R[17]=(u32)module_for_unwind((u64)&finish,1,unwindInfo);
 R[18]=unwindInfo[0x108/8]==R[11] && unwindInfo[0x110/8]==R[12] && unwindInfo[0x120/8]==R[14] &&
  unwindInfo[0x118/8]!=0 && unwindInfo[1]==R[15];
 R[20]=(u32)module_from_addr(0x10,2,moduleInfo);R[21]=(u32)module_from_addr((u64)&finish,2,0);
 moduleInfo[0]=8;R[22]=(u32)module_from_addr((u64)&finish,2,moduleInfo);
}

/* ---- mode 7: debug exceptions continue ---- */
static void debug(void) {
 R[10]=(u32)debug_raise(0x1234,5);R[11]=(u32)debug_raise_release(0x1234,5);R[12]=(u32)debug_cpp_info(0);
}

/* ---- mode 8: module loading, process identity, POSIX semaphores, signals ---- */
EXPORT __attribute__((noreturn)) void _start(void);
static u64 semA[4], semB[4], semC[4];
static void* semWaiter(void* unused) {(void)unused;R[60]=(u32)psem_wait(semB);R[61]=1;return 0;}
static void* semTimed(void* unused) {
 long now[2];(void)unused;clock_now(0,now);now[1]+=20000000;if(now[1]>=1000000000){now[1]-=1000000000;++now[0];}
 R[62]=(u32)psem_timedwait(semC,now);R[63]=(u32)*err_loc();return 0;
}
static void posixServices(void) {
 int result=7;u64 address=0;u32 uuid[4]={~0u,~0u,~0u,~0u};
 R[10]=(u32)load_start("/app0/kernel-thread-extras.elf",0,0,0,0,&result);R[11]=(u32)result;
 R[12]=(u32)load_start("/system/common/lib/libSceUnknown.sprx",0,0,0,0,&result);R[13]=(u32)load_start(0,0,0,0,0,&result);
 R[14]=(u32)module_dlsym(0,"_start",&address);R[15]=address==(u64)&_start;
 R[16]=(u32)module_dlsym(0,"no_such_symbol",&address);R[17]=(u32)module_dlsym(9,"_start",&address);
 R[18]=(u32)stop_unload(0,0,0,0,0,&result);R[19]=(u32)stop_unload(9,0,0,0,0,&result);
 R[20]=(u32)is_trinity();R[21]=(u32)uuid_create(uuid);R[22]=(uuid[1]>>28)==4&&(uuid[2]&0xc0)==0x80&&uuid[0]!=~0u;
 R[23]=(u32)uuid_create(0);
 R[24]=(u32)atexit_dec((const void*)&_start);R[25]=(u32)atexit_inc((const void*)&_start);
 R[26]=(u32)atexit_dec((const void*)&_start);R[27]=(u32)atexit_dec((const void*)&_start);R[28]=(u32)atexit_inc((const void*)0x10);
 R[29]=get_pid()>0;
 /* single-threaded semaphore contract */
 R[30]=(u32)psem_init(semA,0,1);R[31]=(u32)psem_wait(semA);
 {long past[2]={0,0};R[32]=(u32)psem_timedwait(semA,past);R[33]=(u32)*err_loc();}
 R[34]=(u32)psem_post(semA);R[35]=(u32)psem_wait(semA);
 R[36]=(u32)psem_destroy(semA);R[37]=(u32)psem_destroy(semA);R[38]=(u32)*err_loc();
 R[39]=(u32)psem_init(semA,0,0x80000000u);R[59]=(u32)*err_loc();
 /* a blocked waiter resumes only after the post */
 status(psem_init(semB,0,0));
 const u64 w=spawn(semWaiter,0,0,relocatedLabel);settle(4);R[41]=R[61];status(psem_post(semB));status(thread_join(w,0));R[42]=R[61];
 /* timed wait that expires reports ETIMEDOUT through the waiter's own errno */
 status(psem_init(semC,0,0));
 const u64 t=spawn(semTimed,0,0,relocatedLabel);status(thread_join(t,0));
 /* rusage, signals, signal mask, program-header match */
 {u64 usage[18];R[43]=(u32)get_rusage(0,usage);R[44]=(u32)get_rusage(5,usage);}
 R[45]=sig_set(2,0x1234);R[46]=sig_set(2,0x5678);R[47]=sig_set(9,0x1);R[48]=(u32)*err_loc();
 {u32 set=0x6,old[4]={9,9,9,9};R[49]=(u32)sig_mask(1,&set,old);R[50]=old[0];
  set=0x2;R[51]=(u32)sig_mask(2,&set,old);R[52]=old[0];R[53]=(u32)sig_mask(3,0,old);R[54]=old[0];R[55]=(u32)sig_mask(7,&set,old);}
 {u64 info[4]={0x1000,0,(u64)&info,1};u64 header[7]={0x100000001ULL,0,0x2000,0,0,0x100,0};
  info[2]=(u64)header;u64 inside=0x1000+0x2000+0x40;
  R[56]=(u32)phdr_match(info,(const void*)inside);R[57]=(u32)phdr_match(info,(const void*)(0x1000+0x2000+0x100));
  info[3]=0;R[58]=(u32)phdr_match(info,(const void*)inside);}
}

EXPORT __attribute__((noreturn)) void _start(void) {
 switch(R[1]){
 case 1: rwlocks();break;
 case 2: onces();break;
 case 3: keys();break;
 case 4: addresses();break;
 case 5: controls();break;
 case 6: modules();break;
 case 7: debug();break;
 case 8: posixServices();break;
 default: ++R[4];
 }
 finish();
}
