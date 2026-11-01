/* Minimal read-only VFS additions for mockfs's fixed lookup namespace.
 * ABI: xnu-8796.121.2 bsd/sys/vnode_if.h, struct vnop_readdir_args.
 * sbin aliases the root node in upstream mockfs_lookup. */
extern long uio_offset(void *);
extern long uio_resid(void *);
extern int uiomove(const void *,int,void *);
struct args {void *desc,*vp,*uio;int flags;int *eof,*count;void *context;};
struct entry {unsigned long ino,seek;unsigned short reclen,namlen;unsigned char type;char name[11];};
static const struct entry entries[]={
 {1,32,32,1,4,"."},{1,64,32,2,4,".."},{1,96,32,4,4,"sbin"},
 {2,128,32,3,4,"dev"},{3,160,32,7,8,"launchd"}
};
int shell_vfs_nop(void *a) {(void)a;return 0;}
int shell_vfs_readdir(struct args *a) {
 long off=uio_offset(a->uio);int count=0,error=0;
 if(off<0||off>160||off%32)return 22;
 while(off<160&&uio_resid(a->uio)>=32) {
  const struct entry *e=&entries[off/32];
  if(a->flags&1)error=uiomove(e,32,a->uio);
  else {unsigned char b[32]={0};unsigned ino=(unsigned)e->ino;for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(ino>>(8*i));b[4]=32;b[6]=e->type;b[7]=(unsigned char)e->namlen;for(unsigned i=0;i<e->namlen;i++)b[8+i]=e->name[i];error=uiomove(b,32,a->uio);}
  if(error)break;
  off+=32;count++;
 }
 if(a->eof)*a->eof=off==160;
 if(a->count)*a->count=count;
 return error;
}
