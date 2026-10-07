/* Public x86-64 ELF provider control; never executed. */
.text
.weak guest_function
.protected guest_function
.type guest_function,@function
guest_function:
  mov $9, %eax
  ret
.size guest_function, .-guest_function
.data
.globl wrong_type, guest_object
.type wrong_type,@object
.type guest_object,@object
wrong_type: .quad 11
.size wrong_type,8
guest_object: .quad 13
.size guest_object,8
.section .tdata,"awT",@progbits
.globl guest_tls
.type guest_tls,@tls_object
guest_tls: .quad 17
.size guest_tls,8
.section .note.GNU-stack,"",@progbits
