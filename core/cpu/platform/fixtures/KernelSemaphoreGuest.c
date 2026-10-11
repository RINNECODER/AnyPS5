/* Public synthetic guest. Real linked semaphore imports, TLS and thread frames. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int sema_create(u64*, const char*, u32, int, int, const void*);
extern int sema_delete(u64);
extern int sema_wait(u64, int, void*);
extern int sema_signal(u64, int);
extern int thread_create(u64*,const u64*,void*(*)(void*),void*,const char*);
extern void thread_yield(void);
extern int thread_join(u64,void**);
extern u64 thread_self(void);
extern __attribute__((noreturn)) void process_exit(int);
extern int attr_init(u64*);
extern int attr_destroy(u64*);
extern int attr_priority(u64*,const int*);
extern int attr_inherit(u64*,int);
extern int mutex_init(u64*,const u64*,const char*);
extern int mutex_lock(u64*);
extern int mutex_unlock(u64*);
extern int mutex_destroy(u64*);
extern int mutex_attr_init(u64*);
extern int mutex_attr_protocol(u64*,int);
extern int mutex_attr_destroy(u64*);
#define FUNC(n) ".type " #n ",@function\n"
__asm__(FUNC(sema_create) FUNC(sema_delete) FUNC(sema_wait) FUNC(sema_signal)
 FUNC(thread_create) FUNC(thread_yield) FUNC(thread_join) FUNC(thread_self)
 FUNC(process_exit) FUNC(attr_init) FUNC(attr_destroy) FUNC(attr_priority) FUNC(attr_inherit)
 FUNC(mutex_init) FUNC(mutex_lock) FUNC(mutex_unlock) FUNC(mutex_destroy)
 FUNC(mutex_attr_init) FUNC(mutex_attr_protocol) FUNC(mutex_attr_destroy));
EXPORT volatile u64 KernelSemaphoreReceipt[128];
#define R KernelSemaphoreReceipt
#define CANARY 0x1badc0ffeef00d55ULL
struct Slot {u64 before,value,after;};
static struct Slot sema, attr, donorAttr;
static u64 inheritanceMutex, mutexAttr;
static const char label[]="public-semaphore-fixture";
static const char* volatile relocatedLabel=label;
static __thread u64 tlsInitial __attribute__((used,aligned(16)))=0x1020304050607080ULL;
static __thread u64 tlsZero[2] __attribute__((used));
static volatile u64 payload;
/* Use the existing SCE TCB/DTV ABI; compiler TLS declarations supply the true
   initialized/zero-fill PT_TLS template without adding private lookup seams. */
static volatile u64* active_tls(void) {
 u64 dtv;__asm__ volatile("movq %%fs:8,%0":"=r"(dtv));return (volatile u64*)((const u64*)dtv)[2];
}
static void status(int code) {if(code) ++R[4];}
static void trace(u64 value) {u64 n=R[5];if(n<16){R[100+n]=value;R[5]=n+1;}else ++R[4];}
/* Preserve the true suspended PLT frame, six callee-saved registers and spill. */
__attribute__((visibility("hidden"))) void semaphore_wait_abi(u64,int,volatile u64*);
__asm__(".text\n.hidden semaphore_wait_abi\n.type semaphore_wait_abi,@function\nsemaphore_wait_abi:\n"
 "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $24,%rsp\n"
 "mov %rdx,(%rsp)\nmovabs $0x1234567890abcdef,%rax\n"
 "mov %rax,%rbx\nmov %rax,%rbp\nmov %rax,%r12\nmov %rax,%r13\nmov %rax,%r14\nmov %rax,%r15\n"
 "mov %rax,16(%rsp)\nlea -8(%rsp),%rax\nmov %rax,256(%rdx)\n"
 "lea 1f(%rip),%rax\nmov %rax,264(%rdx)\nxor %edx,%edx\ncall sema_wait@PLT\n1:\n"
 "mov (%rsp),%rdi\nmov %eax,%eax\nmov %rax,272(%rdi)\n"
 "movabs $0x1234567890abcdef,%rdx\nxor %eax,%eax\n"
 "mov %rbx,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rbp,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r12,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r13,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov %r14,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %r15,%rcx\nxor %rdx,%rcx\nor %rcx,%rax\n"
 "mov 16(%rsp),%rcx\nxor %rdx,%rcx\nor %rcx,%rax\nmov %rax,280(%rdi)\n"
 "mov %rsp,%rax\nsub 256(%rdi),%rax\nsub $8,%rax\nmov %rax,288(%rdi)\n"
 "add $24,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
 ".size semaphore_wait_abi,.-semaphore_wait_abi\n"
 ".hidden poison_signal\n.type poison_signal,@function\npoison_signal:\n"
 "movabs $0xa5a5a5a500000002,%rsi\njmp sema_signal@PLT\n.size poison_signal,.-poison_signal\n"
 ".hidden poison_wait\n.type poison_wait,@function\npoison_wait:\n"
 "movabs $0xa5a5a5a500000001,%rsi\nxor %edx,%edx\njmp sema_wait@PLT\n.size poison_wait,.-poison_wait\n"
 ".hidden poison_create\n.type poison_create,@function\npoison_create:\n"
 "xor %edx,%edx\nmovabs $0xa5a5a5a500000000,%rcx\nmovabs $0xa5a5a5a500000003,%r8\n"
 "xor %r9d,%r9d\njmp sema_create@PLT\n.size poison_create,.-poison_create\n");
__attribute__((visibility("hidden"))) int poison_signal(u64);
__attribute__((visibility("hidden"))) int poison_wait(u64);
__attribute__((visibility("hidden"))) int poison_create(u64*,const char*);
static void* donor(void* unused) {
 (void)unused;R[67]=1;R[12]=thread_self();status(mutex_lock(&inheritanceMutex));
 status(mutex_unlock(&inheritanceMutex));R[68]=1;return (void*)0x4000;
}
static u64 spawn_donor(void) {
 u64 id=0;int priority=300;
 status(attr_init(&donorAttr.value));status(attr_priority(&donorAttr.value,&priority));status(attr_inherit(&donorAttr.value,0));
 status(thread_create(&id,&donorAttr.value,donor,0,relocatedLabel));status(attr_destroy(&donorAttr.value));return id;
}
static void* waiter(void* argument) {
 u64 role=(u64)argument; R[8+role]=thread_self();
 u64 donorId=0;
 if(R[1]==13 && role==0){status(mutex_lock(&inheritanceMutex));donorId=spawn_donor();}
 volatile u64* tls=active_tls();R[22+role]=(u64)tls;
 if(tls[0]!=0x1020304050607080ULL || tls[1] || tls[2]) ++R[4];
 tls[0]=0x9000+role;tls[1]=role+1;
 R[6]|=1ULL<<role;
 int need=(R[1]==9 && role==0)?2:1;
 if(role==0){semaphore_wait_abi(sema.value,need,R);R[40]=R[34];}
 else R[40+role]=(u32)sema_wait(sema.value,need,0);
 if(active_tls()!=tls || tls[0]!=0x9000+role || tls[1]!=role+1 || tls[2]) ++R[4];
 if(!R[40+role] && payload!=0x2131415161718191ULL) ++R[4];
 trace(role+1);R[7]|=1ULL<<role;
 if(R[1]==13 && role==0){status(mutex_unlock(&inheritanceMutex));status(thread_join(donorId,(void**)&R[69]));}
 return (void*)(0x3000+role);
}
static u64 spawn(unsigned role) {
 u64 id=0;int priority=R[1]==9?(role?400:300):(R[1]==13?(role?400:600):(role?300:600));
 status(attr_init(&attr.value));status(attr_priority(&attr.value,&priority));status(attr_inherit(&attr.value,0));
 status(thread_create(&id,&attr.value,waiter,(void*)(u64)role,relocatedLabel));status(attr_destroy(&attr.value));return id;
}
static __attribute__((noreturn)) void finish(void) {
 R[19]=sema.value;R[20]=sema.before;R[21]=sema.after;
 if(sema.before!=CANARY || sema.after!=CANARY) ++R[4];
 R[0]=0x53454d4150484f52ULL;process_exit(R[4]?1:0);
}
EXPORT __attribute__((noreturn)) void _start(void) {
 const u64 mode=R[1];sema.before=sema.after=attr.before=attr.after=CANARY;
 sema.value=0xfeedbeefdeadbeefULL;R[18]=(u64)&sema.value;R[11]=thread_self();
 if(mode==7) {
  R[70]=(u32)sema_create((u64*)R[90],R[98]?(const char*)R[91]:relocatedLabel,0,0,3,0);
  if(!R[70]) {R[16]=*(u64*)R[90];status(sema_delete(R[16]));}finish();
 }
 if(mode==6) {
  status(sema_create(&sema.value,relocatedLabel,0,0,3,0));R[16]=sema.value;
  R[70]=(u32)sema_delete(R[90]);R[71]=(u32)sema_wait(R[90],1,0);R[72]=(u32)sema_signal(R[90],1);
  status(sema_delete(sema.value));finish();
 }
 if(mode==4 || mode==5) {
  status(sema_create(&sema.value,relocatedLabel,0,mode==5?0x7fffffff:2,mode==5?0x7fffffff:3,0));R[16]=sema.value;
  if(mode==5) {
   R[70]=(u32)sema_signal(sema.value,1);R[71]=(u32)sema_wait(sema.value,0x7fffffff,0);
   R[72]=(u32)sema_signal(sema.value,0x7fffffff);R[73]=(u32)sema_wait(sema.value,0x7fffffff,0);
  } else {
   R[70]=(u32)sema_wait(sema.value,1,0);R[71]=(u32)sema_signal(sema.value,3);
   R[72]=(u32)poison_signal(sema.value);R[73]=(u32)poison_wait(sema.value);
   R[74]=(u32)sema_wait(sema.value,0,0);R[75]=(u32)sema_wait(sema.value,-1,0);
   R[76]=(u32)sema_wait(sema.value,4,0);R[77]=(u32)sema_signal(sema.value,0);R[78]=(u32)sema_signal(sema.value,-1);
   R[79]=(u32)sema_wait(sema.value,1,(void*)1);R[80]=(u32)sema_wait(sema.value,2,0);
   R[81]=(u32)sema_signal(sema.value,3);R[82]=(u32)sema_signal(sema.value,1);R[83]=(u32)sema_wait(sema.value,3,0);
   R[84]=(u32)sema_signal((u32)sema.value,1);R[85]=(u32)sema_wait(sema.value^0x100000000ULL,1,0);
   R[86]=(u32)sema_delete((u32)sema.value);
   u64 invalid=0xfeedbeefdeadbeefULL;
   R[87]=(u32)sema_create(&invalid,relocatedLabel,3,0,3,0);R[88]=(u32)sema_create(&invalid,relocatedLabel,0,0,3,(void*)1);
   R[89]=invalid;R[93]=(u32)sema_create(&invalid,relocatedLabel,0,-1,3,0);
   R[94]=(u32)sema_create(&invalid,relocatedLabel,0,4,3,0);R[95]=(u32)sema_create(&invalid,relocatedLabel,0,0,0,0);
  }
  status(sema_delete(sema.value));R[96]=(u32)sema_delete(sema.value);
  status(mode==4?poison_create(&sema.value,relocatedLabel):sema_create(&sema.value,relocatedLabel,0,0,3,0));R[17]=sema.value;
  R[97]=(u32)sema_signal(R[16],1);status(sema_delete(sema.value));finish();
 }
 status(sema_create(&sema.value,relocatedLabel,0,0,mode==0 || mode==10?0x7fffffff:3,0));R[16]=sema.value;
 if(mode==13){status(mutex_attr_init(&mutexAttr));status(mutex_attr_protocol(&mutexAttr,1));
  status(mutex_init(&inheritanceMutex,&mutexAttr,relocatedLabel));status(mutex_attr_destroy(&mutexAttr));}
 if(mode==10){R[60]=1;R[70]=(u32)sema_wait(sema.value,1,0);finish();}
 unsigned count=mode==9 || mode==13?2:3;u64 ids[3];
 for(unsigned i=0;i<count;++i) ids[i]=spawn(i);
 R[60]=R[7];R[62]=R[6];
 if(mode==2 || mode==8 || mode==12 || mode==13) {R[63]=1;while(!R[92]){++R[61];thread_yield();}}
 if(mode==11) process_exit(0);
 payload=0x2131415161718191ULL;
 if(mode==12){status(sema_signal(sema.value,1));status(sema_delete(sema.value));R[66]=1;thread_yield();}
 if(mode==1) status(sema_delete(sema.value));
 else if(mode==3){status(sema_signal(sema.value,1));status(sema_delete(sema.value));}
 else if(mode==9){status(sema_signal(sema.value,1));thread_yield();R[64]=R[7];status(sema_signal(sema.value,2));}
 else if(mode==13){status(sema_signal(sema.value,1));thread_yield();R[64]=R[7];status(sema_signal(sema.value,1));}
 else {status(sema_signal(sema.value,1));thread_yield();R[64]=R[7];status(sema_signal(sema.value,1));thread_yield();R[65]=R[7];status(sema_signal(sema.value,1));}
 for(unsigned i=0;i<count;++i) status(thread_join(ids[i],(void**)&R[48+i]));
 if(mode==13)status(mutex_destroy(&inheritanceMutex));
 if(mode!=1 && mode!=3) status(sema_delete(sema.value));finish();
}
