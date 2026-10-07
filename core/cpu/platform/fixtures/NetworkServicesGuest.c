/* Flat .text fixture; no libc, imports, globals, or external relocations. */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef int (*Escape)(char*, u64*, u64, const char*);
struct Arguments { Escape escape; char* output; const char* input; u64* required; u64* receipt; };
__attribute__((used, section(".text"))) int NetworkServicesGuest(struct Arguments* a) {
    a->receipt[0] = 0;
    a->receipt[1] = (u32)a->escape((char*)0, a->required, 0, a->input);
    a->receipt[2] = *a->required;
    if (a->receipt[1] || a->receipt[2] != 17) return 1;
    a->receipt[3] = (u32)a->escape(a->output, a->required, 16, a->input);
    if (a->receipt[3] != 0x80431022U || a->output[0] != (char)0xa7) return 2;
    a->receipt[4] = (u32)a->escape(a->output, a->required, 17, a->input);
    if (a->receipt[4] || *a->required != 17) return 3;
    if (a->output[0]!='A' || a->output[1]!='z' || a->output[2]!='0' ||
        a->output[3]!='-' || a->output[4]!='_' || a->output[5]!='.' ||
        a->output[6]!='~' || a->output[7]!='%' || a->output[8]!='2' ||
        a->output[9]!='0' || a->output[10]!='%' || a->output[11]!='2' ||
        a->output[12]!='F' || a->output[13]!='%' || a->output[14]!='F' ||
        a->output[15]!='F' || a->output[16]!=0 || a->output[17]!=(char)0xa7) return 4;
    a->receipt[5] = (u32)a->escape(a->output, a->required, 17, (const char*)0);
    if (a->receipt[5] != 0x804311feU) return 5;
    a->receipt[0] = 0x4e45545552494f4bULL;
    return 0;
}
