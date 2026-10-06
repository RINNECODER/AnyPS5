.text
.globl _start
.protected _start
.type _start,@function
_start:
    sub $8, %rsp
    call SceCrtCheck@PLT
    mov %eax, %edi
    call uMei1W9uyNo@PLT
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
