#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/ucontext.h>
#include <mach-o/loader.h>
static struct { uintptr_t lo,hi; const char *name; } images[256];
static unsigned imageCount;
static void imageAdded(const struct mach_header *h,intptr_t slide) {
 if(h->magic!=MH_MAGIC_64 || imageCount>=256)return;
 const struct mach_header_64 *mh=(const void *)h;
 const struct load_command *lc=(const void *)(mh+1);
 uintptr_t hi=(uintptr_t)h;
 for(unsigned i=0;i<mh->ncmds;i++) {
  if(lc->cmd==LC_SEGMENT_64) { const struct segment_command_64 *sg=(const void *)lc; if(!strcmp(sg->segname,"__TEXT"))hi=slide+sg->vmaddr+sg->vmsize; }
  lc=(const void *)((const char *)lc+lc->cmdsize);
 }
 const char *name="?";
 for(unsigned i=0;i<_dyld_image_count();i++)if(_dyld_get_image_header(i)==h) {name=_dyld_get_image_name(i);break;}
 images[imageCount++]=(typeof(images[0])){(uintptr_t)h,hi,name};
}
static void fault(int sig,siginfo_t *info,void *ctx) {
 ucontext_t *u=ctx;
 dprintf(2,"Maps fault %d addr=%p pc=%llx lr=%llx\n",sig,info?info->si_addr:NULL,
 (unsigned long long)u->uc_mcontext->__ss.__pc,(unsigned long long)u->uc_mcontext->__ss.__lr);
 uintptr_t pc=u->uc_mcontext->__ss.__pc;
 for(unsigned i=0;i<imageCount;i++)if(pc>=images[i].lo && pc<images[i].hi)
  dprintf(2,"image=%s base=%llx offset=%llx\n",images[i].name,(unsigned long long)images[i].lo,(unsigned long long)(pc-images[i].lo));
 for(int i=0;i<29;i++) dprintf(2,"x%d=%llx ",i,(unsigned long long)u->uc_mcontext->__ss.__x[i]);
 dprintf(2,"\nfp=%llx sp=%llx\n",(unsigned long long)u->uc_mcontext->__ss.__fp,(unsigned long long)u->uc_mcontext->__ss.__sp);
 signal(sig,SIG_DFL); raise(sig);
}
static void exception(NSException *e) { NSLog(@"Maps uncaught %@: %@\n%@",e.name,e.reason,e.callStackSymbols); }
__attribute__((constructor)) static void setup(void) {
 _dyld_register_func_for_add_image(imageAdded);
 int fd=open("/private/var/tmp/jm21-maps-render.log",O_WRONLY|O_CREAT|O_APPEND,0644);
 if(fd>=0) { dup2(fd,2); close(fd); }
 setenv("OS_ACTIVITY_DT_MODE","YES",1);
 struct sigaction sa={0}; sa.sa_sigaction=fault; sa.sa_flags=SA_SIGINFO|SA_RESETHAND;
 sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL); sigaction(SIGABRT,&sa,NULL);
 NSSetUncaughtExceptionHandler(exception);
 NSLog(@"Maps render diagnostics active pid=%d",getpid());
}
