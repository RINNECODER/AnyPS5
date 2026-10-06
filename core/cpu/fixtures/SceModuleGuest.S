.text
.globl SceModuleTlsIndex
.protected SceModuleTlsIndex
.type SceModuleTlsIndex,@function
SceModuleTlsIndex:
    leaq SceModuleTls@TLSGD(%rip), %rax
    ret
.size SceModuleTlsIndex, .-SceModuleTlsIndex
.section .note.GNU-stack,"",@progbits
