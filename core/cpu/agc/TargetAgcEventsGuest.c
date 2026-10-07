/* Public synthetic x86 guest, linked with genuine PLT and initialized/zero TLS.
 * The target engineering profile is exercised here; this is no vendor binary. */
typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
extern int queue_create(u64*, const char*);
extern int queue_delete(u64);
extern int queue_wait(u64, void*, int, int*, const u32*);
extern int graphics_add(u64, int, u64);
extern int graphics_delete(u64, int);
extern void thread_yield(void);
extern __attribute__((noreturn)) void process_exit(int);
__asm__(".type queue_create,@function\n.type queue_delete,@function\n.type queue_wait,@function\n"
        ".type graphics_add,@function\n.type graphics_delete,@function\n"
        ".type thread_yield,@function\n.type process_exit,@function\n");
EXPORT volatile u64 AgcEventsReceipt[64];
static const char name[] = "public-agc-admission-event";
static const char* volatile relocated_name = name;
static __thread u64 tls_initial __attribute__((used, aligned(16))) = 0x1020304050607080ULL;
static __thread u64 tls_zero[2] __attribute__((used));
static void copy_event(unsigned slot) {
    volatile u64* r=AgcEventsReceipt;
    volatile u64* out=(volatile u64*)r[6];
    for(unsigned i=0;i<4;++i) r[slot+i]=out[i];
}
EXPORT int _start(void) {
    volatile u64* r=AgcEventsReceipt;
    void* out=(void*)r[6]; int* count=(int*)r[7];
    r[4]=(u32)queue_create((u64*)&r[2],relocated_name);
    r[3]=10;
    r[5]=(u32)graphics_add(r[1]==1?r[8]:r[2],(int)r[19],r[20]);
    if(r[1]==4 || r[1]==5 || r[1]==10) {
        r[3]=11;
        r[9]=(u32)graphics_delete(r[1]==4?r[8]:r[2],(int)r[21]);
        if(r[1]==10) {
            r[14]=(u32)queue_delete(r[2]);
            r[0]=0x41474345564e5430ULL;
            process_exit(0);
        }
    }
    r[22]=(u32)queue_wait(r[2],0,1,count,(const u32*)&r[32]);
    r[23]=(u32)queue_wait(r[2],out,1,0,(const u32*)&r[32]);
    r[3]=1;
    thread_yield();
    r[10]=(u32)queue_wait(r[2],r[1]==6?(void*)r[16]:out,1,
                        r[1]==7?(int*)r[17]:count,
                        r[1]==8?(const u32*)r[18]:0);
    r[11]=*(u32*)count;
    copy_event(24);
    r[3]=2;
    r[9]=(u32)graphics_delete(r[2],0);
    r[3]=3;
    thread_yield(); /* The owner submits a second genuine EOP after deletion. */
    r[12]=(u32)queue_wait(r[2],out,1,count,(const u32*)&r[32]);
    r[13]=*(u32*)count;
    copy_event(28);
    r[14]=(u32)queue_delete(r[2]);
    r[15]=(u32)queue_wait(r[2],out,1,count,(const u32*)&r[32]);
    r[0]=0x41474345564e5430ULL;
    process_exit(0);
}
