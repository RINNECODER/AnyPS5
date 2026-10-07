// Contract owner: compiled x86 SysV calls must produce independent Gregorian UTC
// ticks and signed failures. Regression: native-pointer casts, offset sign reversal,
// fake success, fractional scale or epoch errors. Existing core/libs RTC tests call
// host pointers directly; they cannot protect ARM64/TCG guest marshaling. No seam.
typedef unsigned long long u64;
typedef int (*Parse)(u64 *, const char *);
typedef int (*Format)(char *, const u64 *, int);
struct Context {
    u64 parse, format;
    char input[128], output[32];
    u64 parsed, source, mask;
    int status[8];
};
__attribute__((section(".text.entry"))) u64 rtc_guest(struct Context *context) {
    Parse parse = (Parse)context->parse;
    Format format = (Format)context->format;
    context->mask = 0;
    context->parsed = 0x123456789abcdef0ULL;
    context->status[0] = parse(&context->parsed, context->input);
    if (context->status[0] == 0 && context->parsed == 63844806896789000ULL) context->mask |= 1;
    context->source = 63844806896789000ULL;
    context->status[1] = format(context->output, &context->source, 90);
    if (context->status[1] == 0 && context->output[11] == '1' && context->output[12] == '4' && context->output[22] == '+' && context->output[28] == 0) context->mask |= 2;
    context->status[2] = parse(&context->parsed, context->output);
    if (context->status[2] == 0 && context->parsed == 63844806896780000ULL) context->mask |= 4;
    context->status[3] = parse((u64 *)0, context->input);
    if ((unsigned)context->status[3] == 0x80b50002u) context->mask |= 8;
    context->status[4] = format((char *)0xdead0000ULL, &context->source, 0);
    if ((unsigned)context->status[4] == 0x80b50002u) context->mask |= 16;
    context->status[5] = format(context->output, &context->source, 1440);
    if ((unsigned)context->status[5] == 0x80b50003u && context->output[22] == '+') context->mask |= 32;
    context->status[6] = format(context->output, (u64 *)0xdead0000ULL, 0);
    if ((unsigned)context->status[6] == 0x80b50002u && context->output[22] == '+') context->mask |= 64;
    context->status[7] = parse(&context->parsed, (char *)0xdead0000ULL);
    if ((unsigned)context->status[7] == 0x80b50002u && context->parsed == 63844806896780000ULL) context->mask |= 128;
    return context->mask;
}
