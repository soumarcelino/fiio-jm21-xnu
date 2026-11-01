#include <stdint.h>
#include "font.h"
#include "loader.h"
#ifdef JM21_SILENT
#include "boot-logo.h"
#endif
#define W 750u
#define H 1334u
#define FB 0x5c000000ul
static unsigned pitch, bpp, swap, row;
#ifdef JM21_SILENT
static unsigned screen_quiet = 1;
#else
static unsigned screen_quiet;
#endif
static void pixel(unsigned x,unsigned y,uint32_t c) {
 if(x>=W || y>=H) return;
 volatile uint8_t *p=(volatile uint8_t *)(FB+y*pitch+x*bpp);
 p[0]=swap ? c>>16 : c; p[1]=c>>8; p[2]=swap ? c : c>>16;
 if(bpp==4) p[3]=255;
}
static void rect(unsigned x,unsigned y,unsigned w,unsigned h,uint32_t c) {
 if(x>=W || y>=H)return;
 unsigned end=x+w<W?x+w:W, bottom=y+h<H?y+h:H;
 uint32_t color=0xff000000u|c;
 for(unsigned j=y;j<bottom;j++) {
  volatile uint32_t *dst=(volatile uint32_t *)(FB+j*pitch);
  for(unsigned i=x;i<end;i++)dst[i]=color;
 }
}
static void text(unsigned x,unsigned y,const char *s) {
 while(*s && x+16<=W) {
  unsigned ch=(unsigned char)*s++;
  if(ch<32 || ch>126) ch='?';
  for(unsigned j=0;j<16;j++) for(unsigned i=0;i<8;i++)
   for(unsigned dy=0;dy<2;dy++) for(unsigned dx=0;dx<2;dx++)
   pixel(x+2*i+dx,y+2*j+dy,(font[ch-32][j]&(128u>>i)) ? 0xffffff : 0x101820);
  x+=16;
 }
}
/* Error-only snapshot. Normal USB operation never paints over XNU logs. */
void usb_screen_status(const char *stage,uint64_t address,uint32_t value,unsigned fault) {
 char detail[]="MMIO 0000000000000000 = 00000000 FAULT 0";
 for(unsigned i=0;i<16;i++)detail[5+i]="0123456789ABCDEF"[(address>>(60-4*i))&15];
 for(unsigned i=0;i<8;i++)detail[24+i]="0123456789ABCDEF"[(value>>(28-4*i))&15];
 detail[39]=fault?'1':'0';
 rect(0,H-440,W,440,0x101820);
 text(8,H-436,stage);text(8,H-400,detail);
 __asm__ volatile("dsb sy":::"memory");
}
void usb_screen_detail(unsigned row,const char *message) {
 if(row<10)text(8,H-364+row*36,message);
 __asm__ volatile("dsb sy":::"memory");
}
void putc_screen(char c);
void line(const char *s) { while(*s) putc_screen(*s++); putc_screen('\n'); }
void hex(const char *name,uint64_t v) {
 char s[19]="0x0000000000000000";
 for(unsigned i=0;i<16;i++) s[17-i]="0123456789ABCDEF"[(v>>(i*4))&15];
 while(*name) {putc_screen(*name++);} putc_screen(' ');line(s);
}
#define READ(r) ({ uint64_t v; __asm__ volatile("mrs %0," #r : "=r"(v)); v; })
static unsigned col;
void putc_screen(char c) {
 if(screen_quiet) return;
 if(c=='\r') return;
 if(c=='\n' || col>=44) {
  col=0; row+=36;
  if(row+32>=H) {
   volatile uint32_t *fb=(volatile uint32_t *)FB;
   for(unsigned i=0;i<W*(H-36);i++) fb[i]=fb[i+W*36];
   rect(0,H-36,W,36,0x101820); row-=36;
  }
  if(c=='\n') return;
 }
 char str[2]={c,0}; text(16+16*col++,row,str);
 __asm__ volatile("dsb sy" ::: "memory");
}
void failure(uint64_t log,uint64_t len,uint64_t reason,uint64_t esr,uint64_t elr,uint64_t far) {
 /* Preserve the return log for USB even when the framebuffer scrolls it away. */
 if(log>=0x40000000 && len<=1536*1024 && log+len<=0x100000000UL) {
  volatile uint8_t *capture=(volatile uint8_t *)0x8a000000;
  uint64_t count=len<0x7ffff?len:0x7ffff;
  for(uint64_t i=0;i<count;i++)capture[i]=*(volatile uint8_t *)(log+i);
  capture[count]=0;
  __asm__ volatile("dsb sy":::"memory");
 }
 screen_quiet=0;
 rect(0,0,W,H,0x101820); row=12;col=0;
 line("XNU STOP / DEBUG CAPTURE");
 if(len>1536*1024)len=1536*1024;
 if(log>=0x40000000 && log+len<=0x100000000UL)
  for(uint64_t i=0;i<len;i++)putc_screen(*(volatile char *)(log+i));
 putc_screen('\n');hex("REASON",reason);hex("ESR / CODE",esr);hex("ELR",elr);hex("FAR",far);
 for(;;)__asm__ volatile("wfe");
}
void boot_error(void) {
 screen_quiet=0;row=24;col=0;
 rect(0,0,W,H,0x101820);
}
void main(uint64_t dtb,uint64_t el) {
 (void)dtb;
 pitch=3000;bpp=4;swap=0;row=24;
 rect(0,0,W,H,screen_quiet ? 0 : 0x101820);
#ifdef JM21_SILENT
 for(unsigned i=0;i<sizeof(boot_logo)/sizeof(boot_logo[0]);i+=3)
  rect(boot_logo[i+1],boot_logo[i],boot_logo[i+2]-boot_logo[i+1],1,0xffffff);
 __asm__ volatile("dsb sy":::"memory");
#endif
 if(!screen_quiet) rect(16,8,W-32,4,0x42c8b0);
 line("FiiO JM21  /  Darwin");
 line("Native boot console");
 hex("ENTRY EL",el>>2);hex("BOOT CPU",READ(mpidr_el1));
 if(el!=4){boot_error();line("STOP: EL1 REQUIRED");for(;;)__asm__ volatile("wfe");}
 uint64_t args=load_xnu();
 (void)args;
}
