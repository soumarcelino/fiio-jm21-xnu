ENTRY(shell_read_copy)
SECTIONS {
 . = 0xfffffff007b27890;
 .text : { *(.text*) *(.rodata*) }
 /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
