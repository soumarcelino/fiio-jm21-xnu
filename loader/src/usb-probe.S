/* Recover synchronous MMIO aborts on the loader CPU only. XNU has its own VBAR.
 * No guessed register access may send the entire boot to the debug stop loop.
 */
.text
.global probe_read32
probe_read32:
 adr x9,usb_fault_resume
 adr x10,1f
 str x10,[x9]
 dsb sy
 ldr w0,[x0]
 dsb sy
1:
 str xzr,[x9]
 ret
.global probe_write32
probe_write32:
 adr x9,usb_fault_resume
 adr x10,2f
 str x10,[x9]
 dsb sy
 str w1,[x0]
 dsb sy
2:
 str xzr,[x9]
 ret
.bss
.balign 8
.global usb_fault_resume
usb_fault_resume: .skip 8
.global usb_faulted
usb_faulted: .skip 4
