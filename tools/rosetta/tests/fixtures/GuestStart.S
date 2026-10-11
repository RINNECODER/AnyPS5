// Guest entry for the Rosetta runtime fixture: _start(frame, exit) with the
// FreeBSD argc/argv frame in %rdi, as Aps5StartGuest calls it. Returns
// GuestMain's status, which the runtime turns into the process exit code.
.text
.globl _start
.protected _start
.type _start,@function
_start:
    sub $8, %rsp
    mov (%rdi), %rax
    lea 8(%rdi), %rsi
    mov %rax, %rdi
    call GuestMain
    add $8, %rsp
    ret
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
