/* Actual undefined STT_FUNC call, PLT relocation and relocated input pointer. */
typedef unsigned long long u64;
typedef unsigned int u32;
extern int sceHttpUriEscape(char*, u64*, u64, const char*);
__asm__(".type sceHttpUriEscape,@function\n");
static const unsigned char source[] = {'A','z','0','-','_','.','~',' ','/',255,0};
static const char* volatile relocatedSource = (const char*)source;
struct Arguments {
    char* output;
    u64* required;
    u64 capacity;
    const char* input;
    volatile u64* receipt;
    u64 mode;
};
__attribute__((visibility("protected")))
int _start(struct Arguments* a) {
    volatile u64* r = a->receipt;
    r[0] = 0;
    if (a->mode) {
        r[1] = (u32)sceHttpUriEscape(a->output, a->required, a->capacity, a->input);
        r[0] = 0x55524953494e474cULL;
        return 0;
    }
    r[1] = (u32)sceHttpUriEscape((char*)0, a->required, 0, relocatedSource);
    r[2] = *a->required;
    if (r[1] || r[2] != 17) return 1;
    r[3] = (u32)sceHttpUriEscape(a->output, a->required, 16, relocatedSource);
    if (r[3] != 0x80431022U || a->output[0] != (char)0xa7) return 2;
    r[4] = (u32)sceHttpUriEscape(a->output, a->required, 17, relocatedSource);
    r[5] = *a->required;
    if (r[4] || r[5] != 17) return 3;
    if (a->output[0]!='A' || a->output[1]!='z' || a->output[2]!='0' ||
        a->output[3]!='-' || a->output[4]!='_' || a->output[5]!='.' ||
        a->output[6]!='~' || a->output[7]!='%' || a->output[8]!='2' ||
        a->output[9]!='0' || a->output[10]!='%' || a->output[11]!='2' ||
        a->output[12]!='F' || a->output[13]!='%' || a->output[14]!='F' ||
        a->output[15]!='F' || a->output[16]!=0 || a->output[17]!=(char)0xa7) return 4;
    r[0] = 0x555249434f4d504cULL;
    return 0;
}
