/* Static Darwin arm64 entry and raw syscalls for the APFS root programs.
 * sp holds argc, argv[], NULL, envp[], NULL. Errors return -errno. */
.text
.global _start
_start:
 mov x0,sp
 and x9,x0,#~15
 mov sp,x9
 bl umain
 mov x16,#1
 svc #0x80
1: b 1b
.global raw_syscall
raw_syscall:
 mov x16,x0
 mov x0,x1
 mov x1,x2
 mov x2,x3
 mov x3,x4
 mov x4,x5
 mov x5,x6
 svc #0x80
 b.cc 1f
 neg x0,x0
1: ret
/* fork returns twice; the kernel marks the child with x1 = 1. */
.global sys_fork
sys_fork:
 mov x16,#2
 svc #0x80
 b.cc 1f
 neg x0,x0
 ret
1: cbz x1,2f
 mov x0,#0
2: ret
/* pipe returns both descriptors in x0 and x1. */
.global sys_pipe
sys_pipe:
 mov x9,x0
 mov x16,#42
 svc #0x80
 b.cc 1f
 neg x0,x0
 ret
1: str w0,[x9]
 str w1,[x9,#4]
 mov x0,#0
 ret
