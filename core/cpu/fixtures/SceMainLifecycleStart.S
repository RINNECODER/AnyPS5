.text
.globl _start
.protected _start
.type _start,@function
_start:
    mov %rdi, %r12
    mov %rsi, %r13
    sub $8, %rsp
    mov %r13, %rdi
    call "8G2LB+A3rzg"@PLT
    test %eax, %eax
    jne .Lregister_failure
    lea SceMainLifecycleRegisteredTermination(%rip), %rdi
    call "8G2LB+A3rzg"@PLT
    test %eax, %eax
    jne .Lregister_failure
    lea SceMainLifecycleFini(%rip), %rdi
    call "8G2LB+A3rzg"@PLT
    test %eax, %eax
    jne .Lregister_failure
    call SceMainLifecycleInit
    mov (%r12), %rdi
    lea 8(%r12), %rsi
    call SceMainLifecycleMain
    mov %eax, %edi
    call uMei1W9uyNo@PLT
    ud2
.Lregister_failure:
    mov $72, %edi
    call uMei1W9uyNo@PLT
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
