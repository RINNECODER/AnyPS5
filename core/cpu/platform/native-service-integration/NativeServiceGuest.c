typedef unsigned long long u64;
typedef unsigned int u32;
#define EXPORT __attribute__((visibility("protected")))
#if BUILD_SERVICE_DEPENDENCY
static volatile u32 anchor = 7;
static volatile u32* volatile relocatedAnchor = &anchor;
EXPORT u32 NativeServiceDependency(u32 value) { return value * 3 + *relocatedAnchor; }
#else
extern u32 NativeServiceDependency(u32);
extern void guest_exit(int) __asm__("uMei1W9uyNo");
__asm__(".type NativeServiceDependency,@function\n.type uMei1W9uyNo,@function\n");
#if BUILD_SERVICE_NP
extern int sceNpGetOnlineId(u32, void*);
extern int sce_np_state(u32, int*) __asm__("eQH7nWPcAgc");
__asm__(".type sceNpGetOnlineId,@function\n.type eQH7nWPcAgc,@function\n");
static int check(void) {
    volatile unsigned char destination[64];
    for (unsigned i=0;i<64;++i) destination[i]=0xa7;
    int state=-9;
    if (sce_np_state(0x10000000,&state)!=0 || state!=1) return 81;
    if ((u32)sceNpGetOnlineId(0x10000000,(void*)destination)!=0x80550006u) return 82;
    for (unsigned i=0;i<64;++i) if(destination[i]!=0xa7) return 83;
    state=-9;
    if (sce_np_state(0x10000000,&state)!=0 || state!=1) return 84;
    if ((u32)sceNpGetOnlineId(0x10000001,(void*)destination)!=0x80550003u) return 85;
    for (unsigned i=0;i<64;++i) if(destination[i]!=0xa7) return 86;
    return 0;
}
#else
extern int sceHttpUriEscape(char*,u64*,u64,const char*);
__asm__(".type sceHttpUriEscape,@function\n");
static const char* volatile relocatedSource="a /";
static int check(void) {
    u64 required=0;
    volatile char output[9];
    for (unsigned i=0;i<9;++i) output[i]=(char)0xa7;
    if (sceHttpUriEscape((char*)0,&required,0,relocatedSource)!=0 || required!=8) return 91;
    if (sceHttpUriEscape((char*)output,&required,8,relocatedSource)!=0 || required!=8) return 92;
    const char expected[]="a%20%2F";
    for (unsigned i=0;i<8;++i) if(output[i]!=expected[i]) return 93;
    if ((unsigned char)output[8]!=0xa7) return 94;
    return 0;
}
#endif
EXPORT void _start(void);
static void (*volatile relocatedEntry)(void)=_start;
EXPORT void _start(void) {
    if (relocatedEntry!=_start) guest_exit(78);
    const int result=check();
    guest_exit(result ? result : (NativeServiceDependency(11)==40 ? 0 : 77));
    for (;;) {}
}
#endif
