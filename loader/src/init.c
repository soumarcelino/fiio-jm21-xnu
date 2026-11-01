/* Darwin PID 1 for the APFS root: runs /bin/sh on a pty and bridges it to the
 * USB monitor through the kern.hostname mailbox. Restarts the shell if it exits. */
#include "ulib.h"
#include "shell-wire.h"
#include "build-epoch.h"
struct bridge {char frame[256];unsigned seq;int lost;int master;long shell;char tty[128];int hostname;
 const char *sh;
 unsigned spawns,status;unsigned long to_pty,from_pty;};
/* kern.jm21_bridge on the source-built kernel; the pinned prebuilt redirects kern.hostname instead. */
static long mailbox(struct bridge *s,char *b,int put_) {
 unsigned long n=256;
 const char *name=s->hostname?"kern.hostname":"kern.jm21_bridge";
 long r=SC(SYS_sysctlbyname,name,wire_len(name),put_?0:b,put_?0:&n,put_?b:0,put_?wire_len(b)+1:0);
 if(r==-2&&!s->hostname){s->hostname=1;return mailbox(s,b,put_);}
 return r;
}
static void publish(struct bridge *s,char type,const char *p) {wire_make(s->frame,type,++s->seq,p);mailbox(s,s->frame,1);}
/* One frame per 96 bytes; an unacknowledged frame means the host is gone: drop output until it returns. */
static void output(struct bridge *s,const char *p,unsigned n) {
 while(n&&!s->lost) {
  char hex[193];unsigned take=n>96?96:n;
  for(unsigned i=0;i<take;i++){hex[2*i]=wire_hex((unsigned char)p[i]>>4);hex[2*i+1]=wire_hex(p[i]);}hex[2*take]=0;
  publish(s,'O',hex);
  unsigned i;for(i=0;i<3000;i++){char b[256]={0};if(mailbox(s,b,0)>=0&&wire_valid(b)&&b[0]=='A'&&wire_seq(b)==s->seq)break;sleep_ms(10);}
  if(i==3000)s->lost=1;
  p+=take;n-=take;
 }
}
/* Idle frame with bridge state, for diagnosis from the host. */
static unsigned field(char *b,unsigned n,const char *k,unsigned long v) {
 while(*k){b[n++]=*k++;}
 char d[24];unsigned i=0;do{d[i++]=(char)('0'+v%10);v/=10;}while(v);while(i)b[n++]=d[--i];b[n++]=' ';return n;
}
static void ready(struct bridge *s) {
 char b[160];unsigned n=0;const char *k="pty ";while(*k)b[n++]=*k++;
 n=field(b,n,"pid=",(unsigned long)s->shell);n=field(b,n,"spawns=",s->spawns);n=field(b,n,"in=",s->to_pty);
 n=field(b,n,"out=",s->from_pty);n=field(b,n,"wait=",s->status);n=field(b,n,"lost=",(unsigned long)s->lost);b[n]=0;
 publish(s,'R',b);
}
static void spawn(struct bridge *s,int console) {
 long pid=sys_fork();
 if(pid) {s->shell=pid;s->spawns++;return;}
 /* PID 1 starts without descriptors, so the master may be fd 0-2: close it before dup2. */
 SC(SYS_close,s->master,0,0,0,0,0);
 SC(SYS_setsid,0,0,0,0,0,0);
 long fd=SC(SYS_open,s->tty,O_RDWR,0,0,0,0);
 if(fd<0){put(console,"init: cannot open pty slave\n");SC(SYS_exit,1,0,0,0,0,0);}
 SC(SYS_ioctl,fd,TIOCSCTTY,0,0,0,0);
 /* termios: iflag, oflag, cflag, lflag (8 bytes each), cc[20], speeds. Host echoes locally. */
 long t[9];if(SC(SYS_ioctl,fd,TIOCGETA,t,0,0,0)>=0){t[3]&=~8L;SC(SYS_ioctl,fd,TIOCSETA,t,0,0,0);}
 SC(SYS_dup2,fd,0,0,0,0,0);SC(SYS_dup2,fd,1,0,0,0,0);SC(SYS_dup2,fd,2,0,0,0,0);
 if(fd>2)SC(SYS_close,fd,0,0,0,0,0);
 char *argv[]={"sh",0};
 char *envp[]={"PATH=/bin:/sbin:/usr/bin:/private/preboot/bin","HOME=/","TERM=dumb","USER=root",0};
 SC(SYS_execve,s->sh,argv,envp,0,0,0);
 put(2,"init: cannot exec ");put(2,s->sh);put(2,"\n");SC(SYS_exit,127,0,0,0,0,0);
}
static int step(int console,const char *what,long r);
/* Is KEY (e.g. "var_store=1") one of the kernel's arguments? */
static int boot_arg(const char *key) {
 char a[1024]={0};unsigned long n=sizeof(a)-1;
 if(SC(SYS_sysctlbyname,"kern.bootargs",13,a,&n,0,0)<0)return 0;
 for(unsigned i=0;a[i];){
  while(a[i]==' ')i++;
  unsigned j=0;while(key[j]&&a[i+j]==key[j])j++;
  if(!key[j]&&((j&&key[j-1]=='=')||a[i+j]==' '||!a[i+j]))return 1;
  while(a[i]&&a[i]!=' ')i++;
 }
 return 0;
}
/* /private/var on ramfs, laid out by the template taken from iOS (hakios sysimage/var.skel):
 * "d MODE UID GID PATH" or "l TARGET PATH", tab separated. */
static void var_skeleton(int console) {
 char b[6144];long fd=SC(SYS_open,"/private/preboot/etc/var.skel",O_RDONLY,0,0,0,0);
 if(fd<0){put(console,"init: no var.skel\n");return;}
 long n=SC(SYS_read,fd,b,sizeof(b)-1,0,0,0);SC(SYS_close,fd,0,0,0,0,0);
 if(n<=0)return;
 b[n]=0;unsigned made=0;
 for(char *line=b;*line;) {
  char *f[5];unsigned k=0;char *p=line;
  while(*p&&*p!='\n'&&k<5){f[k++]=p;while(*p&&*p!='\t'&&*p!='\n')p++;if(*p=='\t')*p++=0;}
  char *next=*p?p+1:p;*p=0;
  if(k==5&&f[0][0]=='d') {
   long mode=0;for(char *m=f[1];*m;m++)mode=mode*8+(*m-'0');
   SC(SYS_mkdir,f[4],mode,0,0,0,0);SC(SYS_chmod,f[4],mode,0,0,0,0);SC(SYS_chown,f[4],atol_(f[2]),atol_(f[3]),0,0,0);made++;
  } else if(k==3&&f[0][0]=='l')SC(SYS_symlink,f[1],f[2],0,0,0,0);
  line=next;
 }
 put(console,"init: /private/var entries ");putn(console,made);put(console,"\n");
 step(console,"/private/var skeleton",made?0:-2);
}
/* Each step on the console and, through kern.jm21_log, in the kernel log the USB monitor reads. */
static int step(int console,const char *what,long r) {
 put(console,"init: ");put(console,what);put(console,r<0?" FAILED errno ":" ok\n");
 if(r<0){putn(console,-r);put(console,"\n");}
 char line[160];unsigned n=0;const char *p="init: ";
 while(*p)line[n++]=*p++;
 for(p=what;*p&&n<120;)line[n++]=*p++;
 for(p=r<0?" FAILED errno ":" ok";*p;)line[n++]=*p++;
 if(r<0){char d[12];unsigned i=0;unsigned long v=(unsigned long)-r;do{d[i++]=(char)('0'+v%10);v/=10;}while(v);while(i)line[n++]=d[--i];}
 line[n]=0;SC(SYS_sysctlbyname,"kern.jm21_log",13,0,0,line,n);
 /* Created only by the post-restore repair below: bounded boot evidence even
  * when the kernel ring buffer rolls over before the USB host connects. */
 long log=SC(SYS_open,"/private/var/log/jm21-init.log",O_WRONLY|O_APPEND,0,0,0,0);
 if(log>=0){put(log,line);put(log,"\n");SC(SYS_close,log,0,0,0,0,0);}
 return r>=0;
}
/* These mobile daemons failed on EACCES after restoring system containers
 * made by root, or ENOENT when opening CoreData stores with missing parents.
 * Provision after var_store load so legacy snapshots receive the repair.
 * Repair only their observed private trees;
 * leave the shared container parents and other services' ownership alone. */
static void service_private_directories(int console) {
 long log=SC(SYS_open,"/private/var/log/jm21-init.log",O_WRONLY|O_CREAT|O_TRUNC,0600,0,0,0);
 if(log>=0)SC(SYS_close,log,0,0,0,0,0);
 const char *paths[]={
  "/private/var/containers/Data/System/5CC3B9C5-E378-4EF4-9484-6A0FD3FB3CAB",
  "/private/var/containers/Data/System/5CC3B9C5-E378-4EF4-9484-6A0FD3FB3CAB/Library",
  "/private/var/containers/Data/System/5CC3B9C5-E378-4EF4-9484-6A0FD3FB3CAB/Library/internal",
  "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd",
  "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd/Library",
  "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd/Library/InstallCoordination",
  "/private/var/mobile/Library/Application Support/com.apple.remotemanagementd",
  "/private/var/mobile/Library/Application Support/com.apple.ap.adprivacyd",
  "/private/var/mobile/Library/Application Support/CloudKitMailConversationsFlags",
  "/private/var/mobile/Library/Application Support/videosubscriptionsd"
 };
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
  long r=SC(SYS_mkdir,paths[i],0700,0,0,0,0);
  step(console,"private directory mkdir",r);
  if(r==-17)r=0; /* EEXIST: restored directories still need their owner repaired. */
  if(r>=0){r=SC(SYS_chown,paths[i],501,501,0,0,0);step(console,"private directory chown",r);}
  if(r>=0){r=SC(SYS_chmod,paths[i],0700,0,0,0,0);step(console,"private directory chmod",r);}
  step(console,paths[i],r);
 }
 /* analyticsd's private CATasking fallback must also survive legacy restores. */
 const char *analytics[]={"/private/var/db/analyticsd",
  "/private/var/db/analyticsd/Library",
  "/private/var/db/analyticsd/Library/CoreAnalytics"};
 for(unsigned i=0;i<sizeof(analytics)/sizeof(analytics[0]);i++){
  long r=SC(SYS_mkdir,analytics[i],0700,0,0,0,0);
  if(r==-17)r=0;
  if(r>=0)r=SC(SYS_chown,analytics[i],263,263,0,0,0);
  if(r>=0)r=SC(SYS_chmod,analytics[i],0700,0,0,0,0);
  step(console,analytics[i],r);
 }
}
/* boot-arg jm21_ios=1: the iOS root of userdata (/dev/mmcroot) becomes the root and Apple's
 * launchd process 1; this image goes to /private/preboot, and its LaunchDaemons (the USB bridge)
 * are all launchd starts. Returns only on failure: 0 before the switch, 1 after it. */
static int ios_boot(int console) {
 if(!step(console,"mount apfs /dev/mmcroot /mnt",SC(SYS_mount,"apfs","/mnt",1,"/dev/mmcroot",0,0)))return 0;
 step(console,"mount devfs /mnt/dev",SC(SYS_mount,"devfs","/mnt/dev",0,"devfs",0,0));
 /* /System/Library/LaunchDaemons is a symlink to /redwood/LaunchDaemons (hakios system image) */
 if(!step(console,"bind /redwood",SC(SYS_mount,"bindfs","/mnt/redwood",1,"/redwood",0,0)))return 0;
 step(console,"bind NanoLaunchDaemons",SC(SYS_mount,"bindfs","/mnt/System/Library/NanoLaunchDaemons",1,"/empty",0,0));
 /* Keep the extension-point cache while excluding the stock launchd job cache. */
 if(!step(console,"bind xpc extension cache",SC(SYS_mount,"bindfs","/mnt/System/Library/xpc",1,"/redwood/xpc",0,0)))return 0;
 /* where the old root goes; the volume is read only, so a synthetic directory if it has none */
 const char *pre="/mnt/private/preboot";
 step(console,"synthetic /private/preboot",SC(SYS_sysctlbyname,"kern.apfs_synthetic",19,0,0,pre,slen(pre)));
 const char *sw="/mnt private/preboot";
 if(!step(console,"switch root",SC(SYS_sysctlbyname,"kern.hakios_switch_root",23,0,0,sw,slen(sw)))) return 0;
 step(console,"chdir new root",SC(SYS_chdir,"/",0,0,0,0,0));
 /* No RTC is exposed yet. Do not let persisted Biome history open in 1970;
  * network time may refine this floor later. Never move a newer clock back. */
 struct {long sec; int usec; int pad;} wall={0,0,0};
 if(SC(SYS_gettimeofday,&wall,0,0,0,0,0)>=0 && wall.sec<JM21_BUILD_EPOCH){
  wall.sec=JM21_BUILD_EPOCH;wall.usec=0;
  step(console,"initial wall clock",SC(SYS_settimeofday,&wall,0,0,0,0,0));
 }
 const char *cache_parent="/System/Library/Caches";
 step(console,"synthetic cache parent",SC(SYS_sysctlbyname,"kern.apfs_synthetic",19,0,0,cache_parent,slen(cache_parent)));
 const char *cache_link="/System/Library/Caches/com.apple.dyld=/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld";
 step(console,"synthetic default dyld cache",SC(SYS_sysctlbyname,"kern.apfs_synthetic",19,0,0,cache_link,slen(cache_link)));
 /* Publish the opt-in Metal bundle without modifying the Apple system volume. */
 long metalfd=SC(SYS_open,"/redwood/Metal/HakiosMetal.bundle/Info.plist",O_RDONLY,0,0,0,0);
 if(metalfd>=0){
  SC(SYS_close,metalfd,0,0,0,0,0);
  /* Synthetic aliases only work within one APFS mount. Redwood is bindfs. */
  const char *mp="/System/Library/Extensions/HakiosMetal.bundle";
  step(console,"synthetic Metal mountpoint",SC(SYS_sysctlbyname,"kern.apfs_synthetic",19,0,0,mp,slen(mp)));
  step(console,"bind Metal plugin",SC(SYS_mount,"bindfs",mp,1,"/redwood/Metal/HakiosMetal.bundle",0,0));
  const char *vl="/System/Library/PrivateFrameworks/VectorKit.framework/metal_libraries";
  step(console,"synthetic VectorKit libraries",SC(SYS_sysctlbyname,"kern.apfs_synthetic",19,0,0,vl,slen(vl)));
  if(!step(console,"bind VectorKit libraries",SC(SYS_mount,"bindfs",vl,1,"/redwood/Metal/VectorKit.framework/metal_libraries",0,0)))return 1;
 }
 /* dyld's preboot cryptex path must also resolve after md0 moves to /private/preboot. */
 step(console,"bind OS cryptex in preboot",SC(SYS_mount,"bindfs","/private/preboot/Cryptexes/OS",1,"/System/Cryptexes/OS",0,0));
 long cache=SC(SYS_open,"/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64",O_RDONLY,0,0,0,0);
 step(console,"open dyld shared cache",cache);
 if(cache>=0)SC(SYS_close,cache,0,0,0,0,0);
 int live_data=boot_arg("mmc_data=")||boot_arg("var_hfs=1");
 if(live_data){
  /* Stop before launchd on mount/receipt failure. Do not silently boot an empty
   * RAM tree and later replace a user's persistent state with that tree. */
  long pid=sys_fork();
  if(!pid){char *va[]={"data-volume",0};char *ve[]={0};
   SC(SYS_execve,"/private/preboot/bin/data-volume",va,ve,0,0,0);SC(SYS_exit,127,0,0,0,0,0);}
  int w=0;long r=pid>0?SC(SYS_wait4,pid,&w,0,0,0,0):pid;
  if(!step(console,"persistent HFSX Data",r<0?r:(w?-1:0)))return 1;
 }else if(!step(console,"ramfs /private/var",SC(SYS_mount,"ramfs","/private/var",0,0,0,0)))return 1;
 var_skeleton(console);
 const char *markers[]={"/private/var/db/diagnostics/.migrated","/private/var/db/diagnostics/timesync/.migrated","/private/var/db/uuidtext/.migrated"};
 for(unsigned i=0;i<3;i++){
  long fd=SC(SYS_open,markers[i],O_WRONLY|O_CREAT,0644,0,0,0);
  if(fd>=0){SC(SYS_chown,markers[i],272,272,0,0,0);SC(SYS_close,fd,0,0,0,0,0);}
 }
 /* jm21_migrated=1 (JM21_MIGRATED): tell DataMigration this build (20F66) was already migrated, so
  * SpringBoard's checkInAndWaitForDataMigration need not wait for the plugins. Written before
  * var_store load, so a persisted /private/var keeps its own copy. */
 if(!live_data&&boot_arg("jm21_migrated=1")){
  const char *mp="/private/var/mobile/Library/Preferences/com.apple.migration.plist";
  static const char mx[]="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict><key>LastSystemVersion</key><string>20F66</string><key>LastBuildVersion</key><string>20F66</string></dict></plist>\n";
  long fd=SC(SYS_open,mp,O_WRONLY|O_CREAT|O_TRUNC,0644,0,0,0);
  if(fd>=0){SC(SYS_write,fd,mx,sizeof(mx)-1,0,0,0);SC(SYS_close,fd,0,0,0,0,0);SC(SYS_chown,mp,501,501,0,0,0);}
  step(console,"migration marker",fd<0?fd:0);
 }
 /* Persistent /private/var (JM21_PERSIST): unpack the newest good archive from userdata before
  * any daemon starts. A missing or torn store leaves the fresh skeleton as it is. */
 if(!live_data&&boot_arg("var_store=1")){
  long pid=sys_fork();
  if(!pid){char *va[]={"var_store","load",0};char *ve[]={0};
   SC(SYS_execve,"/private/preboot/bin/var_store",va,ve,0,0,0);SC(SYS_exit,127,0,0,0,0,0);}
  int w=0;long r=pid>0?SC(SYS_wait4,pid,&w,0,0,0,0):pid;
  step(console,"var_store load",r<0?r:(w?-1:0));
 }
 service_private_directories(console);
 /* launchd opens each job's console after selecting its user (e.g. mobile). */
 step(console,"console access for iOS daemons",SC(SYS_chmod,"/dev/console",0666,0,0,0,0));
 /* Optional Home profile: launchd rejects DYLD_ variables in jobs, so use
  * the kernel's insertion table before executing Apple launchd. */
 char inject[4096];
 long ifd=SC(SYS_open,"/private/preboot/etc/inject.conf",O_RDONLY,0,0,0,0);
 if(ifd>=0){
  long n=SC(SYS_read,ifd,inject,sizeof(inject)-1,0,0,0);
  SC(SYS_close,ifd,0,0,0,0,0);
  if(n>0){
   int vnodes=32768;
   step(console,"Home vnode capacity",SC(SYS_sysctlbyname,"kern.maxvnodes",14,0,0,&vnodes,sizeof(vnodes)));
   unsigned claim[4]={1,1,0,0};
   step(console,"Home owns framebuffer",SC(SYS_sysctlbyname,"kern.jm21_fb",12,0,0,claim,sizeof(claim)));
   while(n>0&&(inject[n-1]=='\n'||inject[n-1]==' '))n--;
   for(long i=0;i<n;i++)if(inject[i]=='\n')inject[i]=';';
   inject[n]=0;
   step(console,"Home library insertion",SC(SYS_sysctlbyname,"kern.exec_inject",16,0,0,inject,n+1));
  }
 }
 /* hakios' system image keeps Apple's launchd as launchd.ios (userland/build.sh): its
  * /sbin/launchd is stage0, which wants a second RAM disk (/dev/md1 on /hakios) and halts. */
 char *argv[]={"launchd",0};char *envp[]={0};
 step(console,"exec /sbin/launchd.ios next",0);
 step(console,"exec /sbin/launchd.ios",SC(SYS_execve,"/sbin/launchd.ios",argv,envp,0,0,0));
 return 1;
}
int umain(long *sp) {
 struct bridge s;memset(&s,0,sizeof(s));
 int argc=(int)sp[0];char **argv=(char **)(sp+1);
 s.sh=argc>1?argv[1]:"/bin/sh";      /* a LaunchDaemon passes the shell of the old root */
 int console=(int)SC(SYS_open,"/dev/console",O_RDWR|O_NOCTTY,0,0,0,0);
 /* Apple launchd inherits these descriptors across exec; keep its diagnostics visible. */
 if(console>=0) {
  SC(SYS_dup2,console,0,0,0,0,0);SC(SYS_dup2,console,1,0,0,0,0);SC(SYS_dup2,console,2,0,0,0,0);
 }
 if(SC(SYS_getpid,0,0,0,0,0,0)==1) {
  char args[1024]={0};unsigned long an=sizeof(args)-1;
  SC(SYS_sysctlbyname,"kern.bootargs",13,args,&an,0,0);
  int ios=0;for(unsigned i=0;args[i];i++)if(args[i]=='j'){const char *k="jm21_ios=1";unsigned j=0;while(k[j]&&args[i+j]==k[j])j++;if(!k[j])ios=1;}
  if(ios&&ios_boot(console))s.sh="/private/preboot/bin/sh";
 }
 put(console,"\nFIIO XNU: APFS root, PID 1 pty bridge for /bin/sh\n");
 s.master=(int)SC(SYS_open,"/dev/ptmx",O_RDWR|O_NOCTTY,0,0,0,0);
 if(s.master<0||SC(SYS_ioctl,s.master,TIOCPTYGRANT,0,0,0,0)<0||SC(SYS_ioctl,s.master,TIOCPTYUNLK,0,0,0,0)<0||
    SC(SYS_ioctl,s.master,TIOCPTYGNAME,s.tty,0,0,0)<0) {
  put(console,"init: no pty\n");publish(&s,'R',"no pty");for(;;)sleep_ms(1000);
 }
 SC(SYS_fcntl,s.master,4,O_NONBLOCK,0,0,0);
 spawn(&s,console);
 put(console,"init: /bin/sh on ");put(console,s.tty);put(console,"\n");
 ready(&s);int idle=1;
 for(;;) {
  char out[960];long n=SC(SYS_read,s.master,out,sizeof(out),0,0,0);
  if(n>0){s.from_pty+=(unsigned long)n;output(&s,out,(unsigned)n);idle=0;continue;}
  /* WNOHANG|WUNTRACED: a stopped shell would never read again; replace it. */
  int w=0;if(SC(SYS_wait4,s.shell,&w,3,0,0,0)==s.shell) {
   s.status=(unsigned)w;
   if((w&0xff)==0x7f){SC(SYS_kill,s.shell,9,1,0,0,0);SC(SYS_wait4,s.shell,&w,0,0,0,0);}
   put(console,"init: shell ended, restarting\n");spawn(&s,console);
  }
  if(!idle){ready(&s);idle=1;}
  char b[256]={0};
  if(mailbox(&s,b,0)>=0&&wire_valid(b)&&b[0]=='C') {
   unsigned len=wire_len(b)-15;s.lost=0;
   long wr=SC(SYS_write,s.master,b+6,len,0,0,0);if(wr>0)s.to_pty+=(unsigned long)wr;
   ready(&s);
  }
  sleep_ms(10);
 }
}
