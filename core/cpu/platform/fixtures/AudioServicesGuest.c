typedef unsigned int u32;
typedef unsigned long long u64;
typedef int (*Init)(long long, u32*);
typedef int (*Finalize)(u32);
typedef int (*Register)(u32, u32, long long);
struct Gates { Init init; Finalize finalize; Register module_register; };
struct Output { u32 before, a, after, b, next, passed; };

// Contract: actual guest pointer writes, independent contexts, errors, stale-handle teardown.
// Regression: stub finalize accepts a stale handle; stub module register invents decoder support;
// 8-byte context write corrupts a sentinel. Existing AudioOut fixture has no AJM owner.
__attribute__((section(".text.start"))) int _start(const struct Gates* g, volatile struct Output* o) {
    o->before = 0xa7a7a7a7u; o->after = 0x5e5e5e5eu;
    o->a = 0xababababu; o->b = 0xbcbcbcbcu; o->passed = 0;
    if ((u32)g->init(1, (u32*)&o->a) != 0x80930005u || o->a != 0xababababu) return 1;
    if ((u32)g->init(0, (u32*)0) != 0x80930005u) return 2;
    if (g->init(0, (u32*)&o->a) || g->init(0, (u32*)&o->b)) return 3;
    if (!o->a || !o->b || o->a == o->b || o->before != 0xa7a7a7a7u || o->after != 0x5e5e5e5eu) return 4;
    if ((u32)g->module_register(o->a, 0, 0) != 0x80930008u) return 5;
    if ((u32)g->module_register(o->a, 1, 1) != 0x80930005u) return 6;
    if (g->finalize(o->a)) return 7;
    if ((u32)g->finalize(o->a) != 0x80930002u) return 8;
    if ((u32)g->module_register(o->a, 0, 0) != 0x80930002u) return 9;
    if ((u32)g->module_register(o->b, 24, 0) != 0x80930008u) return 10;
    if (g->init(0, (u32*)&o->next) || o->next == o->a || o->next == o->b) return 11;
    if (g->finalize(o->b) || g->finalize(o->next)) return 12;
    if ((u32)g->finalize(0) != 0x80930002u) return 13;
    o->passed = 0x50415353u;
    return 0;
}
