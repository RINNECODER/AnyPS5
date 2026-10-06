.text
.globl _start
.protected _start
.type _start,@function
_start:
    mov %rdi, %r12
    sub $8, %rsp
    mov (%r12), %rdi
    lea 8(%r12), %rsi
    xor %edx, %edx
    call SceModuleMainInit
    test %eax, %eax
    jne .Lexit
    mov (%r12), %rdi
    lea 8(%r12), %rsi
    call SceModuleMain
.Lexit:
    mov %eax, %edi
    call uMei1W9uyNo@PLT
    ud2
.size _start, .-_start

.globl SceModuleImportedTlsIndex
.protected SceModuleImportedTlsIndex
.type SceModuleImportedTlsIndex,@function
SceModuleImportedTlsIndex:
    leaq SceModuleTls@TLSGD(%rip), %rax
    ret
.size SceModuleImportedTlsIndex, .-SceModuleImportedTlsIndex

.globl SceModuleMainTlsIndex
.protected SceModuleMainTlsIndex
.type SceModuleMainTlsIndex,@function
SceModuleMainTlsIndex:
    leaq SceModuleMainTls@TLSGD(%rip), %rax
    ret
.size SceModuleMainTlsIndex, .-SceModuleMainTlsIndex
.section .note.GNU-stack,"",@progbits
