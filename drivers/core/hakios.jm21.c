/*
 * kern.jm21_bridge: 256 bytes shared with the FiiO JM21 loader's USB monitor, which runs on a Silver
 * core with its MMU and caches off (fiio-xnu-poc-display). The boot-arg jm21_bridge=PA says where: in
 * the padding after the root image, inside the ramdisk segment the kernel maps statically. Each access
 * cleans or invalidates the four cache lines to the point of coherency, since the other side bypasses
 * the cache. Frames, sequence numbers and checksums are the monitor's business (shell-wire.h).
 */

#include <console/video_console.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/msgbuf.h>
#include <sys/kauth.h>
#include <sys/hakios.mmc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern struct vc_info vinfo;
extern vm_offset_t ml_static_ptovirt(vm_offset_t paddr);
extern void CleanPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define BRIDGE_SIZE 256

static int
sysctl_jm21_bridge SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint64_t pa = 0;
	char buf[BRIDGE_SIZE];
	int changed = 0, error;

	if (!PE_parse_boot_argn("jm21_bridge", &pa, sizeof(pa)) || pa == 0 || (pa & 63) != 0) {
		return ENOENT;
	}
	vm_offset_t box = ml_static_ptovirt((vm_offset_t)pa);
	if (box == 0) {
		return ENOENT;
	}
	FlushPoC_DcacheRegion(box, BRIDGE_SIZE);
	memcpy(buf, (const void *)box, BRIDGE_SIZE);
	buf[BRIDGE_SIZE - 1] = 0;
	error = sysctl_io_string(req, buf, BRIDGE_SIZE, 0, &changed);
	if (error == 0 && changed) {
		memcpy((void *)box, buf, BRIDGE_SIZE);
		CleanPoC_DcacheRegion(box, BRIDGE_SIZE);
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, jm21_bridge, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY,
    0, 0, sysctl_jm21_bridge, "A", "the JM21 loader's USB shell mailbox");

/* kern.jm21_log: a line written here goes to the kernel log, so that the loader's USB log shows
 * process 1's own progress (its console output reaches only the screen). */
static int
sysctl_jm21_log SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[200];
	size_t len = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (len >= sizeof(line)) {
		len = sizeof(line) - 1;
	}
	int error = SYSCTL_IN(req, line, len);
	if (error == 0) {
		line[len] = 0;
		printf("jm21: %s\n", line);
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, jm21_log, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY,
    0, 0, sysctl_jm21_log, "A", "a line for the kernel log");

/*
 * The monitor reads the kernel log (msgbufp) from RAM with its caches off, so lines the Gold
 * cores have written but not yet evicted are not there: the log seemed to stop at whatever was
 * last written back. Clean the buffer to the point of coherency four times a second.
 */
extern struct msgbuf *msgbufp;
static thread_call_t jm21_log_call;

static void
jm21_log_flush(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	struct msgbuf *mb = msgbufp;
	uint64_t deadline;

	if (mb != NULL && mb->msg_bufc != NULL && mb->msg_size > 0) {
		CleanPoC_DcacheRegion((vm_offset_t)mb, sizeof(*mb));
		CleanPoC_DcacheRegion((vm_offset_t)mb->msg_bufc, (size_t)mb->msg_size);
	}
	clock_interval_to_deadline(250, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(jm21_log_call, deadline);
}

void
hakios_jm21_init(void)
{
	uint64_t pa = 0;

	if (!PE_parse_boot_argn("jm21_bridge", &pa, sizeof(pa)) || pa == 0) {
		return;
	}
	jm21_log_call = thread_call_allocate(jm21_log_flush, NULL);
	jm21_log_flush(NULL, NULL);
}

/*
 * Minimal software presentation for a root Darwin process, without exposing physical memory.
 * The bootloader owns scanout; use the existing write-combining video-console mapping.
 * Wire ABI is little endian u32. info: version,width,height,rowbytes,format,max_pixels,owned,writes.
 * Write: version,op,x,y then zero pixels for claim(1)/release(2), or 1..750 BGRX pixels for span(3).
 * No allocation, hardware reprogramming or access beyond this board's confirmed framebuffer.
 */
#define JM21_FB_WIDTH 750u
#define JM21_FB_HEIGHT 1334u
#define JM21_FB_ROWBYTES 3000u
#define JM21_FB_VERSION 1u
static uint32_t jm21_fb_owned;
static uint32_t jm21_fb_writes;

static int
jm21_fb_ready(void)
{
	uint64_t pa = 0;
	return PE_parse_boot_argn("jm21_bridge", &pa, sizeof(pa)) && pa != 0 &&
	       vinfo.v_baseaddr != 0 && vinfo.v_physaddr == 0x5c000000ULL &&
	       vinfo.v_width == JM21_FB_WIDTH && vinfo.v_height == JM21_FB_HEIGHT &&
	       vinfo.v_rowbytes == JM21_FB_ROWBYTES && vinfo.v_depth == 32;
}

static int
sysctl_jm21_fb_info SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint32_t info[8];
	if (suser(kauth_cred_get(), NULL) != 0) {
		return EPERM;
	}
	if (!jm21_fb_ready()) {
		return ENXIO;
	}
	info[0] = JM21_FB_VERSION;
	info[1] = JM21_FB_WIDTH;
	info[2] = JM21_FB_HEIGHT;
	info[3] = JM21_FB_ROWBYTES;
	info[4] = 1; /* BGRX bytes; u32 0x00RRGGBB */
	info[5] = JM21_FB_WIDTH;
	info[6] = jm21_fb_owned;
	info[7] = jm21_fb_writes;
	return SYSCTL_OUT(req, info, sizeof(info));
}

static int
sysctl_jm21_fb SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint32_t packet[4 + JM21_FB_WIDTH];
	size_t bytes = req->newlen;
	int error;
	if (suser(kauth_cred_get(), NULL) != 0) {
		return EPERM;
	}
	if (!jm21_fb_ready()) {
		return ENXIO;
	}
	if (req->newptr == USER_ADDR_NULL || bytes < 16 || bytes > sizeof(packet) || (bytes & 3)) {
		return EINVAL;
	}
	error = SYSCTL_IN(req, packet, bytes);
	if (error != 0) {
		return error;
	}
	if (packet[0] != JM21_FB_VERSION) {
		return EINVAL;
	}
	if (packet[1] == 1 || packet[1] == 2) {
		if (bytes != 16 || packet[2] != 0 || packet[3] != 0) {
			return EINVAL;
		}
		/* Screen pause takes VCPUTC_LOCK and also silences /dev/console writes;
		 * msgbuf and the separate USB monitor remain available. Avoid switching to serial. */
		initialize_screen(NULL, packet[1] == 1 ? kPEDisableScreen : kPEEnableScreen);
		jm21_fb_owned = packet[1] == 1;
		return 0;
	}
	if (packet[1] != 3 || bytes == 16) {
		return EINVAL;
	}
	if (!jm21_fb_owned) {
		return EBUSY;
	}
	size_t count = (bytes - 16) / sizeof(uint32_t);
	if (packet[2] >= JM21_FB_WIDTH || packet[3] >= JM21_FB_HEIGHT ||
	    count > JM21_FB_WIDTH - packet[2]) {
		return EINVAL;
	}
	volatile uint32_t *dst = (volatile uint32_t *)(vinfo.v_baseaddr +
	    packet[3] * JM21_FB_ROWBYTES + packet[2] * sizeof(uint32_t));
	for (size_t i = 0; i < count; i++) {
		dst[i] = packet[4 + i];
	}
	/* vinfo is VM_WIMG_WCOMB (non-cacheable), so D-cache clean is unnecessary.
	 * Complete the stores before reporting successful presentation to userspace. */
	__asm__ volatile("dsb sy" ::: "memory");
	jm21_fb_writes++;
	return 0;
}

/* Omit CTLFLAG_LOCKED: the sysctl compatibility mutex serializes claim/release/spans. */
SYSCTL_PROC(_kern, OID_AUTO, jm21_fb_info, CTLTYPE_OPAQUE | CTLFLAG_RD,
    0, 0, sysctl_jm21_fb_info, "S,jm21_fb_info", "JM21 software framebuffer geometry and status");
SYSCTL_PROC(_kern, OID_AUTO, jm21_fb, CTLTYPE_OPAQUE | CTLFLAG_WR,
    0, 0, sysctl_jm21_fb, "S,jm21_fb_packet", "JM21 root-only software framebuffer presentation");
