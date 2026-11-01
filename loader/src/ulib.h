/* Freestanding helpers for the static Darwin programs: no libc, dyld or writable globals. */
extern long raw_syscall(long,long,long,long,long,long,long);
extern long sys_fork(void);
extern long sys_pipe(int *fds);
#define SC(n,a,b,c,d,e,f) raw_syscall(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_wait4 7
#define SYS_chmod 15
#define SYS_chown 16
#define SYS_chdir 12
#define SYS_chroot 61
#define SYS_getpid 20
#define SYS_getuid 24
#define SYS_geteuid 25
#define SYS_kill 37
#define SYS_sigaction 46
#define SYS_ioctl 54
#define SYS_reboot 55
#define SYS_symlink 57
#define SYS_execve 59
#define SYS_setpgid 82
#define SYS_dup2 90
#define SYS_fcntl 92
#define SYS_select 93
#define SYS_gettimeofday 116
#define SYS_settimeofday 122
#define SYS_lseek 199
#define SYS_mkdir 136
#define SYS_setsid 147
#define SYS_mount 167
#define SYS_sysctl 202
#define SYS_sysctlbyname 274
#define SYS_getdirentries64 344
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_NONBLOCK 4
#define O_APPEND 8
#define O_CREAT 0x200
#define O_TRUNC 0x400
#define O_NOCTTY 0x20000
#define TIOCSCTTY 0x20007461
#define TIOCSPGRP 0x80047476
#define TIOCGETA 0x40487413
#define TIOCSETA 0x80487414
#define TIOCPTYGRANT 0x20007454
#define TIOCPTYUNLK 0x20007452
#define TIOCPTYGNAME 0x40807453
void *memset(void *p,int c,unsigned long n) {unsigned char *b=p;while(n--)*b++=(unsigned char)c;return p;}
void *memcpy(void *d,const void *s,unsigned long n) {unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
static __attribute__((unused)) unsigned slen(const char *s) {unsigned n=0;while(s[n])n++;return n;}
static __attribute__((unused)) int seq(const char *a,const char *b) {while(*a&&*a==*b){a++;b++;}return *a==*b;}
static __attribute__((unused)) void put(int fd,const char *s) {SC(SYS_write,fd,s,slen(s),0,0,0);}
static __attribute__((unused)) void putn(int fd,long v) {char b[24];unsigned i=23;b[i]=0;unsigned long u=v<0?-(unsigned long)v:(unsigned long)v;do{b[--i]=(char)('0'+u%10);u/=10;}while(u);if(v<0)b[--i]='-';put(fd,b+i);}
static __attribute__((unused)) long atol_(const char *s) {long v=0;int neg=*s=='-';if(neg)s++;while(*s>='0'&&*s<='9')v=v*10+(*s++-'0');return neg?-v:v;}
static __attribute__((unused)) const char *base_name(const char *p) {const char *b=p;while(*p)if(*p++=='/')b=p;return b;}
/* Darwin struct __sigaction: handler, trampoline, mask, flags. SIG_DFL 0, SIG_IGN 1. */
static __attribute__((unused)) void sig(int n,long handler) {long sa[3]={handler,0,0};SC(SYS_sigaction,n,sa,0,0,0,0);}
static __attribute__((unused)) void sleep_ms(long ms) {long tv[2]={ms/1000,(ms%1000)*1000};SC(SYS_select,0,0,0,0,tv,0);}
