.text
.globl _start
.type _start,@function
_start:
    movq $0xa003, 0x9000
    movq $0xb003, 0xa000
    movq $0x83, 0xb000
    movq $0xc003, 0xb010
    movq $0x30003, 0xc000
    mov %cr4, %rax
    or $0x20, %rax
    mov %rax, %cr4
    mov $0x9000, %rax
    mov %rax, %cr3
    mov %cr0, %rax
    mov $0x80000001, %edx
    or %rdx, %rax
    mov %rax, %cr0
    movabs $0x1122334455667788, %rax
    mov %rax, 0x400000
    movq $0xe003, 0xd000
    movq $0xf003, 0xe000
    movq $0x83, 0xf000
    movq $0x10003, 0xf010
    movq $0x31003, 0x10000
    mov $0xd000, %rax
    mov %rax, %cr3
    movabs $0x8877665544332211, %rax
    mov %rax, 0x400000
#ifdef FAULT_CASE
    movq $0x55, 0x401000
#endif
    hlt
    ud2
.section .note.GNU-stack,"",@progbits
