// Logging stubs for imports that no x86_64 library exports yet. run_title.py
// generates one entry per symbol (movl $index, %eax; jmp _aps5_stub_common)
// and injects the dylib with DYLD_INSERT_LIBRARIES. A stub logs its first call
// and returns 0 to the guest, so a run shows which NIDs a title needs next.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const char* const aps5_stub_names[];
extern const unsigned aps5_stub_count;

static unsigned char* g_called;

void aps5_stub_hit(unsigned index) {
    if (g_called == NULL) g_called = calloc(aps5_stub_count, 1);
    if (g_called == NULL || g_called[index]) return;
    g_called[index] = 1;
    char line[256];
    const int length = snprintf(line, sizeof line, "[rosetta] unimplemented import %s\n", aps5_stub_names[index]);
    if (length > 0) (void)!write(STDERR_FILENO, line, (size_t)length < sizeof line ? (size_t)length : sizeof line - 1);
}

__asm__(".text\n"
        ".globl _aps5_stub_common\n"
        "_aps5_stub_common:\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    andq $-16, %rsp\n"
        "    movl %eax, %edi\n"
        "    call _aps5_stub_hit\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    xorl %eax, %eax\n"
        "    ret\n");
