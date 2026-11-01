/* Mount provisioned JM21 Data. Explicit dm=1 permits a one-time import into
 * an empty HFSX volume. Never format here or fall back to an empty RAM tree. */
#include "mini.h"

/* Clang lowers aggregate zero initialization to this Darwin entry point. */
void bzero(void *p,size_t n) { memset(p,0,n); }

struct data_statfs {
 uint32_t bsize; int32_t iosize;
 uint64_t blocks, bfree, bavail, files, ffree;
 int32_t fsid[2]; uint32_t owner, type, flags, subtype;
 char fstype[16], on[1024], from[1024]; uint32_t reserved[8];
};
_Static_assert(sizeof(struct data_statfs)==2168, "Darwin statfs64 ABI");
struct data_mount_args {
 const char *device; uint32_t uid,gid; uint16_t mask,pad;
 uint32_t encoding; int32_t timezone[2],flags,tbuffer,jflags,disable;
};
_Static_assert(sizeof(struct data_mount_args)==48, "Darwin hfs_mount_args ABI");

static uint32_t be32(const uint8_t *p) {
 return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static int fail(const char *message) {
 printf("data-volume: %s; launchd will not start, USB recovery remains available\n",message);
 return 1;
}
static int migration_requested(void) {
 char args[1024]={0};size_t n=sizeof(args)-1;
 if(sysctlbyname("kern.bootargs",args,&n,NULL,0)!=0)return 0;
 for(char *p=args;*p;) {
  while(*p==' ')p++;
  if(!memcmp(p,"dm=1",4)&&(p[4]==0||p[4]==' '))return 1;
  while(*p&&*p!=' ')p++;
 }
 return 0;
}
static int synchronize_data(void) {
 mini_syscall(36,0,0,0,0,0,0); /* sync pending data before the device barrier */
 int d=open("/private/var",O_RDONLY,0);
 if(d<0)return 0;
 /* F_FULLFSYNC: flush this filesystem and the underlying eMMC cache. */
 int rc=(int)mini_syscall(92,d,51,0,0,0,0);
 close(d);return rc==0;
}
int main(int argc,char **argv,char **envp) {
 (void)envp;
 if(argc==2 && !strcmp(argv[1],"sync")) {
  struct data_statfs fs={0};
  if(mini_syscall(345,(long)"/private/var",(long)&fs,0,0,0,0)!=0 ||
     strcmp(fs.fstype,"hfs") || strcmp(fs.from,"/dev/mmcdata"))return 1;
  return synchronize_data()?0:1;
 }
 uint8_t header[512]; uint64_t sectors=0; uint32_t sector_size=0;
 int fd=open("/dev/rmmcdata",O_RDONLY,0);
 if(fd<0) return fail("no dedicated Data window");
 /* DKIOCGETBLOCKSIZE / DKIOCGETBLOCKCOUNT, from this kernel's sys/disk.h. */
 int ok=mini_syscall(SYS_ioctl,fd,0x40046418,(long)&sector_size,0,0,0)==0 &&
        mini_syscall(SYS_ioctl,fd,0x40086419,(long)&sectors,0,0,0)==0 &&
        pread(fd,header,sizeof(header),1024)==sizeof(header);
 close(fd);
 if(!ok || sector_size!=512 || sectors>UINT64_MAX/512) return fail("cannot establish Data geometry");
 /* HFSX v5, journaled, exact device size; no HFS wrapper/embedded volume. */
 uint32_t block_size=be32(header+40), blocks=be32(header+44);
 if(header[0]!='H' || header[1]!='X' || header[2]!=0 || header[3]!=5 ||
    !(be32(header+4)&0x2000) || block_size!=4096 ||
    (uint64_t)block_size*blocks!=sectors*512) return fail("Data is not the expected journaled HFSX");
 struct data_mount_args args={0}; args.device="/dev/mmcdata";args.flags=4;
 if(mount("hfs","/private/var",0,&args)!=0) return fail("HFS mount failed");
 struct data_statfs fs={0};
 /* SYS_statfs64=345; match the actual mount, not a boot-argument assumption. */
 if(mini_syscall(345,(long)"/private/var",(long)&fs,0,0,0,0)!=0 ||
    strcmp(fs.fstype,"hfs") || strcmp(fs.from,"/dev/mmcdata") ||
    strcmp(fs.on,"/private/var") || (fs.flags&1) || !(fs.flags&0x00800000))
  return fail("Data mount is not writable and journaled");
 /* Created only after a separate, verified migration. Never import the old
  * archive over a live Data tree, including after a failed boot. */
 static const char receipt[]="JM21-HFSX-DATA-v1\n";
 char found[sizeof(receipt)]={0};
 fd=open("/private/var/.jm21-data-ready",O_RDONLY,0);
 ssize_t n=fd<0?-1:read(fd,found,sizeof(found));
 if(fd>=0)close(fd);
 if(n!=sizeof(receipt)-1 || memcmp(found,receipt,sizeof(receipt)-1)) {
  /* Fresh newfs_hfs creates only the journal files. A partial import is never
   * automatically replayed over existing data: re-provisioning is explicit. */
  if(n>=0 || !migration_requested() || be32(header+32)>2 || be32(header+36)>1)
   return fail("Data has no receipt and is not an explicitly selected empty migration target");
  pid_t pid=fork();
  if(pid==0) {
   char *const va[]={"var_store","migrate",NULL};char *const ve[]={NULL};
   execve("/private/preboot/bin/var_store",va,ve);_exit(127);
  }
  int status=0;
  if(pid<0 || wait4(pid,&status,0,NULL)!=pid || !WIFEXITED(status) || WEXITSTATUS(status))
   return fail("archive migration failed; existing archive remains untouched");
  if(!synchronize_data())return fail("migration full synchronization failed");
  fd=open("/private/var/.jm21-data-ready",O_WRONLY|O_CREAT|O_EXCL,0600);
  if(fd<0)return fail("cannot create migration receipt");
  int done=write(fd,receipt,sizeof(receipt)-1)==sizeof(receipt)-1 && fsync(fd)==0;
  close(fd);
  if(!done || !synchronize_data())return fail("migration receipt synchronization failed");
  printf("data-volume: archive migrated, files read back, receipt committed\n");
 }
 /* These directories belong to this boot. Avoid recursively deleting a tree
  * on persistent storage; a fresh RAM mount also discards stale sockets. */
 static const char *const transient[]={"/private/var/tmp","/private/var/run","/private/var/vm"};
 for(unsigned i=0;i<sizeof(transient)/sizeof(transient[0]);i++) {
  if(mkdir(transient[i],0755)!=0 && errno!=17) return fail("cannot create runtime directory");
  if(mount("ramfs",transient[i],0,NULL)!=0) return fail("cannot isolate runtime directory");
 }
 printf("data-volume: journaled HFSX, %llu MiB total, %llu MiB available; archive restore disabled\n",
        (unsigned long long)(fs.blocks*fs.bsize>>20),(unsigned long long)(fs.bavail*fs.bsize>>20));
 return 0;
}
