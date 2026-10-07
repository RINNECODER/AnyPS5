.text
.globl _start
.protected _start
.type _start,@function
_start:
    sub $8, %rsp
    call SceInternalMain
    mov %eax, %edi
    call "6Z83sYWFlA8"@PLT
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
