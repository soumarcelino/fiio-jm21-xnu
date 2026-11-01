.section .text.head,"ax"
.global _head
_head:
 b entry
 .long 0
 .quad 0
 .quad _end
 .quad 0xa
 .quad 0,0,0
 .ascii "ARM\x64"
 .long 0
entry:
 msr daifset,#15
 mov x19,x0
 mrs x20,CurrentEL
 cmp x20,#4
 b.eq 1f
 cmp x20,#8
 b.ne park
 mrs x1,sctlr_el2
 b 2f
1: mrs x1,sctlr_el1
2: tst x1,#1
 b.ne park
 tst x1,#4
 b.ne park
 msr spsel,#1
 adr x1,_stack_top
 mov sp,x1
 adr x1,vectors
 cmp x20,#4
 b.ne 3f
 msr vbar_el1,x1
 b 4f
3: msr vbar_el2,x1
4: isb
 adr x1,_bss_start
 adr x2,_bss_end
5: cmp x1,x2
 b.hs 6f
 str xzr,[x1],#8
 b 5b
6: mov x0,x19
 mov x1,x20
 bl main
park:
 wfe
 b park
.balign 2048
vectors:
.rept 16
 b shim_exception
 .space 124
.endr
.balign 64
.global secondary_entry
secondary_entry:
 msr daifset,#15
 msr spsel,#1
 adr x1,_gold_stack_top
 mov sp,x1
 adr x1,vectors
 msr vbar_el1,x1
 isb
 bl gold_main
 b park
.global xnu_return
xnu_return:
 msr daifset,#15
 mrs x9,sctlr_el1
 bic x9,x9,#1
 bic x9,x9,#4
 bic x9,x9,#0x1000
 msr sctlr_el1,x9
 isb
 msr spsel,#1
 adr x9,_gold_stack_top
 mov sp,x9
 adr x9,vectors
 msr vbar_el1,x9
 isb
 bl failure
 b park

shim_exception:
 /* USB probes run on Silver; never recover a Gold/XNU exception here. */
 mrs x10,mpidr_el1
 tst x10,#0xff00
 b.ne 9f
 mrs x10,esr_el1
 lsr x10,x10,#26
 cmp x10,#0x25
 b.ne 9f
 adr x9,usb_fault_resume
 ldr x10,[x9]
 cbz x10,9f
 msr elr_el1,x10
 adr x10,usb_faulted
 mov w0,#1
 str w0,[x10]
 mov x0,#0
 eret
9:
 mov x0,#0
 mov x1,#0
 mov x2,#3
 mrs x3,esr_el1
 mrs x4,elr_el1
 mrs x5,far_el1
 b failure
