/* SWEETXNU payload format: ios-topaz tools/mkpayload.py. JM21-specific loader. */
#include <stdint.h>
#include "loader.h"
#include "usb.h"
#include "payload_size.h"
#include "usb-kernel.h"
struct seg { uint64_t va,size,off,len; };
struct payload { char magic[8]; uint32_t ver,n; uint64_t virt,entry,adto,adtn,end,phys,mem; char cmd[608]; struct seg segs[]; };
struct args { uint16_t rev,ver; uint64_t virt,phys,mem,top; uint64_t video[6]; uint32_t machine; uint64_t dt; uint32_t dtn; char cmd[608]; uint64_t flags,actual; };
_Static_assert(sizeof(struct args)==736,"boot args size");
extern char _end[],secondary_entry[],xnu_return[];
int sdcc_dma_bypass(void);
int wlan_dma_bypass(void);
static volatile uint64_t entry_pa,bootargs;
static void stop(const char *s) {boot_error();line(s);line("LOADER STOPPED: USB MONITOR UP");usb_monitor();for(;;)__asm__ volatile("wfe");}
static void zero(uint64_t dst,uint64_t n) {while(n && (dst&7)) {*(volatile uint8_t *)dst++=0; n--;}
 while(n>=8) {*(volatile uint64_t *)dst=0; dst+=8; n-=8;}
 while(n--) *(volatile uint8_t *)dst++=0;}
static void copy(uint64_t dst,uint64_t src,uint64_t n) {if(!((dst|src)&7)) while(n>=8) {
  *(volatile uint64_t *)dst=*(volatile uint64_t *)src; dst+=8; src+=8; n-=8;
 }
 while(n--) *(volatile uint8_t *)dst++=*(volatile uint8_t *)src++;}
#define RD(r) ({uint64_t v;__asm__ volatile("mrs %0," #r:"=r"(v));v;})
static uint64_t smc(uint64_t fn,uint64_t a,uint64_t b) {
 register uint64_t x0 __asm__("x0")=fn,x1 __asm__("x1")=a,x2 __asm__("x2")=b,x3 __asm__("x3")=0;
 __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3)::"x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");return x0;
}
void gold_main(void) {
 hex("XNU CPU",RD(mpidr_el1));hex("CPU FEATURES",RD(id_aa64mmfr1_el1));
 *(volatile uint32_t *)0xf200000 |= 0x13;
 __asm__ volatile("dsb sy; ic iallu; dsb sy; isb":::"memory");
 line("ENTERING REAL XNU / VERBOSE CONSOLE");
 ((void (*)(uint64_t))entry_pa)(bootargs);
 stop("XNU RETURNED");
}
uint64_t load_xnu(void) {
 uint64_t src=((uint64_t)_end+4095)&~4095UL;
 const struct payload *p=(void *)src;
 const char *magic="SWEETXNU";
 for(unsigned i=0;i<8;i++)if(p->magic[i]!=magic[i])stop("BAD PAYLOAD MAGIC");
 if(p->ver!=1 || p->n>32 || p->phys!=0x80000000 || p->mem!=0x80000000 ||
    p->adto>PAYLOAD_SIZE || p->adtn>PAYLOAD_SIZE-p->adto)stop("BAD PAYLOAD HEADER");
 if(src+PAYLOAD_SIZE>p->phys)stop("SOURCE OVERLAPS XNU RAM");
 uint64_t delta=p->phys-p->virt;
 uint64_t dt=p->end+delta,ba=(dt+p->adtn+0x3fff)&~0x3fffUL;
 if(dt<p->phys || ba+0x4000>p->phys+p->mem)stop("BOOT ARGS OUTSIDE RAM");
 for(unsigned i=0;i<p->n;i++) {
  const struct seg *s=&p->segs[i]; uint64_t dst=s->va+delta;
  if(s->len>s->size || s->off>PAYLOAD_SIZE || s->len>PAYLOAD_SIZE-s->off ||
     dst<p->phys || dst>dt || s->size>dt-dst)stop("SEGMENT OUTSIDE RAM");
  copy(dst,src+s->off,s->len);zero(dst+s->len,s->size-s->len);
  /* Early exceptions return to our physical console; normal panic retains text. */
  for(uint64_t k=0;k+32<=s->len;k+=8) {
   volatile uint64_t *q=(void *)(dst+k);
   if(q[0]==0x545352444c554e58UL){q[1]=(uint64_t)xnu_return;q[2]=0;q[3]=delta;}
  }
 }
 /* Ramdisk (APFS + mailbox padding), ADT and boot args end below the panic region. */
 if(ba+0x4000>0x8a000000)stop("RAMDISK TOO LARGE");
 /* Keep the previous boot's panic log (a reset leaves RAM): monitor 'lastpanic' reads it. */
 copy(0x8a080000,0x8a000000,0x80000);zero(0x8a000000,0x80000);
 copy(dt,src+p->adto,p->adtn);zero(ba,0x4000);
 volatile struct args *a=(void *)ba;
 a->rev=2;a->ver=2;a->virt=p->virt;a->phys=p->phys;a->mem=p->mem;a->actual=p->mem;a->top=0x8a100000;
#if XNU_JM21_LOW_RAM
 /* Keep the kernel where it was loaded; extend its linear aperture downwards.
  * The kernel releases only explicitly allowed low banks after VM startup. */
 extern char _head[];
 if((uint64_t)_head<0x40000000 || (uint64_t)_end>0x40100000)stop("LOW RAM OVERLAPS MONITOR");
 a->virt-=0x40000000;a->phys=0x40000000;a->mem=0xc0000000;
 a->actual=0xbe280000; /* DT's installed banks, excluding the 29.5 MiB hole. */
#endif
 a->video[0]=0x5c000000;a->video[1]=0;a->video[2]=3000;a->video[3]=750;a->video[4]=1334;a->video[5]=32;
#ifdef JM21_SILENT
 a->video[1]=1; /* PE_create_console: graphical mode, preserve loader logo. */
#endif
 a->dt=dt-delta;a->dtn=p->adtn;
 for(unsigned i=0;i<608;i++)a->cmd[i]=p->cmd[i];
 entry_pa=p->entry+delta;bootargs=ba;
 if(entry_pa<p->phys || entry_pa>=dt)stop("ENTRY OUTSIDE KERNEL");
 hex("KERNEL ENTRY",entry_pa);hex("BOOT ARGS",ba);line("START GOLD CORE 0x100");
 hex("SDCC SMMU BYPASS",(uint64_t)sdcc_dma_bypass());
 hex("WLAN SMMU BYPASS",(uint64_t)wlan_dma_bypass());
 __asm__ volatile("dsb sy; ic iallu; dsb sy; isb":::"memory");
 uint64_t r=smc(0xc4000003,0x100,(uint64_t)secondary_entry);
 if(r) {hex("PSCI CPU_ON FAILED",r);stop("XNU NOT ENTERED");}
 usb_monitor();
 for(;;)__asm__ volatile("wfe");
}
