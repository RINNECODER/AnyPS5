.text
.globl _start
.type _start,@function
_start:
    xor %ebp, %ebp
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    and $-16, %rsp
    call GuestMain
    mov %eax, %edi
    mov $60, %eax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
