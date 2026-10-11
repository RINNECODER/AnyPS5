// TLS allocation entry for the relinker's macOS %fs stubs: the descriptor is
// in %rax and every other register belongs to the guest, so save the
// caller-saved state around the C allocator. Returns the thread pointer in %rax.
    .text
    .globl _Aps5GuestTlsAllocate_nid_no_patch
_Aps5GuestTlsAllocate_nid_no_patch:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    andq $-64, %rsp
    subq $512, %rsp
    fxsave64 (%rsp)
    movq %rax, %rdi
    call _aps5_tls_allocate
    fxrstor64 (%rsp)
    leaq -64(%rbp), %rsp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    ret

// Shared tail of the generated import stubs: %eax holds the stub index.
    .globl _aps5_stub_common
_aps5_stub_common:
    pushq %rbp
    movq %rsp, %rbp
    movl %eax, %edi
    andq $-16, %rsp
    call _aps5_stub_hit
    movq %rbp, %rsp
    popq %rbp
    xorl %eax, %eax
    ret
