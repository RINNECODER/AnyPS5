/* Linked ET_DYN fixture: real function imports and one data-pointer RELATIVE relocation. */
typedef unsigned long long u64;
typedef unsigned int u32;
extern int sceHttpUriEscape(char*, u64*, u64, const char*);
extern int sceAjmInitialize(long long, u32*);
extern int sceAjmFinalize(u32);
/* Undefined C references otherwise need not retain STT_FUNC in ELF. Require it explicitly. */
__asm__(".type sceHttpUriEscape,@function\n"
        ".type sceAjmInitialize,@function\n"
        ".type sceAjmFinalize,@function\n");
static const char input[] = {'A','z','0','-','_','.','~',' ','/',(char)255,0};
static const char* volatile relocated_input = input;
struct Receipt {
    u64 marker, required;
    u32 before, a, after, b;
    u32 query, short_output, escape, null_input;
    u32 reserved_init, null_init, finalize_a, stale_a, finalize_b;
    char output[32];
};
__attribute__((visibility("protected"))) int _start(volatile struct Receipt* r) {
    r->marker = 0; r->required = 0;
    r->before = 0xa7a7a7a7u; r->a = 0xababababu;
    r->after = 0x5e5e5e5eu; r->b = 0xbcbcbcbcu;
    for (u32 i=0;i<32;++i) r->output[i] = (char)0xa7;
    r->query = (u32)sceHttpUriEscape((char*)0,(u64*)&r->required,0,relocated_input);
    if (r->query || r->required != 17) return 1;
    r->short_output = (u32)sceHttpUriEscape((char*)r->output,(u64*)&r->required,16,relocated_input);
    if (r->short_output != 0x80431022u || r->output[0] != (char)0xa7) return 2;
    r->escape = (u32)sceHttpUriEscape((char*)r->output,(u64*)&r->required,17,relocated_input);
    if (r->escape || r->required != 17) return 3;
    if (r->output[0]!='A' || r->output[1]!='z' || r->output[2]!='0' ||
        r->output[3]!='-' || r->output[4]!='_' || r->output[5]!='.' || r->output[6]!='~' ||
        r->output[7]!='%' || r->output[8]!='2' || r->output[9]!='0' ||
        r->output[10]!='%' || r->output[11]!='2' || r->output[12]!='F' ||
        r->output[13]!='%' || r->output[14]!='F' || r->output[15]!='F' || r->output[16]!=0) return 4;
    for (u32 i=17;i<32;++i) if (r->output[i] != (char)0xa7) return 5;
    r->null_input = (u32)sceHttpUriEscape((char*)r->output,(u64*)&r->required,17,(const char*)0);
    if (r->null_input != 0x804311feu) return 6;
    r->reserved_init = (u32)sceAjmInitialize(1,(u32*)&r->a);
    if (r->reserved_init != 0x80930005u || r->a != 0xababababu) return 7;
    r->null_init = (u32)sceAjmInitialize(0,(u32*)0);
    if (r->null_init != 0x80930005u) return 8;
    if (sceAjmInitialize(0,(u32*)&r->a) || sceAjmInitialize(0,(u32*)&r->b)) return 9;
    if (!r->a || !r->b || r->a==r->b || r->before!=0xa7a7a7a7u || r->after!=0x5e5e5e5eu) return 10;
    r->finalize_a = (u32)sceAjmFinalize(r->a);
    r->stale_a = (u32)sceAjmFinalize(r->a);
    r->finalize_b = (u32)sceAjmFinalize(r->b);
    if (r->finalize_a || r->stale_a!=0x80930002u || r->finalize_b) return 11;
    r->marker = 0x534345504c41544fULL;
    return 0;
}
