ENTRY(_start)
SECTIONS {
    . = 0x1000;
    .text : { *(.text.entry) *(.text.start) *(.text .text.*) *(.rodata .rodata.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
