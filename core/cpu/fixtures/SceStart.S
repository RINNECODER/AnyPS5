.text
.globl _start
.type _start,@function
_start:
    mov %rsp, %rcx
    and $15, %ecx
    xor %r8d, %r8d
    cmp %rdi, %rsp
    sete %r8b
    mov %rsp, %r9
    sub $8, %rsp
    mov %rdi, %rax
    mov %rsi, %rdx
    mov (%r9), %rdi
    lea 8(%rax), %rsi
    call SceGuestMain
    mov %eax, %edi
    call uMei1W9uyNo@PLT
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
