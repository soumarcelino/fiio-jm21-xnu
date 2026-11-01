/* Static multi-call tools for the APFS root, selected by argv[0]. */
#include "ulib.h"
static int fail(const char *who,const char *what,long r) {put(2,who);put(2,": ");put(2,what);put(2,": errno ");putn(2,-r);put(2,"\n");return 1;}
static int cat_fd(long fd) {char b[4096];long n;while((n=SC(SYS_read,fd,b,sizeof(b),0,0,0))>0)SC(SYS_write,1,b,n,0,0,0);return n<0;}
static int ls(const char *path,int many) {
 long fd=SC(SYS_open,path,O_RDONLY,0,0,0,0);if(fd<0)return fail("ls",path,fd);
 if(many){put(1,path);put(1,":\n");}
 char b[4096];long pos=0,n;
 while((n=SC(SYS_getdirentries64,fd,b,sizeof(b),&pos,0,0))>0) {
  unsigned off=0;
  while(off+21<=(unsigned)n) {
   unsigned rec=(unsigned char)b[off+16]|(unsigned char)b[off+17]<<8,len=(unsigned char)b[off+18]|(unsigned char)b[off+19]<<8;
   if(rec<22||off+rec>(unsigned)n||len>rec-21)break;
   const char *name=b+off+21;
   if(!(name[0]=='.'&&(len==1||(len==2&&name[1]=='.')))) {
    SC(SYS_write,1,name,len,0,0,0);put(1,b[off+20]==4?"/\n":b[off+20]==10?"@\n":"\n");
   }
   off+=rec;
  }
 }
 SC(SYS_close,fd,0,0,0,0,0);return n<0;
}
static int sysctl(const char *name) {
 char b[16384];unsigned long n=sizeof(b);long r=SC(SYS_sysctlbyname,name,slen(name),b,&n,0,0);
 if(r<0)return fail("sysctl",name,r);
 int text=n>0;for(unsigned long i=0;i<n;i++)if((b[i]<32||b[i]>126)&&!(b[i]==0&&i==n-1)&&b[i]!='\n'&&b[i]!='\t')text=0;
 put(1,name);put(1,": ");
 if(text){if(b[n-1]==0)n--;SC(SYS_write,1,b,n,0,0,0);}
 else if(n==4)putn(1,*(int *)b);
 else if(n==8)putn(1,*(long *)b);
 else {put(1,"(");putn(1,(long)n);put(1," bytes)");}
 put(1,"\n");return 0;
}
/* kinfo_proc (648 bytes): kp_proc.p_pid at 40, p_stat at 36, p_comm[17] at 243; kp_eproc.e_ppid at 560. */
static int ps(void) {
 int mib[3]={1,14,0};char b[648*96];unsigned long n=sizeof(b);
 long r=SC(SYS_sysctl,mib,3,b,&n,0,0);if(r<0)return fail("ps","kern.proc.all",r);
 put(1,"  PID  PPID COMMAND\n");
 for(unsigned long off=0;off+648<=n;off+=648) {
  int pid=*(int *)(b+off+40),ppid=*(int *)(b+off+560);char c[18];
  for(unsigned i=0;i<17;i++){c[i]=b[off+243+i];}
  c[17]=0;
  char p[8];long v=pid;for(int i=6;i>=0;i--){p[i]=v||i==6?(char)('0'+v%10):' ';v/=10;}p[7]=0;put(1,p+2);put(1," ");
  v=ppid;for(int i=6;i>=0;i--){p[i]=v||i==6?(char)('0'+v%10):' ';v/=10;}put(1,p+2);put(1," ");put(1,c);put(1,"\n");
 }
 return 0;
}
int umain(long *sp) {
 int argc=(int)sp[0];char **argv=(char **)(sp+1);char **envp=argv+argc+1;
 const char *cmd=base_name(argv[0]);
 if(seq(cmd,"echo")){int nl=1,i=1;if(argc>1&&seq(argv[1],"-n")){nl=0;i=2;}for(int f=i;i<argc;i++){if(i>f)put(1," ");put(1,argv[i]);}if(nl)put(1,"\n");return 0;}
 if(seq(cmd,"cat")){if(argc<2)return cat_fd(0);int e=0;for(int i=1;i<argc;i++){long fd=SC(SYS_open,argv[i],O_RDONLY,0,0,0,0);if(fd<0){e=fail("cat",argv[i],fd);continue;}e|=cat_fd(fd);SC(SYS_close,fd,0,0,0,0,0);}return e;}
 if(seq(cmd,"ls")){if(argc<2)return ls(".",0);int e=0;for(int i=1;i<argc;i++)e|=ls(argv[i],argc>2);return e;}
 if(seq(cmd,"pwd")){char b[1024]={0};long fd=SC(SYS_open,".",O_RDONLY,0,0,0,0);if(fd<0)return fail("pwd",".",fd);long r=SC(SYS_fcntl,fd,50,b,0,0,0);SC(SYS_close,fd,0,0,0,0,0);if(r<0)return fail("pwd","F_GETPATH",r);put(1,b);put(1,"\n");return 0;}
 if(seq(cmd,"id")){put(1,"uid=");putn(1,SC(SYS_getuid,0,0,0,0,0,0));put(1," euid=");putn(1,SC(SYS_geteuid,0,0,0,0,0,0));put(1," pid=");putn(1,SC(SYS_getpid,0,0,0,0,0,0));put(1,"\n");return 0;}
 if(seq(cmd,"uname")){if(argc>1&&seq(argv[1],"-a"))return sysctl("kern.version");put(1,"Darwin\n");return 0;}
 if(seq(cmd,"sysctl")){if(argc<2){put(2,"usage: sysctl NAME...\n");return 1;}int e=0;for(int i=1;i<argc;i++)e|=sysctl(argv[i]);return e;}
 if(seq(cmd,"ps"))return ps();
 if(seq(cmd,"kill")){int s=15,i=1;if(argc>1&&argv[1][0]=='-'){s=(int)atol_(argv[1]+1);i=2;}int e=0;for(;i<argc;i++){long r=SC(SYS_kill,atol_(argv[i]),s,1,0,0,0);if(r<0)e=fail("kill",argv[i],r);}return e;}
 if(seq(cmd,"sleep")){if(argc>1)sleep_ms(atol_(argv[1])*1000);return 0;}
 if(seq(cmd,"env")){for(unsigned i=0;envp[i];i++){put(1,envp[i]);put(1,"\n");}return 0;}
 if(seq(cmd,"mkdir")){int e=0;for(int i=1;i<argc;i++){long r=SC(SYS_mkdir,argv[i],0755,0,0,0,0);if(r<0)e=fail("mkdir",argv[i],r);}return e;}
 if(seq(cmd,"xd")) { /* xd FILE [OFFSET] [LEN]: hex of LEN bytes (default 256); raw disks want 512-byte multiples */
  if(argc<2){put(2,"usage: xd FILE [OFFSET] [LEN]\n");return 1;}
  long off=argc>2?atol_(argv[2]):0,len=argc>3?atol_(argv[3]):256;if(len<1||len>4096)len=256;
  long fd=SC(SYS_open,argv[1],O_RDONLY,0,0,0,0);if(fd<0)return fail("xd",argv[1],fd);
  long r=SC(SYS_lseek,fd,off,0,0,0,0);if(r<0)return fail("xd","lseek",r);
  unsigned char b[4096];long n=SC(SYS_read,fd,b,(len+511)&~511L,0,0,0);
  if(n<0){n=SC(SYS_read,fd,b,len,0,0,0);}
  SC(SYS_close,fd,0,0,0,0,0);if(n<0)return fail("xd","read",n);if(n>len)n=len;
  for(long i=0;i<n;i+=16) {
   char line[80];unsigned k=0;const char *h="0123456789abcdef";
   for(int d=28;d>=0;d-=4){line[k++]=h[((unsigned long)(off+i)>>d)&15];}
   line[k++]=' ';
   for(long j=0;j<16;j++){if(i+j<n){line[k++]=h[b[i+j]>>4];line[k++]=h[b[i+j]&15];}else{line[k++]=' ';line[k++]=' ';}line[k++]=' ';}
   for(long j=0;j<16&&i+j<n;j++)line[k++]=b[i+j]>=32&&b[i+j]<127?(char)b[i+j]:'.';
   line[k++]='\n';line[k]=0;put(1,line);
  }
  return 0;
 }
 if(seq(cmd,"mount")) { /* mount TYPE SPEC DIR [rw]: apfs takes the device path, devfs any string */
  if(argc<4){put(2,"usage: mount TYPE SPEC DIR [rw]\n");return 1;}
  long r=SC(SYS_mount,argv[1],argv[3],argc>4&&seq(argv[4],"rw")?0:1,argv[2],0,0);
  return r<0?fail("mount",argv[3],r):0;
 }
 if(seq(cmd,"chroot")) { /* chroot DIR CMD [ARGS]: runs a program of another root, its dyld and shared cache */
  if(argc<3){put(2,"usage: chroot DIR CMD [ARGS]\n");return 1;}
  long r=SC(SYS_chroot,argv[1],0,0,0,0,0);if(r<0)return fail("chroot",argv[1],r);
  SC(SYS_chdir,"/",0,0,0,0,0);
  r=SC(SYS_execve,argv[2],argv+2,envp,0,0,0);return fail("chroot",argv[2],r);
 }
 if(seq(cmd,"poweroff")) {
  if(argc!=1){put(2,"usage: poweroff\n");return 2;}
  /* Darwin reboot(RB_HALT), without NOSYNC/QUICK: process shutdown, sync,
   * unmount, IOKit notifications, then platform SYSTEM_OFF. Root required. */
  if(SC(SYS_geteuid,0,0,0,0,0,0)!=0){put(2,"poweroff: root required\n");return 1;}
  put(1,"poweroff: shutting down and syncing volumes\n");
  long r=SC(SYS_reboot,0x08,0,0,0,0,0);
  return r<0?fail("poweroff","reboot(RB_HALT)",r):0;
 }
 if(seq(cmd,"true"))return 0;
 if(seq(cmd,"false"))return 1;
 put(2,"tools: unknown command ");put(2,cmd);put(2,"\n");return 127;
}
