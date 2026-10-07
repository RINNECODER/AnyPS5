/* Freestanding x86-64 entry: all pointers are guest addresses, no host libc or fixed test returns. */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef int (*query_fn)(u32, int*);
struct context { query_fn query; int* output; int* bad_output; int result[6]; int value[2]; };
__attribute__((section(".text.entry"))) void content_guest(struct context* c) {
    c->result[0] = c->query(0, c->output);
    c->value[0] = *c->output;
    c->result[1] = c->query(2, c->output);
    c->value[1] = *c->output;
    c->result[2] = c->query(9, c->output);
    c->result[3] = c->query(0, (int*)0);
    c->result[4] = c->query(0, c->bad_output);
    c->result[5] = *c->output;
}
