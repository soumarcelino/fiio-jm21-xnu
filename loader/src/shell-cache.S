/* Pinned kern.hostname bridge: isolated ramdisk padding, explicit cache sync.
 * No cache line is shared with hostname_lock or another kernel object. */
.text
.global shell_read_copy, shell_write_copy
.extern strlcpy_chk
shell_read_copy:
 stp x19,x30,[sp,#-16]!
 mov x19,x1
 dc ivac,x19
 add x19,x19,#64
 dc ivac,x19
 add x19,x19,#64
 dc ivac,x19
 add x19,x19,#64
 dc ivac,x19
 dsb sy
 bl strlcpy_chk
 ldp x19,x30,[sp],#16
 ret
shell_write_copy:
 stp x19,x30,[sp,#-16]!
 mov x19,x0
 bl strlcpy_chk
 dc cvac,x19
 add x19,x19,#64
 dc cvac,x19
 add x19,x19,#64
 dc cvac,x19
 add x19,x19,#64
 dc cvac,x19
 dsb sy
 ldp x19,x30,[sp],#16
 ret
