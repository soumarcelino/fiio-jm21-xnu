/* FiiO JM21 GT967 (GT9xx): Android DTB + Linux goodix/pinctrl-khaje.
 * Opt-in jm21_touch=1 and jm21_ios=1. GPIO I2C avoids loading GENI firmware.
 * TLMM 0x400000 + WEST 0x100000; SDA6, SCL7, reset71, INT80.
 * Only controller RAM is written; no flash firmware update. */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/errno.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
static int phase, report_count, down_count;
static unsigned poll_ms = 8, poll_us = 8000;
static bool precise_poll;
static uint64_t last_poll_gap_us, max_wake_late_us;
static int touch_poll_us SYSCTL_HANDLER_ARGS {
 int value = (int)poll_us;
 int error = sysctl_handle_int(oidp, &value, 0, req);
 if (error || !req->newptr) return error;
 if (value < 100 || value > 20000) return EINVAL;
 poll_us = (unsigned)value;
 poll_ms = (poll_us + 999) / 1000;
 return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, jm21_touch_poll_us, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
 NULL, 0, touch_poll_us, "I", "GT967 sleep in us, 100..20000; transfer time is additional");
SYSCTL_QUAD(_kern, OID_AUTO, jm21_touch_last_poll_gap_us, CTLFLAG_RD | CTLFLAG_LOCKED, &last_poll_gap_us, "Latest status poll interval including transfer");
SYSCTL_QUAD(_kern, OID_AUTO, jm21_touch_max_wake_late_us, CTLFLAG_RD | CTLFLAG_LOCKED, &max_wake_late_us, "Maximum precise timer overshoot");
static uint64_t poll_count, io_errors, max_poll_gap_us;
SYSCTL_UINT(_kern, OID_AUTO, jm21_touch_poll_ms, CTLFLAG_RD | CTLFLAG_LOCKED, &poll_ms, 0, "GT967 sleep rounded up to milliseconds; poll_us is authoritative");
SYSCTL_QUAD(_kern, OID_AUTO, jm21_touch_polls, CTLFLAG_RD | CTLFLAG_LOCKED, &poll_count, "GT967 status polls");
SYSCTL_QUAD(_kern, OID_AUTO, jm21_touch_io_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &io_errors, "GT967 failed status, report or acknowledge transactions");
SYSCTL_QUAD(_kern, OID_AUTO, jm21_touch_max_poll_gap_us, CTLFLAG_RD | CTLFLAG_LOCKED, &max_poll_gap_us, "Maximum interval between status polls, including sleep, scheduling and previous transfer");
SYSCTL_INT(_kern, OID_AUTO, jm21_touch_phase, CTLFLAG_RD | CTLFLAG_LOCKED, &phase, 0, "GT967: 1 GPIO, 2 supply, 3 reset, 4 ID, 5 config, 6 polling, negative failed");
SYSCTL_INT(_kern, OID_AUTO, jm21_touch_frames, CTLFLAG_RD | CTLFLAG_LOCKED, &report_count, 0, "GT967 physical frames");
SYSCTL_INT(_kern, OID_AUTO, jm21_touch_downs, CTLFLAG_RD | CTLFLAG_LOCKED, &down_count, 0, "GT967 physical finger downs");
static vm_offset_t gpio;
static bool bus_error;
static uint32_t saved_cfg[4], saved_io[4];
static const unsigned pins[] = {6, 7, 71, 80};
static struct { bool down; unsigned slot; uint32_t x, y; } fingers[16];
static const uint8_t panel_config[] = {
	0x45, 0xee, 0x02, 0x36, 0x05, 0x05, 0x0c, 0x00, 0x01, 0x0d, 0x2a, 0x0f, 0x50, 0x37, 0x05, 0xc5,
	0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x08, 0x18, 0x1a, 0x1e, 0x14, 0x86, 0x25, 0xaa, 0x3c, 0x3e,
	0x0c, 0x08, 0xb8, 0x08, 0x60, 0x1a, 0x22, 0x11, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x53, 0x0f,
	0x3c, 0x28, 0x00, 0x28, 0x50, 0x94, 0xd5, 0x82, 0x00, 0x00, 0x00, 0x04, 0xb1, 0x2a, 0x00, 0x9a,
	0x31, 0x00, 0x88, 0x38, 0x00, 0x77, 0x41, 0x00, 0x6a, 0x4a, 0x00, 0x6a, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x0e,
	0x17, 0x16, 0x13, 0x12, 0x11, 0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
	0x03, 0x02, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x08, 0x09,
	0x0a, 0x0b, 0x0c, 0x0d, 0x24, 0x25, 0x26, 0x27, 0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xf0, 0xf0, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x28, 0x01,
};
#define CFG(n) (gpio + 0x1000UL * (n))
#define IO(n) (CFG(n) + 4)
static uint32_t rd(vm_offset_t p) { return *(volatile uint32_t *)p; }
static void wr(vm_offset_t p, uint32_t v) {
	*(volatile uint32_t *)p = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static void us(unsigned n) {
	uint64_t end;
	nanoseconds_to_absolutetime((uint64_t)n * NSEC_PER_USEC, &end);
	end += mach_absolute_time();
	while (mach_absolute_time() < end) {}
}
/* Sleep, rather than busy-spin; opt-in low-latency timer avoids SYS_NORMAL
 * coalescing. I2C electrical and controller startup delays are unchanged. */
static void poll_sleep(void) {
 unsigned interval = poll_us;
 if (!precise_poll) { delay_for_interval(interval, NSEC_PER_USEC); return; }
 uint64_t deadline, ns;
 clock_interval_to_deadline(interval, NSEC_PER_USEC, &deadline);
 if (assert_wait_deadline_with_leeway((event_t)&poll_count, THREAD_UNINT,
     TIMEOUT_URGENCY_SYS_CRITICAL, deadline, 0) == THREAD_WAITING)
  thread_block(THREAD_CONTINUE_NULL);
 uint64_t now = mach_absolute_time();
 if (now > deadline) {
  absolutetime_to_nanoseconds(now - deadline, &ns);
  if (ns / 1000 > max_wake_late_us) max_wake_late_us = ns / 1000;
 }
}
/* High is open drain: release, never drive the I2C wires high. */
static void line(unsigned n, bool high) { wr(IO(n), 0); wr(CFG(n), high ? 3 : 3 | (1u << 9)); }
static bool level(unsigned n) { return rd(IO(n)) & 1; }
static void clock_high(void) {
	line(7, true);
	for (unsigned n=0; n<100 && !level(7); n++) us(5);
	if (!level(7)) bus_error=true;
	us(5);
}
static void start(void) {
	line(6,true); clock_high(); line(6,false); us(5); line(7,false); us(5);
}
static void stop(void) {
	line(6,false); us(5); clock_high(); line(6,true); us(5);
}
static bool bit(bool value) {
	line(6,value); us(5); clock_high(); bool v=level(6); line(7,false); us(5); return v;
}
static bool put(uint8_t v) {
	for (int n=7;n>=0;n--) bit((v>>n)&1);
	return !bit(true) && !bus_error;
}
static uint8_t get(bool more) {
	uint8_t v=0;
	for (unsigned n=0;n<8;n++) v=(uint8_t)((v<<1)|bit(true));
	bit(!more); return v;
}
static bool read_reg(unsigned reg, uint8_t *p, unsigned count) {
	bus_error=false; start();
	bool ok=put(0x5d<<1) && put(reg>>8) && put(reg);
	if(ok) { start(); ok=put((0x5d<<1)|1); }
	if(ok) for(unsigned n=0;n<count;n++) p[n]=get(n+1<count);
	stop(); return ok && !bus_error;
}
static bool write_reg(unsigned reg, const uint8_t *p, unsigned count) {
	bus_error=false; start();
	bool ok=put(0x5d<<1) && put(reg>>8) && put(reg);
	for(unsigned n=0;ok && n<count;n++) ok=put(p[n]);
	stop(); return ok && !bus_error;
}
static void output(unsigned n, bool high) { wr(IO(n),high?2:0); wr(CFG(n),1u<<9); }
static void restore(void) {
	for(unsigned n=0;n<4;n++) { wr(IO(pins[n]),saved_io[n]&2); wr(CFG(pins[n]),saved_cfg[n]); }
}
static void release_all(void) {
	for(unsigned n=0;n<16;n++) if(fingers[n].down) {
		hakios_touch_post(HAKIOS_TOUCH_UP,fingers[n].slot,fingers[n].x,fingers[n].y); fingers[n].down=false;
	}
}
/* DT eldo1_3v0: PM6125 SID0 GPIO7, peripheral 0xc6. v5 arbiter,
 * matching loader USB power-command implementation; require EE0 ownership. */
static vm_offset_t pm_read, pm_write;
static bool pm_changed;
static const unsigned pm_regs[] = {0x40,0x41,0x42,0x44,0x45,0x46};
static uint8_t pm_saved[6];
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
/* One arbiter transfer at a time with jm21-buttons' 10 ms PON polling (hakios_spmi_lock):
 * unserialized, the arbiter drops one and the supply step failed (touch_phase=-2). */
static bool pm_byte(unsigned reg, uint8_t *value, bool writing) {
	vm_offset_t b=writing?pm_write:pm_read;
	unsigned status=0;
	for(unsigned attempt=0;attempt<3;attempt++) {
		hakios_spmi_lock();
		if(writing) wr(b+0x10,*value);
		wr(b,((writing?0u:1u)<<27)|(reg<<4));
		for(unsigned n=0;n<200;n++) { status=rd(b+8); if(status&1) break; us(5); }
		uint32_t data=rd(b+0x18);
		hakios_spmi_unlock();
		if((status&15)==1) { if(!writing) *value=data&255; return true; }
		printf("jm21-touch: PMIC reg=%#x write=%u status=%#x (attempt %u)\n",reg,writing,status,attempt+1);
		us(1000);
	}
	return false;
}
static void power_restore(void) {
	if(!pm_changed) return;
	for(unsigned i=0;i<6;i++) pm_byte(pm_regs[i],&pm_saved[i],true);
	pm_changed=false;
}
static bool power_on(void) {
	vm_offset_t core=ml_io_map(0x01c40000,0x2000), owners=ml_io_map(0x01c0a000,0x1000);
	if(!core || !owners) return false;
	unsigned version=rd(core), apid=512;
	if(version<0x50000000 || version>=0x70000000) return false;
	for(unsigned i=0;i<512;i++) {
		unsigned map=rd(core+0x900+4*i);
		if(map && ((map>>8)&0xfff)==0xc6 && (rd(owners+0x700+4*i)&7)==0) { apid=i; break; }
	}
	if(apid==512) { printf("jm21-touch: GPIO7 peripheral not owned by EE0\n"); return false; }
	pm_read=ml_io_map(0x03e00000UL+0x80UL*apid,0x80);
	pm_write=ml_io_map(0x01e00000UL+0x10000UL*apid,0x1000);
	if(!pm_read || !pm_write) return false;
	uint8_t type=0,subtype=0;
	if(!pm_byte(4,&type,false) || !pm_byte(5,&subtype,false)) return false;
	printf("jm21-touch: PM6125 GPIO7 APID=%u type=%#x subtype=%#x\n",apid,type,subtype);
	if(type!=0x10 || (subtype!=0x10 && subtype!=0x11)) return false;
	for(unsigned i=0;i<6;i++) if(!pm_byte(pm_regs[i],&pm_saved[i],false)) return false;
	printf("jm21-touch: supply before mode=%#x vin=%#x pull=%#x source=%#x drive=%#x enable=%#x\n",
	    pm_saved[0],pm_saved[1],pm_saved[2],pm_saved[3],pm_saved[4],pm_saved[5]);
	/* LV/MV normal CMOS output-high, VIN0, bias disabled, drive strength1. */
	uint8_t values[]={1,0,5,0x80,1,0x80};
	pm_changed=true;
	for(unsigned i=0;i<6;i++) {
		uint8_t check=0;
		if(!pm_byte(pm_regs[i],&values[i],true) || !pm_byte(pm_regs[i],&check,false) || check!=values[i]) {
			power_restore(); return false;
		}
	}
	delay_for_interval(20,NSEC_PER_MSEC);
	return true;
}
static void touch_thread(__unused void *p, __unused wait_result_t result) {
	uint8_t id[6], cfg[228], frame[81], zero=0;
	unsigned frames=0, downs=0;
 bool failure_released=false;
 uint64_t failure_start=0;
	uint64_t last_poll=0;
	phase=1;
	gpio=ml_io_map(0x00500000UL,0x100000UL);
	if(!gpio) goto done;
	for(unsigned n=0;n<4;n++) {
		saved_cfg[n]=rd(CFG(pins[n])); saved_io[n]=rd(IO(pins[n]));
		printf("jm21-touch: gpio%u cfg=%#x io=%#x\n",pins[n],saved_cfg[n],saved_io[n]);
	}
	phase=2;
	output(71,false);
	if(!power_on()) goto failed;
	phase=3;
	line(6,true); line(7,true); us(100);
	/* Select address 0x5d while reset is asserted, then synchronize INT. */
	output(71,false); delay_for_interval(20,NSEC_PER_MSEC);
	output(80,false); us(200); output(71,true);
	delay_for_interval(10,NSEC_PER_MSEC); wr(CFG(71),0);
	output(80,false); delay_for_interval(50,NSEC_PER_MSEC); wr(CFG(80),0);
	if(!level(6) || !level(7) || !read_reg(0x8140,id,sizeof(id))) {
		printf("jm21-touch: probe failed SDA=%u SCL=%u INT=%u\n",level(6),level(7),level(80)); goto failed;
	}
	phase=4;
	printf("jm21-touch: product %.4s firmware=%02x%02x\n",id,id[5],id[4]);
	if(id[0]!='9' || id[1]!='6' || id[2]!='7') goto failed;
	phase=5;
	if(!read_reg(0x8047,cfg,sizeof(cfg))) goto failed;
	printf("jm21-touch: config=%u size=%ux%u trigger=%u\n",cfg[0],cfg[1]|cfg[2]<<8,cfg[3]|cfg[4]<<8,cfg[6]&3);
	if(memcmp(cfg,panel_config,sizeof(cfg)-2)!=0) {
		if(!write_reg(0x8047,panel_config,sizeof(panel_config))) goto failed;
		delay_for_interval(20,NSEC_PER_MSEC);
		if(!read_reg(0x8047,cfg,sizeof(cfg)) || memcmp(cfg,panel_config,sizeof(cfg)-2)!=0) goto failed;
		printf("jm21-touch: JM21 panel RAM config installed\n");
	}
	if(!write_reg(0x814e,&zero,1)) goto failed;
	phase=6;
	printf("jm21-touch: polling GT967 at 0x5d, 750x1334; sleep %u us; posting to BackBoard\n",poll_us);
	for(;;) {
		poll_sleep();
		uint64_t now=mach_absolute_time(), gap_ns;
		if(last_poll) {
			absolutetime_to_nanoseconds(now-last_poll,&gap_ns);
			last_poll_gap_us=gap_ns/1000;
			if(gap_ns/1000>max_poll_gap_us) max_poll_gap_us=gap_ns/1000;
		}
		last_poll=now; poll_count++;
		if(!read_reg(0x814e,frame,1)) {
			io_errors++;
			/* Real elapsed time: sub-ms polling must not turn recovery into
			 * a count of attempts or cause an early stuck-contact release. */
			uint64_t failed_now=mach_absolute_time(), failed_ns;
			if(!failure_start) failure_start=failed_now;
			absolutetime_to_nanoseconds(failed_now-failure_start,&failed_ns);
			if(!failure_released && failed_ns>=24*NSEC_PER_MSEC) {
				release_all(); failure_released=true;
			}
			if(failed_ns>=NSEC_PER_SEC) { printf("jm21-touch: bus failed, stopped\n"); goto failed; }
			continue;
		}
		failure_start=0; failure_released=false;
		if(!(frame[0]&0x80)) continue;
		unsigned count=frame[0]&15, seen=0;
		if(count>10) { if(!write_reg(0x814e,&zero,1)) io_errors++; release_all(); continue; }
		if(count && !read_reg(0x814f,frame+1,count*8)) { io_errors++; release_all(); continue; }
		/* Validate the entire report before emitting any events. */
		bool valid=true;
		for(unsigned n=0;n<count;n++) {
			uint8_t *c=frame+1+8*n; unsigned i=c[0]&15, x=c[1]|c[2]<<8, y=c[3]|c[4]<<8;
			if((seen&(1u<<i)) || x>=750 || y>=1334) valid=false;
			seen|=1u<<i;
		}
		if(!write_reg(0x814e,&zero,1)) { io_errors++; release_all(); continue; }
		if(!valid) { release_all(); continue; }
		frames++; report_count=(int)frames;
		/* Hardware IDs are 4 bits; BackBoard supports ten compact slots.
		 * Release absent contacts before allocating slots for new IDs. */
		for(unsigned i=0;i<16;i++) if(fingers[i].down && !(seen&(1u<<i))) {
			hakios_touch_post(HAKIOS_TOUCH_UP,fingers[i].slot,fingers[i].x,fingers[i].y); fingers[i].down=false;
		}
		for(unsigned n=0;n<count;n++) {
			uint8_t *c=frame+1+8*n; unsigned i=c[0]&15;
			uint32_t x=(c[1]|c[2]<<8)*65535u/749, y=(c[3]|c[4]<<8)*65535u/1333;
			if(!fingers[i].down) {
				unsigned used=0, slot=0;
				for(unsigned j=0;j<16;j++) if(fingers[j].down) used|=1u<<fingers[j].slot;
				while(slot<10 && (used&(1u<<slot))) slot++;
				if(slot==10) { release_all(); break; }
				fingers[i].slot=slot;
				downs++; down_count=(int)downs;
				if(downs<=32 || !(downs%100)) printf("jm21-touch: down id=%u x=%u y=%u frames=%u downs=%u\n",i,x,y,frames,downs);
				hakios_touch_post(HAKIOS_TOUCH_DOWN,fingers[i].slot,x,y);
			} else if(x!=fingers[i].x || y!=fingers[i].y) hakios_touch_post(HAKIOS_TOUCH_MOVE,fingers[i].slot,x,y);
			fingers[i].down=true; fingers[i].x=x; fingers[i].y=y;
		}
	}
failed:
	phase=-phase;
	release_all(); restore(); power_restore(); printf("jm21-touch: disabled; original GPIO configuration restored\n");
done:
	thread_terminate(current_thread());
}
void hakios_jm21_touch_init(void) {
	unsigned on=0, jm21=0; thread_t thread;
	if(!PE_parse_boot_argn("jm21_ios",&jm21,sizeof(jm21)) || !jm21 ||
	   !PE_parse_boot_argn("jm21_touch",&on,sizeof(on)) || !on) return;
	/* Keep stock polling unless explicitly selected; 8 restores the prior path. */
	unsigned requested=0;
	if(PE_parse_boot_argn("jm21_touch_poll_ms",&requested,sizeof(requested)) && requested>=1 && requested<=20)
		poll_ms=requested;
	poll_us=poll_ms*1000;
	if(PE_parse_boot_argn("jm21_touch_poll_us",&requested,sizeof(requested)) && requested>=100 && requested<=20000) {
		poll_us=requested; poll_ms=(requested+999)/1000; precise_poll=true;
	}
	if(kernel_thread_start(touch_thread,NULL,&thread)==KERN_SUCCESS) thread_deallocate(thread);
}
