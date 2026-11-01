ENTRY(_start)
SECTIONS {
 . = 0x100000400;
 .text : { *(.text) *(.text.*) *(.rodata*) }
 .data : { *(.data*) *(.bss*) *(COMMON) }
 ASSERT(SIZEOF(.data) == 0, "userspace writable globals unsupported")
 /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
