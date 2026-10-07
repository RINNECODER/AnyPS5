/* Public x86-64 ELF control; never executed. Types/sizes are intentional ABI data. */
.text
.globl public_entry
.type public_entry,@function
public_entry:
  mov $7, %eax
  ret
.size public_entry, .-public_entry

/* Linker relocations retain independently typed undefined dynamic symbols. */
.type host_function,@function
.type guest_function,@function
.type absent_function,@function
.type wrong_type,@function
.type guest_object,@object
.size guest_object,8
.type guest_tls,@tls_object
.size guest_tls,8
.data
.globl references
.type references,@object
references:
  .quad host_function, guest_function, absent_function, wrong_type, guest_object
  .quad guest_tls@DTPOFF
.size references, .-references

.section .note.GNU-stack,"",@progbits
