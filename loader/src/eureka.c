/* Freestanding Darwin userspace graphics. Claim the inherited framebuffer and
 * stream BGRX rows through the root-only, bounds-checked JM21 kernel sysctl.
 * No GPU, dyld, writable globals, physical-address access, or libc required. */
#include "ulib.h"
#include "eureka-art.h"
struct fb_info {
 unsigned int version,width,height,rowbytes,format,max_pixels,console_owned,writes;
};
struct fb_span {
 unsigned int version,op,x,y;
 unsigned int pixels[750];
};
static long send(struct fb_span *s,unsigned long size) {
 return SC(SYS_sysctlbyname,"kern.jm21_fb",12,0,0,s,size);
}
static void log_(const char *message) {
 SC(SYS_sysctlbyname,"kern.jm21_log",13,0,0,message,slen(message));
}
static int failure(const char *what,long r) {
 log_("eureka: graphics operation failed; query shell for errno\n");
 put(2,"eureka: ");put(2,what);put(2,": errno ");putn(2,-r);put(2,"\n");return 1;
}
int umain(long *sp) {
 int argc=(int)sp[0];char **argv=(char **)(sp+1);
 struct fb_span span;memset(&span,0,sizeof(span));span.version=1;
 if(argc==2&&seq(argv[1],"--release")) {
  span.op=2;long r=send(&span,16);return r<0?failure("release",r):0;
 }
 if(argc>1&&!(argc==2&&seq(argv[1],"--info"))){put(2,"usage: eureka [--info|--release]\n");return 2;}
 struct fb_info info;unsigned long n=sizeof(info);
 long r=SC(SYS_sysctlbyname,"kern.jm21_fb_info",17,&info,&n,0,0);
 if(r<0)return failure("framebuffer info",r);
 if(n!=sizeof(info)){put(2,"eureka: invalid framebuffer info size\n");return 1;}
 if(argc==2) {
  const char *labels[]={"version","width","height","rowbytes","format","max_pixels","owned","spans"};
  unsigned int *values=(unsigned int *)&info;
  for(unsigned i=0;i<8;i++){put(1,labels[i]);put(1,": ");putn(1,values[i]);put(1,"\n");}
  return 0;
 }
 if(n!=sizeof(info)||info.version!=1||info.width!=750||info.height!=1334||
    info.rowbytes!=3000||info.format!=1||info.max_pixels<750) {
  put(2,"eureka: unsupported framebuffer layout\n");return 1;
 }
 span.op=1;r=send(&span,16);if(r<0)return failure("claim",r);
 unsigned run=0;
 for(unsigned y=0;y<info.height;y++) {
  memset(span.pixels,0,sizeof(span.pixels));span.op=3;span.x=0;span.y=y;
  unsigned color=y>=910?0xf2f2f5:
   y<496?0x61bb46:y<558?0xfdb827:y<621?0xf5821f:
   y<683?0xe03a3e:y<746?0x963d97:0x009ddc;
  while(art[run]!=65535) {
   unsigned x=art[run++],end=art[run++];
   for(;x<end;x++)span.pixels[x]=color;
  }
  run++;
  r=send(&span,sizeof(span));
  if(r<0){span.op=2;send(&span,16);return failure("draw row",r);}
 }
 /* One-shot LaunchDaemon: successful exit leaves the picture on the panel. */
 log_("eureka: rainbow apple and Eureka drawn (750x1334 BGRX)\n");
 put(1,"eureka: rainbow apple and Eureka drawn (750x1334 BGRX)\n");
 return 0;
}
