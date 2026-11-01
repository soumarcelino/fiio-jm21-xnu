/* JM21 bare-metal USB diagnostic monitor. No XNU or UEFI services.
 * DWC3 register layout: Linux drivers/usb/dwc3/{core.h,gadget.h,ep0.c}.
 * Platform addresses/SID: JM21 Android device tree. USB2 only; retain ABL
 * clocks/PHY/regulators. Failure is local to this CPU, XNU runs on Gold.
 */
#include <stdint.h>
#include "usb.h"
#include "loader.h"
#include "usb-kernel.h"
#include "shell-wire.h"
#define USB 0x04e00000UL
#define SMMU 0x0c600000UL
#define BIT(n) (1u<<(n))
#define GCTL 0xc110
#define GSNPSID 0xc120
#define PHY2 0xc200
#define PHY3 0xc2c0
#define DCFG 0xc700
#define DCTL 0xc704
#define DEVTEN 0xc708
#define DSTS 0xc70c
#define DALEPENA 0xc720
#define EVENT_BYTES 4096u
#define DATA_BYTES 1024u
extern uint32_t probe_read32(uint64_t);
extern void probe_write32(uint64_t,uint32_t);
extern volatile uint32_t usb_faulted;
static uint64_t last_address;
static uint32_t last_value;
static unsigned stopped;
static const char *stage;
static char diagnostics[10][45];
static unsigned diagnostic_count;
static void record(const char *name,uint32_t a,uint32_t b) {
 unsigned slot=diagnostic_count++%10,n=0;
 char *out=diagnostics[slot];
 while(*name && n<25)out[n++]=*name++;
 out[n++]=' ';
 for(unsigned i=0;i<8;i++)out[n++]="0123456789ABCDEF"[(a>>(28-i*4))&15];
 out[n++]=' ';
 for(unsigned i=0;i<8;i++)out[n++]="0123456789ABCDEF"[(b>>(28-i*4))&15];
 out[n]=0;
}
static uint32_t read32(uint64_t address) {
 if(usb_faulted)return 0;
 last_address=address;last_value=0;
 last_value=probe_read32(address);return last_value;
}
static void write32(uint64_t address,uint32_t value) {
 if(usb_faulted)return;
 last_address=address;last_value=value;probe_write32(address,value);
}
static uint32_t get(uint32_t off) { return read32(USB+off); }
static void set(uint32_t off,uint32_t v) { write32(USB+off,v); }
static void status(void) {
 if(!stopped)return;
 char label[45];unsigned n=0;
 if(stopped) {const char *p="STOP ";while(*p)label[n++]=*p++;}
 const char *p=stage;while(*p && n<44)label[n++]=*p++;
 label[n]=0;usb_screen_status(label,last_address,last_value,usb_faulted);
 unsigned count=diagnostic_count<10?diagnostic_count:10;
 for(unsigned i=0;i<count;i++)
  usb_screen_detail(i,diagnostics[(diagnostic_count-count+i)%10]);
}
static void barrier(void) { __asm__ volatile("dsb sy":::"memory"); }
static uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0,cntpct_el0":"=r"(v));return v; }
static uint64_t frequency(void) { uint64_t v; __asm__ volatile("mrs %0,cntfrq_el0":"=r"(v));return v; }
static void delay(unsigned ms) { uint64_t end=ticks()+frequency()*ms/1000;while(ticks()<end)__asm__ volatile("yield"); }
static int wait_bits(unsigned off,uint32_t mask,uint32_t want) {
 uint64_t end=ticks()+frequency()/10;
 do { if((get(off)&mask)==want && !usb_faulted)return 1; } while(!usb_faulted && ticks()<end);
 record("WAIT off / actual",off,last_value);
 record("WAIT mask / wanted",mask,want);
 return 0;
}
struct trb { uint32_t lo,hi,size,ctrl; };
static volatile uint32_t events[EVENT_BYTES/4] __attribute__((aligned(4096)));
static volatile struct trb trb[2] __attribute__((aligned(64)));
static volatile uint8_t setup[64] __attribute__((aligned(64)));
static volatile uint8_t data[DATA_BYTES] __attribute__((aligned(64)));
static uint8_t log_snapshot[64*1024] __attribute__((aligned(64)));
static unsigned log_size;
static unsigned resource[2],active[2];
static unsigned event_pos,phase,three_stage,status_ep,configured,pending_config,reboot_pending;
static uint64_t boot_ticks;
static int healthy=1;
/* EP0-only, vendor-specific diagnostic interface. Experimental local VID/PID;
 * not a registered identity for product distribution. No bulk/TTY implied. */
static const uint8_t device_desc[]={18,1,0,2,0,0,0,64,0x09,0x12,0x21,0x4a,0,1,1,2,3,1};
static const uint8_t qualifier_desc[]={10,6,0,2,0,0,0,64,1,0};
static const uint8_t config_desc[]={9,2,18,0,1,1,0,0x80,50,9,4,0,0,0,0xff,0,0,0};
static unsigned text(const char *s) { unsigned n=0;while(*s && n<DATA_BYTES)data[n++]=(uint8_t)*s++;return n; }
static unsigned number(unsigned n,uint64_t v) {
 char b[24];unsigned count=0;do {b[count++]=(char)('0'+v%10);v/=10;}while(v);
 while(count && n<DATA_BYTES)data[n++]=(uint8_t)b[--count];
 return n;
}
static int command(unsigned ep,unsigned cmd,uint32_t p0,uint32_t p1,uint32_t p2) {
 unsigned base=0xc800+ep*16;
 /* Endpoint commands cross into the PHY clock domain. Linux also clears
  * SUSPHY/ENBLSLPM immediately before issuing each command. */
 set(PHY2,get(PHY2)&~(BIT(6)|BIT(8)));
 record("CMD ep / opcode",ep,cmd);
 record("PAR0 / PAR1",p0,p1);
 record("PAR2 / PHY2",p2,get(PHY2));
 set(base,p2);set(base+4,p1);set(base+8,p0);barrier();set(base+12,cmd|BIT(10));
 if(!wait_bits(base+12,BIT(10),0) || (get(base+12)&0xf000)) {record("CMD result / status",last_value,(last_value>>12)&15);
  stage="USB: endpoint command failed";healthy=0;return 0;}
 return 1;
}
static int ep_config(unsigned ep,unsigned modify) {
 return command(ep,1,(64<<3)|(modify?2u<<30:0),BIT(8)|BIT(10)|(ep<<25),0);
}
static int transfer(unsigned ep,const volatile void *buf,unsigned size,unsigned type) {
 uint64_t addr=(uint64_t)buf,t=(uint64_t)&trb[ep];
 trb[ep].lo=(uint32_t)addr;trb[ep].hi=(uint32_t)(addr>>32);
 trb[ep].size=size;trb[ep].ctrl=BIT(0)|BIT(1)|BIT(10)|BIT(11)|(type<<4);
 barrier();
 if(!command(ep,6,(uint32_t)(t>>32),(uint32_t)t,0))return 0;
 resource[ep]=(get(0xc80c+ep*16)>>16)&127;active[ep]=1;return 1;
}
static void setup_start(void) { phase=0;three_stage=0;transfer(0,setup,8,2); }
static void stall(void) { reboot_pending=0;pending_config=0;command(0,4,0,0,0);setup_start(); }
/* Qualcomm firmware can rewrite BYPASS to FAULT (arm-smmu-qcom.c).
 * Emulate bypass through a disabled, unreferenced S1 context, as Linux does.
 * Unlike Linux's full initialization, preserve all firmware-owned contexts.
 */
static int qcom_dma_bypass(unsigned stream,unsigned groups,uint32_t original) {
 stage="USB: Qualcomm SMMU bypass";status();
 uint32_t id=read32(SMMU+0x24);
 unsigned shift=(id&BIT(31))?16:12,banks=id&255,s2banks=(id>>16)&255;
 uint64_t page=1UL<<shift,base=page<<(((id>>28)&7)+1);
 if(usb_faulted || !banks || banks>128 || s2banks>=banks ||
    base>=0x80000 || banks*page>0x80000-base)return 0;
 static uint8_t used[128]; /* Monitor initialization runs once per boot. */
 for(unsigned i=0;i<groups;i++) {
  uint32_t smr=read32(SMMU+0x800+4*i),route=read32(SMMU+0xc00+4*i);
  if(usb_faulted)return 0;
  if((smr&BIT(31)) && ((route>>16)&3)==0) {
   unsigned cb=route&255;if(cb>=banks)return 0;
   used[cb]=1;
  }
 }
 for(unsigned cb=banks;cb-->s2banks;) {
  if(used[cb])continue;
  uint64_t control=SMMU+base+cb*page,cbar=SMMU+page+4*cb;
  uint32_t sctlr=read32(control);
  if(usb_faulted)return 0;
  /* SCTLR.M alone enables translation. Firmware may leave fault-reporting
   * bits (observed 0xe0) set on an otherwise disabled, unreferenced bank. */
  if(sctlr&BIT(0))continue;
  write32(control,0);write32(cbar,BIT(16));barrier();
  if(usb_faulted || read32(control)!=0 || read32(cbar)!=BIT(16))return 0;
  /* S2CR TRANS selects the context; SCTLR.M=0 and CBAR S2 bypass
   * make its DMA addresses physical without a translation table. */
  uint32_t route=(original&~((3u<<16)|255u))|cb;
  write32(SMMU+0xc00+4*stream,route);barrier();
  uint32_t actual=read32(SMMU+0xc00+4*stream);
  return !usb_faulted && (actual&((3u<<16)|255u))==cb;
 }
 stage="USB: no unused SMMU context";return 0;
}
/* Exact SID only. Do not disable the SMMU globally or alter a shared mapping.
 * Quiesce DWC3 before this call. Existing translations cannot map our buffers.
 */
static int dma_bypass_sid(unsigned want) {
 uint32_t id=read32(SMMU+0x20),cr=read32(SMMU);
 if(usb_faulted || !(id&BIT(27)))return 0; /* stream matching required */
 unsigned count=id&255,free_index=count,match=count;
 if(!count || count>128)return 0;
 for(unsigned i=0;i<count;i++) {
  uint32_t smr=read32(SMMU+0x800+4*i);
  if(!(smr&BIT(31))) {if(free_index==count)free_index=i;continue;}
  unsigned mask=(smr>>16)&0x7fff,sid=smr&0x7fff;
  if(((sid^want)&~mask)==0) {
   if(mask || match!=count)return 0;
   match=i;
  }
 }
 if(usb_faulted)return 0;
 unsigned i=match==count?free_index:match;
 if(i==count)return 0;
 /* EXIDENABLE changes validity semantics; unsupported on this first port. */
 if(cr&BIT(3))return 0;
 uint32_t s2cr=read32(SMMU+0xc00+4*i);
 write32(SMMU+0xc00+4*i,(s2cr&~(3u<<16))|BIT(16));
 barrier();
 uint32_t actual=read32(SMMU+0xc00+4*i);
 if(usb_faulted)return 0;
 if(((actual>>16)&3)!=1) {
  if(((actual>>16)&3)!=2 || !qcom_dma_bypass(i,count,s2cr))return 0;
 }
 if(match==count)write32(SMMU+0x800+4*i,BIT(31)|want);
 barrier();
 return !usb_faulted;
}
static int dma_bypass(void) {return dma_bypass_sid(0x120);}
/* SDCC1 (eMMC, iommus <0x7e 0xc0 0x00>): same bypass for its SID, before XNU starts,
 * so the kernel's ADMA2 self-test (mmc_dma=1) sees physical addresses. A fault here
 * must not poison the USB monitor that runs afterwards. */
int sdcc_dma_bypass(void) {int r=dma_bypass_sid(0xc0);usb_faulted=0;return r;}
/* WCN3990 copy engines (icnss iommus <0x7e 0x1a0 0x01>): SIDs 0x1a0/0x1a1 to physical, for the JM21 Wi-Fi
 * data path (hakios.jm21_wifi.c gives the radio physical addresses). Unused SIDs otherwise: harmless. */
int wlan_dma_bypass(void) {int r=dma_bypass_sid(0x1a0)|dma_bypass_sid(0x1a1)<<1;usb_faulted=0;return r;}
/* Qualcomm MSM wrapper, core_base + 0xf8800 (dwc3-msm.c).
 * USB2-only operation must use UTMI instead of an unavailable PIPE clock.
 * Sequence and delays match dwc3_qcom_select_utmi_clk in Linux. */
static int usb2_wrapper(void) {
 unsigned general=0xf8808,hs=0xf8810;
 uint32_t initial=get(general),session=get(hs);
 record("QSCRATCH cfg / HS",initial,session);
 if(usb_faulted)return 0;
 set(general,initial|BIT(8));barrier();delay(1);
 set(general,get(general)|BIT(0)|BIT(3));barrier();delay(1);
 set(general,get(general)&~BIT(8));
 /* Match the Qualcomm peripheral VBUS override. No VBUS sourcing. */
 set(hs,get(hs)|BIT(20)|BIT(28));barrier();
 uint32_t actual=get(general),valid=get(hs);
 record("QSCRATCH new / HS",actual,valid);
 return !usb_faulted && (actual&(BIT(0)|BIT(3)|BIT(8)))==(BIT(0)|BIT(3)) &&
        (valid&(BIT(20)|BIT(28)))==(BIT(20)|BIT(28));
}
/* Pinned XNU msgbuf ABI: bsd/sys/msgbuf.h (8796). No arbitrary memory API.
 * Only pointers in the known linear kernel mapping are accepted. Capture once
 * before USB chunk reads; the producer never waits for this monitor.
 */
static uint64_t pointer_at(uint64_t pa) {
 return (uint64_t)read32(pa)|((uint64_t)read32(pa+4)<<32);
}
static uint64_t kernel_pa(uint64_t va,unsigned length) {
 if(va<XNU_VIRT_BASE || va-XNU_VIRT_BASE>=XNU_RAM_SIZE)return 0;
 uint64_t pa=va-XNU_VIRT_BASE+XNU_PHYS_BASE;
 if(length>0x100000000UL-pa)return 0;
 return pa;
}
static unsigned capture_log(void) {
 log_size=0;
 uint64_t p=kernel_pa(pointer_at(XNU_MSGBUFP_PA),24);
 if(!p || usb_faulted)return text("XNU msgbuf unavailable\n");
 uint32_t magic=read32(p),size=read32(p+4),write=read32(p+8);
 uint64_t base=kernel_pa(pointer_at(p+16),size);
 if(magic!=0x63061 || !size || size>1024*1024 || write>=size || !base)
  return text("XNU msgbuf not initialized or outside linear mapping\n");
 unsigned captured=size;if(captured>sizeof(log_snapshot))captured=sizeof(log_snapshot);
 for(unsigned i=0;i<captured;i++) {
  uint64_t addr=base+(write+size-captured+i)%size;
  uint32_t v=read32(addr&~3UL);
  if(usb_faulted)return text("XNU msgbuf read fault\n");
  log_snapshot[i]=(uint8_t)(v>>((addr&3)*8));
 }
 log_size=captured;
 unsigned n=text("Log snapshot bytes: ");n=number(n,captured);data[n++]='\n';return n;
}
/* Qualcomm cpufreq-hw v1 (Linux qcom-cpufreq-hw.c, qcom_soc_data: LUT 0x110,
 * 32-byte rows, perf_state 0x920, current_vote 0x704). Hardware sequences
 * voltage and LMH limits; the loader only selects the highest non-turbo row.
 * Aborts here mark the domain unavailable instead of disabling USB probes. */
static const uint64_t cpufreq_base[2]={0xf521000,0xf523000};
static uint32_t cpufreq_mhz[2][12],cpufreq_rows[2],cpufreq_info[2][5];
static int cpufreq_fault;
static uint32_t cpufreq_read(uint64_t a) {uint32_t v=probe_read32(a);if(usb_faulted){usb_faulted=0;cpufreq_fault=1;}return v;}
static void cpufreq_max(void) {
 for(unsigned d=0;d<2;d++) {
  uint64_t b=cpufreq_base[d];cpufreq_fault=0;
  uint32_t enable=cpufreq_read(b),before=cpufreq_read(b+0x920),prev=0,best=~0u,n=0;
  for(unsigned i=0;i<12&&!cpufreq_fault;i++) {
   uint32_t v=cpufreq_read(b+0x110+32*i),lval=v&255,mhz=(v>>30)?lval*192/10:300;
   if(i&&mhz==prev)break;
   cpufreq_mhz[d][n++]=mhz;prev=mhz;
   if((v>>30) && ((v>>16)&7)!=1 && lval>=10 && lval<=160)best=i;
  }
  cpufreq_rows[d]=n;
  /* Gold reads enable=0 yet sits at row 0 (300 MHz, measured) and accepts
   * perf_state like Linux; a valid LUT is the precondition, enable is reported. */
  if(!cpufreq_fault && best!=~0u) {
   /* Step through another row: an unchanged index written before the
    * domain was enabled is not acted on. */
   for(unsigned k=0;k<2;k++) {
    probe_write32(b+0x920,k?best:best-1);if(usb_faulted){usb_faulted=0;cpufreq_fault=1;}
    delay(1);
   }
  }
  uint32_t after=cpufreq_read(b+0x920),vote=cpufreq_read(b+0x704);
  cpufreq_info[d][0]=cpufreq_fault?2:(enable&1);cpufreq_info[d][1]=before;cpufreq_info[d][2]=best;
  cpufreq_info[d][3]=after;cpufreq_info[d][4]=vote;
  record(d?"CPUFREQ GOLD idx/vote":"CPUFREQ SILVER idx/vote",after,vote);
 }
}
static unsigned pon_reasons(void);
static unsigned append(unsigned n,const char *s) {while(*s && n<DATA_BYTES)data[n++]=(uint8_t)*s++;return n;}
static unsigned append_hex(unsigned n,uint32_t v) {for(unsigned i=0;i<8&&n<DATA_BYTES;i++)data[n++]=(uint8_t)"0123456789abcdef"[(v>>(28-i*4))&15];return n;}
/* eMMC prototype on sdhc_1 (qcom,sdhci-msm-v5; Linux sdhci.c/sdhci-msm.c), read-only,
 * PIO through the buffer data port: no DMA, so the SMMU stays untouched. ABL leaves
 * the card selected in transfer state; GCC SDCC1 branches (khaje-gcc, sm6115 offsets)
 * must report on before any SDHCI access, since a gated block can hang the bus. */
#define SDHC 0x4744000UL
#define SDHC_CQHCI 0x4745000UL
#define GCC_SDCC1_APPS 0x1438004UL
#define GCC_SDCC1_AHB 0x1438008UL
static int emmc_fault;
static uint32_t emmc_last_err,emmc_last_resp;
static uint32_t emmc_r(uint64_t a) {uint32_t v=probe_read32(a);if(usb_faulted){usb_faulted=0;emmc_fault=1;}return v;}
static void emmc_w(uint64_t a,uint32_t v) {probe_write32(a,v);if(usb_faulted){usb_faulted=0;emmc_fault=1;}}
static int emmc_usable(void) {
 emmc_fault=0;
 uint32_t apps=emmc_r(GCC_SDCC1_APPS),ahb=emmc_r(GCC_SDCC1_AHB);
 if(emmc_fault || (apps>>31) || (ahb>>31))return 0;
 return !(emmc_r(SDHC_CQHCI+8)&1) && !emmc_fault; /* CQHCI_CFG.ENABLE: legacy commands only without CQE */
}
static int emmc_wait(uint32_t mask) {
 uint64_t end=ticks()+frequency()/10;
 for(;;) {
  uint32_t s=emmc_r(SDHC+0x30);
  if(emmc_fault)return 0;
  if(s&0x8000){emmc_last_err=s;return 0;}
  if(s&mask){emmc_w(SDHC+0x30,s&mask);return 1;}
  if(ticks()>end){emmc_last_err=0xdead0000|s;return 0;}
 }
}
/* Command register flags (SDHCI 2.2.7): response 136/48/48-busy, CRC/index check, data. */
#define R_NONE 0x00
#define R2 0x09
#define R3 0x02
#define R1 0x1a
#define R1B 0x1b
static void emmc_reset_lines(void) {uint32_t c=emmc_r(SDHC+0x2c);emmc_w(SDHC+0x2c,c|(6u<<24));delay(1);}
static int emmc_cmd(unsigned idx,unsigned flags,uint32_t arg,unsigned mode) {
 uint64_t end=ticks()+frequency()/10;
 while(emmc_r(SDHC+0x24)&((flags&0x20)||(flags&3)==3?3:1)) if(emmc_fault||ticks()>end){emmc_last_err=0xb5590000u|idx;return 0;}
 emmc_w(SDHC+0x30,0xffffffffu);
 emmc_w(SDHC+0x08,arg);
 emmc_w(SDHC+0x0c,((idx<<8|flags)<<16)|mode);
 if(!emmc_wait(1)){emmc_last_err|=idx<<24;emmc_reset_lines();return 0;}
 emmc_last_resp=emmc_r(SDHC+0x10);
 if((flags&3)==3) { /* R1b: wait for DAT0 to leave busy */
  end=ticks()+frequency();
  while(!(emmc_r(SDHC+0x24)&(1u<<20))) if(ticks()>end){emmc_last_err=0xb0590000u|idx;return 0;}
 }
 return !emmc_fault;
}
/* SDHCI divider on the 200 MHz base (caps): 10-bit N, f = base/(2N). Timeout at max. */
static int emmc_clock(unsigned div) {
 uint32_t c=emmc_r(SDHC+0x2c)&0xff000000u;
 emmc_w(SDHC+0x2c,c|(0xeu<<16)|((div&0xff)<<8)|(((div>>8)&3)<<6)|1);
 uint64_t end=ticks()+frequency()/10;
 while(!(emmc_r(SDHC+0x2c)&2)) if(ticks()>end){emmc_last_err=0xc10c0000u;return 0;}
 emmc_w(SDHC+0x2c,emmc_r(SDHC+0x2c)|4);
 delay(1);
 return !emmc_fault;
}
/* Bus power: 1.8 V select, then on (xnu-xzs/aboot order); ack the msm power request (Linux pwr_irq). */
static void emmc_power(void) {
 uint32_t h=emmc_r(SDHC+0x28)&~0xff00u;
 emmc_w(SDHC+0x28,h|(0x0au<<8));emmc_w(SDHC+0x28,h|(0x0bu<<8));
 for(unsigned i=0;i<10;i++) {
  uint32_t st=emmc_r(SDHC+0x240)&0xf;
  if(st){emmc_w(SDHC+0x248,st);emmc_w(SDHC+0x24c,(st&2?1:0)|(st&8?4:0)|(st&1?1:0)|(st&4?4:0));}
  delay(1);
 }
}
static uint32_t emmc_ocr,emmc_cid[4];
static int emmc_ready_state;
/* Full card init, as Linux mmc_attach_mmc: the ABL leaves bus power and card clock off. */
static int emmc_init(void) {
 if(emmc_ready_state)return 1;
 if(!emmc_usable())return 0;
 emmc_w(SDHC+0x34,0xffff00ffu);emmc_w(SDHC+0x38,0);
 emmc_power();
 if(!emmc_clock(250))return 0;
 delay(2);                                   /* >= 74 clocks at 400 kHz */
 emmc_cmd(0,R_NONE,0,0);delay(2);
 uint64_t end=ticks()+frequency();
 do {if(!emmc_cmd(1,R3,0x40ff8080u,0))return 0;emmc_ocr=emmc_last_resp;if(ticks()>end){emmc_last_err=0x0c100000u;return 0;}delay(5);} while(!(emmc_ocr>>31));
 if(!emmc_cmd(2,R2,0,0))return 0;
 for(unsigned i=0;i<4;i++)emmc_cid[i]=emmc_r(SDHC+0x10+4*i);
 if(!emmc_cmd(3,R1,1u<<16,0))return 0;
 if(!emmc_cmd(7,R1B,1u<<16,0))return 0;
 if(!emmc_clock(4))return 0;                 /* 25 MHz, 1-bit, default speed */
 if(!emmc_cmd(16,R1,512,0))return 0;
 emmc_ready_state=1;return 1;
}
/* CMD17 READ_SINGLE_BLOCK, sector addressing. Returns 1 with 128 words in out. */
static int emmc_read(uint32_t lba,uint32_t *out) {
 if(!emmc_init())return 0;
 emmc_w(SDHC+0x04,(1u<<16)|0x200);
 if(!emmc_cmd(17,R1|0x20,lba,0x10))return 0;
 int ok=emmc_wait(0x20);
 if(ok)for(unsigned i=0;i<128;i++)out[i]=emmc_r(SDHC+0x20);
 if(ok)ok=emmc_wait(2);
 if(!ok)emmc_reset_lines();
 return ok&&!emmc_fault;
}
static unsigned emmc_report(void) {
 emmc_fault=0;
 unsigned n=append(0,"gcc sdcc1 apps ");n=append_hex(n,emmc_r(GCC_SDCC1_APPS));
 n=append(n," ahb ");n=append_hex(n,emmc_r(GCC_SDCC1_AHB));
 n=append(n,"\nusable ");n=number(n,(uint64_t)emmc_usable());
 if(emmc_usable()) {
  n=append(n,"\nhost ver ");n=append_hex(n,emmc_r(SDHC+0xfc));
  n=append(n," caps ");n=append_hex(n,emmc_r(SDHC+0x40));
  n=append(n," present ");n=append_hex(n,emmc_r(SDHC+0x24));
  n=append(n," hostctl ");n=append_hex(n,emmc_r(SDHC+0x28));
  n=append(n," clk ");n=append_hex(n,emmc_r(SDHC+0x2c));
  n=append(n," ctl2 ");n=append_hex(n,emmc_r(SDHC+0x3c));
  n=append(n," sigen ");n=append_hex(n,emmc_r(SDHC+0x38));
 }
 n=append(n,"\ninit ");n=number(n,(uint64_t)emmc_init());n=append(n," ocr ");n=append_hex(n,emmc_ocr);
 n=append(n," cid ");for(unsigned i=4;i--;)n=append_hex(n,emmc_cid[i]);
 n=append(n," pwrctl ");n=append_hex(n,emmc_r(SDHC+0x240));
 n=append(n,"\ncqhci cfg ");n=append_hex(n,emmc_r(SDHC_CQHCI+8));
 n=append(n," last err ");n=append_hex(n,emmc_last_err);n=append(n," resp ");n=append_hex(n,emmc_last_resp);
 n=append(n,emmc_fault?" FAULT\n":"\n");
 return n;
}
/* Live: reapplies the maximum, since firmware may enable a domain after boot. */
static unsigned cpufreq_report(void) {
 cpufreq_max();
 unsigned n=0;
 for(unsigned d=0;d<2;d++) {
  const uint32_t *f=cpufreq_info[d];
  n=append(n,d?"gold 0x100-0x103: ":"silver 0x0-0x3: ");
  n=append(n,f[0]==2?"FAULT":f[0]?"enabled":"disabled");
  n=append(n,", perf_state ");n=number(n,f[1]);n=append(n," -> ");n=number(n,f[3]);
  if(f[2]!=~0u && f[2]<cpufreq_rows[d]){n=append(n," (max ");n=number(n,cpufreq_mhz[d][f[2]]);n=append(n," MHz)");}
  n=append(n,", vote raw ");n=number(n,f[4]);n=append(n,"\n  LUT MHz:");
  for(unsigned i=0;i<cpufreq_rows[d];i++){n=append(n," ");n=number(n,cpufreq_mhz[d][i]);}
  n=append(n,"\n");
 }
 return n;
}
/* Hostname is a dedicated mailbox in this pinned, networkless prototype. */
static char shell_stage[240];
static unsigned shell_length;
static void shell_read(char *b) {
 for(unsigned i=0;i<256;i+=4) {uint32_t v=read32(XNU_HOSTNAME_PA+i);for(unsigned j=0;j<4;j++)b[i+j]=(char)(v>>(8*j));}
 b[255]=0;
}
static void shell_write(const char *b) {
 unsigned len=wire_len(b);write32(XNU_HOSTNAME_PA,0);barrier();
 for(unsigned i=4;i<256;i+=4) {uint32_t v=0;for(unsigned j=0;j<4;j++)if(i+j<=len)v|=(uint32_t)(uint8_t)b[i+j]<<(8*j);write32(XNU_HOSTNAME_PA+i,v);}
 barrier();uint32_t v=0;for(unsigned j=0;j<4;j++)v|=(uint32_t)(uint8_t)b[j]<<(8*j);write32(XNU_HOSTNAME_PA,v);barrier();
}
static unsigned shell_request(unsigned cmd,unsigned index) {
 char b[256];shell_read(b);
 if(cmd==11)return text(b[0]?b:"EMPTY");
 if(cmd==9) {if(index==65535){shell_length=0;shell_stage[0]=0;}else {unsigned offset=index>>8;if(offset!=shell_length||offset>=220||(index&255)==0)return text("ERR staging");shell_stage[shell_length++]=(char)index;shell_stage[shell_length]=0;}return text("OK");}
 if(!wire_valid(b))return text("ERR shell unavailable");
 if(cmd==10) {if(b[0]!='R')return text("ERR busy");wire_make(b,'C',index,shell_stage);shell_write(b);return text("OK");}
 if(cmd==12&&b[0]=='O'&&wire_seq(b)==index){wire_make(b,'A',index,"");shell_write(b);return text("OK");}
 return text("ERR frame");
}
static unsigned string_descriptor(unsigned index) {
 const char *s=index==1?"FiiO JM21 experimental":index==2?"XNU USB monitor":index==3?"JM21-XNU-1":0;
 if(index==0) {data[0]=4;data[1]=3;data[2]=9;data[3]=4;return 4;}
 if(!s)return 0;
 unsigned n=2;while(*s && n<254) {data[n++]=(uint8_t)*s++;data[n++]=0;}
 data[0]=(uint8_t)n;data[1]=3;return n;
}
static void handle_setup(void) {
 unsigned type=setup[0],req=setup[1],value=setup[2]|setup[3]<<8,index=setup[4]|setup[5]<<8;
 unsigned length=setup[6]|setup[7]<<8,n=0;
 three_stage=length!=0;status_ep=(type&0x80)?0:1;pending_config=0;reboot_pending=0;
 if(type==0x80 && req==6) {
  const uint8_t *src=0;
  if((value>>8)==1 && !(value&255) && !index) {src=device_desc;n=sizeof(device_desc);}
  else if((value>>8)==6 && !(value&255) && !index) {src=qualifier_desc;n=sizeof(qualifier_desc);}
  else if(((value>>8)==2 || (value>>8)==7) && !(value&255) && !index) {src=config_desc;n=sizeof(config_desc);}
  else if((value>>8)==3)n=string_descriptor(value&255);
  if(src)for(unsigned i=0;i<n;i++)data[i]=src[i];
  if((value>>8)==7 && n)data[1]=7;
  if(!n) {stall();return;}
 } else if(type==0 && req==5 && value<=127 && !index && !length) {
  set(DCFG,(get(DCFG)&~(127u<<3))|(value<<3));
 } else if(type==0 && req==9 && value<=1 && !index && !length) {
  pending_config=value+1;
 } else if(type==0x80 && req==8 && !value && !index && length==1) {data[0]=(uint8_t)configured;n=1;
 } else if((type==0x80 || type==0x81) && req==0 && !value && !index && length==2) {data[0]=0;data[1]=0;n=2;
 } else if(type==0x81 && req==10 && !value && !index && configured && length==1) {data[0]=0;n=1;
 } else if(type==1 && req==11 && !value && !index && configured && !length) {
  /* alternate setting zero */
 } else if(type==0xc0 && req==0x40 && configured && length) {
  if(value==0 && !index)n=text("JM21 monitor protocol 2\nLoader diagnostics + XNU userspace shell (sh).\nCommands: sh info uptime diag cpu emmc resetreason lastpanic log panic screen reboot [bootloader|recovery] poweroff\n");
  else if(value>=9 && value<=12)n=shell_request(value,index);
  else if(value==1 && !index)n=text("FiiO JM21 / native XNU\nUSB2 DWC3 monitor on loader CPU\nXNU userspace shell bridge; no full POSIX sh or SSH.\n");
  else if(value==2 && !index) {n=text("Monitor uptime (seconds): ");n=number(n,(ticks()-boot_ticks)/frequency());data[n++]='\n';}
  else if(value==13 && !index)n=cpufreq_report();
  else if(value==15 && !index)n=emmc_report();
  else if(value==16 && !index)n=pon_reasons();
  else if(value==17 && index<256) { /* read-only word of MDP INTF1 (0x5e6b800, DSI) for scanout timing */
   uint32_t v=*(volatile uint32_t *)(0x5e6b800UL+4UL*index);
   n=append(0,"INTF1+");n=append_hex(n,4UL*index);n=append(n," = ");n=append_hex(n,v);data[n++]='\n';
  }
  else if(value==19 && !index) { /* INTF1 FRAME_COUNT (0xac) and LINE_COUNT (0xb0), 40 samples ~0.5 ms apart */
   uint64_t f=frequency(),t0=ticks();n=0;
   for(unsigned i=0;i<40 && n<DATA_BYTES-48;i++) {
    uint64_t t=ticks();
    uint32_t fc=*(volatile uint32_t *)0x5e6b8acUL,lc=*(volatile uint32_t *)0x5e6b8b0UL;
    n=number(n,(t-t0)*1000000/f);data[n++]=' ';n=append_hex(n,fc);data[n++]=' ';n=append_hex(n,lc);data[n++]='\n';
    while(ticks()-t<f/2000){}
   }
  }
  else if(value==14) { /* index = LBA of the eMMC user area, read-only */
   uint32_t sector[128];
   if(emmc_read(index,sector)){for(unsigned i=0;i<512;i++)data[i]=(uint8_t)(sector[i/4]>>(8*(i%4)));n=512;}
   else {n=append(0,"ERR emmc read ");n=append_hex(n,emmc_last_err);n=append(n," resp ");n=append_hex(n,emmc_last_resp);}
  }
  else if(value==8 && !index) {
   n=text(stage);data[n++]='\n';
   unsigned count=diagnostic_count<10?diagnostic_count:10;
   for(unsigned i=0;i<count;i++) {
    const char *p=diagnostics[(diagnostic_count-count+i)%10];
    while(*p && n<DATA_BYTES-1)data[n++]=(uint8_t)*p++;
    if(n<DATA_BYTES)data[n++]='\n';
   }
  }
  else if(value==3 || value==4) {
   uint64_t offset=(uint64_t)index*DATA_BYTES;
   uint64_t base=value==3?0x8a000000UL:0x5c000000UL,size=value==3?0x100000UL:3000UL*1334; /* panic: current, then the previous boot's copy */
   if(offset>=size) {stall();return;}
   n=(unsigned)(size-offset);if(n>DATA_BYTES)n=DATA_BYTES;
   for(unsigned i=0;i<n;i++)data[i]=*(volatile uint8_t *)(base+offset+i);
  } else if(value==6) {
   unsigned offset=index*DATA_BYTES;
   if(offset>=log_size) {stall();return;}
   n=log_size-offset;if(n>DATA_BYTES)n=DATA_BYTES;
   for(unsigned i=0;i<n;i++)data[i]=log_snapshot[offset+i];
  } else if(value==7 && !index)n=capture_log();
  else if(value==5 && index<=3) {
   n=text("Power request accepted. Target (0=normal 1=bootloader 2=recovery 3=off): ");
   n=number(n,index);data[n++]='\n';
   reboot_pending=index+1;
  }
  else {stall();return;}
 } else {stall();return;}
 if(length) {
  if(!(type&0x80) || !n) {stall();return;}
  if(n>length)n=length;
  /* A short IN data stage that is a whole number of 64-byte packets needs a zero-length packet,
   * which this EP0 does not send: the host then waits for more and times out (shell frames of
   * 128/192 bytes did). Text answers get a newline instead; binary chunks are read whole. */
  if(n<length && !(n%64) && value!=3 && value!=4 && value!=6 && value!=14 && n<DATA_BYTES)data[n++]='\n';
  phase=1;transfer(1,data,n,5);
 } else phase=2; /* wait for host's STATUS NRDY before submitting */
}
static int reset_endpoints(unsigned initial) {
 configured=0;reboot_pending=0;pending_config=0;
 set(DCFG,get(DCFG)&~(127u<<3));
 if(initial) {
  set(DALEPENA,0);
  if(!command(0,9,0,0,0))return 0;
  for(unsigned ep=0;ep<2;ep++)if(!ep_config(ep,0)||!command(ep,2,1,0,0))return 0;
 } else {
  for(unsigned ep=0;ep<2;ep++) {
   if(active[ep] && !command(ep,8|(resource[ep]<<16)|BIT(11),0,0,0))return 0;
   active[ep]=0;
  }
 }
 set(DALEPENA,3);setup_start();return healthy;
}
/* JM21 PM6125 SID0/PON peripheral 08, arbiter v5, application EE0.
 * Linux spmi-pmic-arb.c supplies mapping, ownership and command layout.
 * Never guess a channel: require the firmware mapping and write ownership. */
static int pon_byte(unsigned apid,unsigned reg,unsigned *value,int writing) {
 uint64_t base=writing?0x01e00000UL+0x10000UL*apid:0x03e00000UL+0x80UL*apid;
 if(writing)write32(base+0x10,*value);
 barrier();write32(base,((writing?0u:1u)<<27)|(reg<<4));barrier();
 uint64_t end=ticks()+frequency()/1000;
 unsigned result;
 do {result=read32(base+8);if(usb_faulted)break;if(result&1)break;}while(ticks()<end);
 if(usb_faulted || (result&15)!=1) {record("PON command / status",reg,result);return 0;}
 if(!writing)*value=read32(base+0x18)&255;
 return !usb_faulted;
}
/* SPMI v5 channel of PON (SID0, peripheral 08) owned by EE0; 512 if none. */
static unsigned pon_apid(void) {
 unsigned version=read32(0x01c40000UL);
 record("SPMI arbiter version",version,0);
 if(usb_faulted || version<0x50000000 || version>=0x70000000)return 512;
 for(unsigned i=0;i<512;i++) {
  unsigned map=read32(0x01c40900UL+4*i);
  if(usb_faulted)return 512;
  if(map && ((map>>8)&0xfff)==8 &&
     (read32(0x01c0a700UL+4*i)&7)==0)return i;
 }
 return 512;
}
/* Why the PMIC last powered on or reset (PON gen2: 0xC0 PON_REASON1, 0xC2 WARM_RESET_REASON1,
 * 0xC5 POFF_REASON1, 0xC8-0xC9 FAULT_REASON1-2, 0xCA S3_RESET_REASON); raw, decoded on the host. */
static unsigned pon_reasons(void) {
 unsigned apid=pon_apid(),n=append(0,"PON apid ");
 n=number(n,apid);
 if(apid==512)return append(n," unavailable\n");
 for(unsigned base=0;base<2;base++) {
  unsigned first=base?0xc0:0x08,last=base?0xcf:0x0f;
  n=append(n,base?"\nC0:":"\n08:");
  for(unsigned r=first;r<=last;r++){unsigned v=0;n=append(n," ");if(pon_byte(apid,r,&v,0))n=append_hex(n,v);else n=append(n,"--------");}
 }
 return append(n,"\n");
}
static int pon_restart_reason(unsigned action) {
 unsigned apid=pon_apid();
 if(apid==512 || usb_faulted)return 0;
 unsigned subtype=0,old=0,check=0;
 if(!pon_byte(apid,5,&subtype,0))return 0;
 /* PM6125 uses generation 2 PON; reject unfamiliar register layouts. */
 if(subtype!=4 && subtype!=5) {record("PON unknown subtype",subtype,apid);return 0;}
 if(!pon_byte(apid,0x8f,&old,0))return 0;
 unsigned reason=action==2?2:action==3?1:0;
 unsigned value=(old&1)|(reason<<1);
 if(!pon_byte(apid,0x8f,&value,1) || !pon_byte(apid,0x8f,&check,0))return 0;
 record("PON reason old / new",old,check);
 status();return check==value;
}
/* IMEM address from JM21 DT restart_reason@65c. Qualcomm restart ABI:
 * 77665500 bootloader, 77665501 normal, 77665502 recovery. The PMIC/ABL
 * may require additional platform setup; only hardware validation proves
 * which target actually boots. No slot or persistent boot metadata writes. */
static void reset_system(unsigned action) {
 stage="USB: executing power command";status();
 if(action!=4) {
  if(!pon_restart_reason(action)) {
   stage="USB: PMIC restart reason failed";status();healthy=0;return;
  }
  uint32_t reason=action==2?0x77665500:action==3?0x77665502:0x77665501;
  write32(0x0c12565cUL,reason);barrier();
  if(usb_faulted || read32(0x0c12565cUL)!=reason) {
   stage="USB: restart reason write failed";healthy=0;return;
  }
 }

 register uint64_t x0 __asm__("x0")=action==4?0x84000008:0x84000009;
 __asm__ volatile("smc #0":"+r"(x0)::"x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
 record("PSCI action / result",action,(uint32_t)x0);
 stage="USB: PSCI power call returned";healthy=0;
}
static void event(uint32_t e) {
 if(e&1) {
  if((e&255)!=1)return;
  unsigned kind=(e>>8)&15;
  if(kind==1)reset_endpoints(0);
  else if(kind==2) {
   unsigned speed=get(DSTS)&7;
   if(speed!=0 && speed!=1 && speed!=3) {stage="USB: unsupported link speed";healthy=0;return;}
   ep_config(0,1);ep_config(1,1);
  } else if(kind==11) {stage="USB: event overflow";healthy=0;}
  return;
 }
 unsigned ep=(e>>1)&31,kind=(e>>6)&15,status=(e>>12)&15;
 if(ep>1)return;
 if(kind==1) {
  active[ep]=0;
  if(status&1) {stall();return;}
  barrier();
  if(phase==0 && ep==0)handle_setup();
  else if(phase==1 && ep==1)phase=2;
  else if(phase==3 && ep==status_ep) {
   if(pending_config)configured=pending_config-1;
   if(reboot_pending) {unsigned action=reboot_pending;reboot_pending=0;reset_system(action);return;}
   setup_start();
  }
 } else if(kind==3 && (status&3)==2 && phase==2 && ep==status_ep) {
  phase=3;transfer(ep,data,0,three_stage?4:3);
 }
}
void usb_monitor(void) {
 boot_ticks=ticks();
 stage="CPU: maximum cpufreq-hw state";cpufreq_max();
 stage="USB: probing DWC3";status();
 uint32_t id=get(GSNPSID);
 record("GSNPSID / GCTL",id,get(GCTL));
 if(usb_faulted || ((id>>16)!=0x5533 && (id>>16)!=0x3331))goto failed;
 /* ABL must leave clocks/PHY accessible; no speculative PMIC/clock writes. */
 stage="USB: stopping controller";status();
 set(DCTL,get(DCTL)&~BIT(31));
 if(!wait_bits(DSTS,BIT(22),BIT(22)))goto failed;
 stage="USB: Qualcomm USB2 clock/session";status();
 if(!usb2_wrapper())goto failed;
 stage="USB: configuring SMMU";status();
 if(!dma_bypass())goto failed;
 /* Select device role before its reset: resetting the inactive device
  * block while firmware still owns a different role is insufficient. */
 stage="USB: selecting device role";status();
 set(GCTL,(get(GCTL)&~(3u<<12))|(2u<<12)|BIT(0));
 barrier();
 stage="USB: resetting device";status();
 set(DCTL,(get(DCTL)&~BIT(31))|BIT(30));
 if(!wait_bits(DCTL,BIT(30),0))goto failed;
 /* DWC_usb31 <=1.80a requires 50ms PHY synchronization after reset.
  * Use that conservative settling interval until the IP is identified. */
 delay(50);
 stage="USB: configuring core and PHY";status();
 set(GCTL,(get(GCTL)&~((3u<<12)|BIT(11)))|(2u<<12)|BIT(0));
 /* Keep both PHY interfaces awake during endpoint initialization. Setting
  * PIPE SUSPHY here is not equivalent to disabling SuperSpeed (DCFG does
  * that); it can remove the clock needed to complete endpoint commands.
  * Retain firmware tuning and do not power down either physical PHY. */
 set(PHY2,get(PHY2)&~(BIT(6)|BIT(8)));
 set(PHY3,get(PHY3)&~BIT(17));
 set(DCFG,0); /* high speed, address zero, no LPM */
 set(DEVTEN,7);
 stage="USB: configuring event ring";status();
 unsigned interrupters=(get(0xc144)>>15)&63;
 if(!interrupters || interrupters>32)goto failed;
 for(unsigned i=0;i<interrupters;i++) {
  set(0xc408+i*16,BIT(31));set(0xc40c+i*16,get(0xc40c+i*16)&0xfffc);
 }
 uint64_t addr=(uint64_t)events;
 set(0xc400,(uint32_t)addr);set(0xc404,(uint32_t)(addr>>32));
 set(0xc408,EVENT_BYTES|BIT(31));set(0xc40c,get(0xc40c)&0xfffc);
 stage="USB: configuring endpoint zero";status();
 if(!reset_endpoints(1) || usb_faulted)goto failed;
 stage="USB: starting controller";status();
 barrier();delay(20);set(DCTL,BIT(31));
 if(!wait_bits(DSTS,BIT(22),0))goto failed;
 uint64_t next_status=0;
 while(healthy && !usb_faulted) {
  if(ticks()>=next_status) {
   stage=configured?"USB: configured by host":"USB: running / waiting for host";
   status();next_status=ticks()+frequency();
  }
  unsigned count=get(0xc40c)&0xfffc;
  if(count>EVENT_BYTES) {stage="USB: event ring overflow";break;}
  while(count>=4 && healthy && !usb_faulted) {
   barrier();uint32_t e=events[event_pos/4];
   event_pos=(event_pos+4)%EVENT_BYTES;
   /* Acknowledge only consumed words, never let firmware DMA lap the ring. */
   set(0xc40c,4);count-=4;event(e);
  }
  __asm__ volatile("yield");
 }
 /* Preserve the failing access; cleanup must not replace its diagnostic. */
 if(!usb_faulted) {
  uint64_t address=last_address;uint32_t value=last_value;
  set(DCTL,get(DCTL)&~BIT(31));last_address=address;last_value=value;
 }
failed:
 /* Capture read-only context without losing the original failing access.
  * A synchronous abort forbids subsequent probes. */
 if(!usb_faulted) {
  uint64_t address=last_address;uint32_t value=last_value;
  record("GCTL / DCTL",get(GCTL),get(DCTL));
  record("DSTS / PHY2",get(DSTS),get(PHY2));
  record("PHY3 / DALEPENA",get(PHY3),get(DALEPENA));
  record("IP / HW mode",get(GSNPSID),get(0xc140));
  record("QSCRATCH cfg / HS",get(0xf8808),get(0xf8810));
  last_address=address;last_value=value;
 }
 stopped=1;status();
 /* Display a failure once; never repaint over ongoing XNU output. */
 for(;;)__asm__ volatile("wfe");
}
