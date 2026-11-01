/*
 * FiiO JM21 (SM6225 "khaje"): the Adreno 610 for Mesa freedreno, behind the hakios_gpu_* interface that
 * hakios.gpu.cpp (the HakiosGPU service) speaks. Boot-arg jm21_a610=1 (with jm21_ios=1). The core stays at
 * the speed-bin maximum through RPM rail votes and PLL0, with no idle
 * frequency reduction; 600 MHz GPLL0 fallback if initialization fails.
 *
 * The hardware sequence follows Linux (drivers/gpu/drm/msm adreno a6xx for the A610 without GMU,
 * clk/qcom gpucc-sm6115, a6xx_catalog a612_hwcg) as worked out for this SoC by MakrSas/ios-topaz
 * (TopazAdreno.cpp) and topaz-woa: GCC branches, CX then GX GDSC, gfx3d from GPLL0 without a rail vote,
 * the zap shader through TrustZone (PAS 13), the GPU SMMU with one stage-1 context, the SQE microcode,
 * CP_ME_INIT and CP_SET_SECURE_MODE 0, indirect buffers and CACHE_FLUSH_TS fences.
 *
 * JM21 specifics: the zap region is 0x55615000 (0x2000, vendor DT); the firmware (a630_sqe.fw,
 * a610_zap.mdt, a610_zap.b02, from the phone's /vendor/firmware) comes from fw/ of the RAM disk
 * (hakios_image_file). Memory the GPU reads is physically contiguous kernel memory, used by the processor
 * through a device (uncached) alias; all of the JM21's DRAM is below 4 GiB.
 *
 * Any failure leaves the GPU unpublished: CoreAnimation then keeps its software renderer.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void IOSleep(unsigned int milliseconds);
extern void delay(int usec);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern void HakiosGPUStart(void);

bool a610_active;               /* hakios.adreno.c sends hakios_gpu_* here when true */

#define GCC_BASE        0x01400000ull
#define GCC_SIZE        0x80000
#define GPUCC_BASE      0x05990000ull
#define GPUCC_SIZE      0x9000
#define GPU_BASE        0x05900000ull
#define GPU_SIZE        0x40000
#define SMMU_BASE       0x059A0000ull
#define SMMU_SIZE       0x10000
#define SMMU_CB0        0x8000
#define ZAP_REGION      0x55615000ull           /* JM21 vendor DT: zap-shader memory-region, no-map */
#define ZAP_REGION_SIZE 0x2000
#define GPU_PAS_ID      13

#define CBCR_EN         (1u << 0)
#define CBCR_OFF        (1u << 31)
#define GDSC_COLLAPSE   (1u << 0)
#define GDSC_PWR_ON     (1u << 31)
#define RCG_UPDATE      (1u << 0)
#define SRC_GPLL0       5

#define SCM_FN(svc, cmd)    (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_PIL         0x02
#define PIL_INIT_IMAGE      0x01
#define PIL_MEM_SETUP       0x02
#define PIL_AUTH_RESET      0x05
#define PIL_SHUTDOWN        0x06
#define SCM_ARG_RW          2

#define RB_BYTES            0x8000
#define RB_DWORDS           (RB_BYTES / 4)
#define CP_INDIRECT_BUFFER  0x3f
#define CP_EVENT_WRITE      0x46
#define CP_ME_INIT          0x48
#define CP_SET_SECURE_MODE  0x66
#define CACHE_FLUSH_TS      4

#define VA_USER_START       0x01000000ull
#define VA_KERNEL           0xF0000000ull
#define GMEM_SIZE           0x21000
#define GMEM_BASE           0x100000

static volatile uint8_t *gcc, *gpucc, *gpu, *smmu;
static bool ready;
static uint32_t wptr, seqno, core_mhz = 600;
static int phase;
static uint32_t cx_level, oc_mhz;
static LCK_GRP_DECLARE(a610_grp, "jm21-a610");
static LCK_MTX_DECLARE(a610_lock, &a610_grp);

SYSCTL_INT(_kern, OID_AUTO, jm21_a610_phase, CTLFLAG_RD | CTLFLAG_LOCKED, &phase, 0,
    "JM21 A610: 1 power, 2 zap, 3 smmu, 4 cp, 5 ready; negative: failed there");

SYSCTL_INT(_kern, OID_AUTO, jm21_a610_mhz, CTLFLAG_RD | CTLFLAG_LOCKED, &core_mhz, 0,
    "JM21 A610 fixed core clock in MHz");

SYSCTL_INT(_kern, OID_AUTO, jm21_a610_cx_level, CTLFLAG_RD | CTLFLAG_LOCKED, &cx_level, 0,
    "Acknowledged GPU CX RPM corner (not millivolts)");
SYSCTL_INT(_kern, OID_AUTO, jm21_a610_oc_mhz, CTLFLAG_RD | CTLFLAG_LOCKED, &oc_mhz, 0,
    "Applied opt-in GPU overclock; zero means stock");

struct kbuf {
	vm_offset_t cached;             /* the allocation */
	volatile uint8_t *va;           /* device alias the processor uses */
	uint64_t pa, iova, size;
};
static struct kbuf ring, sqe, mem;
static uint64_t kernel_va_next = VA_KERNEL;

/* ---------------------------------------------------------------- MMIO */

static inline uint32_t rd(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr(volatile uint8_t *b, uint32_t o, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(b + o) = v;
}
static inline void rmw(volatile uint8_t *b, uint32_t o, uint32_t clr, uint32_t set) { wr(b, o, (rd(b, o) & ~clr) | set); }
static inline void gpu_wr(uint32_t reg, uint32_t v) { wr(gpu, 4 * reg, v); }
static inline uint32_t gpu_rd(uint32_t reg) { return rd(gpu, 4 * reg); }
static inline void gpu_wr64(uint32_t reg, uint64_t v) { gpu_wr(reg, (uint32_t)v); gpu_wr(reg + 1, (uint32_t)(v >> 32)); }

static bool
poll_bits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i <= us; i++) {
		if ((rd(b, o) & mask) == want) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* ---------------------------------------------------------------- memory */

static bool
kbuf_alloc(struct kbuf *b, uint64_t size)
{
	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &b->cached, size, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	bzero((void *)b->cached, size);
	FlushPoC_DcacheRegion(b->cached, size);         /* the zeros reach memory; no line is left to write back */
	b->pa = kvtophys(b->cached);
	b->va = (volatile uint8_t *)ml_io_map(b->pa, size);
	b->size = size;
	return b->va != NULL && (b->pa >> 32) == 0;
}

static void
kbuf_copy(struct kbuf *b, const uint8_t *from, uint64_t n)
{
	uint64_t i;

	for (i = 0; i + 4 <= n; i += 4) {               /* device memory: aligned words only */
		uint32_t w;
		memcpy(&w, from + i, 4);
		*(volatile uint32_t *)(b->va + i) = w;
	}
	if (i < n) {
		uint32_t w = 0;
		memcpy(&w, from + i, n - i);
		*(volatile uint32_t *)(b->va + i) = w;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static const uint8_t *
firmware(const char *name, uint64_t *size)
{
	const uint8_t *data = hakios_image_file("fw", name, size, NULL);

	if (data == NULL) {
		printf("jm21-a610: fw/%s is not in the RAM disk\n", name);
	}
	return data;
}

/* ---------------------------------------------------------------- power */

static bool
branch_on(volatile uint8_t *b, uint32_t o, const char *name)
{
	rmw(b, o, 0, CBCR_EN);
	bool ok = poll_bits(b, o, CBCR_OFF, 0, 200);
	if (!ok) {
		printf("jm21-a610: %s stays off (%08x)\n", name, rd(b, o));
	}
	return ok;
}

static bool
rcg_set(uint32_t cmd, uint32_t src, uint32_t div)
{
	wr(gpucc, cmd + 4, (src << 8) | (2 * div - 1));
	rmw(gpucc, cmd, 0, RCG_UPDATE);
	return poll_bits(gpucc, cmd, RCG_UPDATE, 0, 500);
}

static bool
gdsc_on(uint32_t gdscr, uint32_t bcr, uint32_t clamp, uint32_t status)
{
	if (bcr != 0) {
		rmw(gpucc, bcr, 0, 1);
		delay(1);
		rmw(gpucc, bcr, 1, 0);
	}
	if (clamp != 0) {
		rmw(gpucc, clamp, 1, 0);
	}
	rmw(gpucc, gdscr, GDSC_COLLAPSE, 0);
	delay(1);
	return poll_bits(gpucc, status, GDSC_PWR_ON, GDSC_PWR_ON, 500);
}

/* Khaje RPM transport: qcom_glink_rpm/native + rpm-smd V0, polled at boot.
 * Only this driver owns the APPS/RPM FIFO in this port. All waits are bounded.
 * Leave the working GPLL0 path selected unless both rail ACKs and PLL lock pass. */
struct rpm_pipe { volatile uint8_t *p; uint32_t size; };
static struct rpm_pipe rpm_tx, rpm_rx;
static volatile uint8_t *rpm_ipc;
static bool rpm_open_ack, rpm_remote_open;
static uint32_t rpm_ack_id, rpm_msg_id;
static bool rpm_ack_error;
static uint32_t rpm_remote_cid;

static void rpm_kick(void) { wr(rpm_ipc, 8, 1); }
static uint32_t rpm_avail(struct rpm_pipe *p)
{
	uint32_t tail = rd(p->p, 0), head = rd(p->p, 4);
	if (tail >= p->size || head >= p->size) return 0;
	return (head + p->size - tail) % p->size;
}
static void rpm_peek(uint32_t *out, uint32_t bytes)
{
	uint32_t tail = rd(rpm_rx.p, 0);
	for (uint32_t i = 0; i < bytes; i += 4)
		out[i / 4] = rd(rpm_rx.p, 8 + (tail + i) % rpm_rx.size);
}
static bool rpm_send(const uint32_t *buf, uint32_t bytes)
{
	uint32_t aligned = (bytes + 7) & ~7u;
	if (rpm_avail(&rpm_tx) + aligned + 8 >= rpm_tx.size) return false;
	uint32_t head = rd(rpm_tx.p, 4);
	for (uint32_t i = 0; i < aligned; i += 4)
		wr(rpm_tx.p, 8 + (head + i) % rpm_tx.size, i < bytes ? buf[i / 4] : 0);
	wr(rpm_tx.p, 4, (head + aligned) % rpm_tx.size);
	rpm_kick();
	return true;
}
static bool rpm_control(uint32_t cmd, uint32_t p1, uint32_t p2)
{
	uint32_t buf[2] = { cmd | (p1 << 16), p2 };
	return rpm_send(buf, sizeof(buf));
}
static bool rpm_poll(void)
{
	uint32_t buf[128], n = 8, avail = rpm_avail(&rpm_rx);
	if (avail < 8) return true;
	rpm_peek(buf, 8);
	uint32_t cmd = buf[0] & 0xffff, cid = buf[0] >> 16;
	if (cmd == 2) n += buf[1] & 0xffff;
	else if (cmd == 9 || cmd == 12) {
		if (avail < 16) return true;
		rpm_peek(buf, 16);
		n = 16 + buf[2];
	} else if (cmd == 5) n += buf[1] * 8;
	n = (n + 7) & ~7u;
	if (n > sizeof(buf) || n > rpm_rx.size - 8) {
		printf("jm21-a610: RPM invalid GLINK cmd %u len %u\n", cmd, n);
		return false;
	}
	if (avail < n) return true;
	rpm_peek(buf, n);
	wr(rpm_rx.p, 0, (rd(rpm_rx.p, 0) + n) % rpm_rx.size);
	switch (cmd) {
	case 0: rpm_control(1, 1, 0); break;
	case 1: break;
	case 2:
		printf("jm21-a610: RPM channel cid %u len %u name %.32s\n", cid, buf[1] & 0xffff, (char *)&buf[2]);
		if ((buf[1] & 0xffff) >= sizeof("rpm_requests") &&
		    memcmp(&buf[2], "rpm_requests", sizeof("rpm_requests")) == 0) {
			rpm_remote_cid = cid;
			rpm_remote_open = rpm_control(4, cid, 0);
		} else {
			rpm_control(4, cid, 0); /* RPM also announces auxiliary channels. */
		}
		break;
	case 4: if (cid == 1) rpm_open_ack = true; break;
	case 9:
		/* GLINK intentless RPM replies: req header followed by msg# and err. */
		if (cid == rpm_remote_cid && buf[3] == 0 && buf[2] >= 20 && buf[4] == 0x716572) {
			rpm_ack_id = 0;
			rpm_ack_error = false;
			uint32_t end = 8 + buf[2]; /* payload length bounds checked by n above */
			for (uint32_t off = 24; off + 8 <= end + 8;) {
				uint32_t k = buf[off / 4], len = buf[off / 4 + 1];
				if (len > n || off + 8 + len > 16 + buf[2]) return false;
				if (k == 0x2367736d && len == 4) rpm_ack_id = buf[off / 4 + 2];
				if (k == 0x727265) rpm_ack_error = true;
				off += 8 + ((len + 3) & ~3u);
			}
			printf("jm21-a610: RPM ACK id %u error %u\n", rpm_ack_id, rpm_ack_error);
		}
		break;
	case 13: rpm_kick(); break;
	case 15: break;
	default:
		printf("jm21-a610: RPM GLINK cmd %u cid %u\n", cmd, cid);
		return false;
	}
	return true;
}
static bool rpm_connect(void)
{
	volatile uint8_t *ram = (volatile uint8_t *)ml_io_map(0x045f0000, 0x7000);
	rpm_ipc = (volatile uint8_t *)ml_io_map(0x0f111000, 0x1000);
	if (!ram || !rpm_ipc || rd(ram, 0x6f00) != 0x67727430) return false;
	uint32_t count = rd(ram, 0x6f04);
	if (count > 20) return false;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t off = 0x6f08 + i * 12;
		uint32_t id = rd(ram, off), start = rd(ram, off + 4), size = rd(ram, off + 8);
		if (size < 64 || start > 0x6f00 || size + 8 > 0x6f00 - start) continue;
		struct rpm_pipe *p = id == 0x61703272 ? &rpm_tx : id == 0x72326170 ? &rpm_rx : NULL;
		if (p) { p->p = ram + start; p->size = size; }
	}
	if (!rpm_tx.p || !rpm_rx.p) return false;
	printf("jm21-a610: RPM GLINK tx %u rx %u\n", rpm_tx.size, rpm_rx.size);
	wr(rpm_tx.p, 4, 0);
	wr(rpm_rx.p, 0, 0);
	if (!rpm_control(0, 1, 0)) return false;
	uint32_t open[6] = { 2 | (1u << 16), sizeof("rpm_requests"), 0, 0, 0, 0 };
	memcpy(&open[2], "rpm_requests", sizeof("rpm_requests"));
	if (!rpm_send(open, 8 + ((sizeof("rpm_requests") + 3) & ~3u))) return false;
	for (unsigned i = 0; i < 2000; i++) {
		if (!rpm_poll()) return false;
		if (rpm_open_ack && rpm_remote_open) return true;
		IOSleep(1);
	}
	printf("jm21-a610: RPM open timeout ack=%u remote=%u\n", rpm_open_ack, rpm_remote_open);
	return false;
}
static bool rpm_rail(uint32_t type, uint32_t level)
{
	uint32_t id = ++rpm_msg_id;
	uint32_t buf[] = { 9 | (1u << 16), 0, 40, 0,
	    0x716572, 32, id, 0, type, 0, 12, 0x6c766c76, 4, level };
	rpm_ack_id = 0;
	if (!rpm_send(buf, sizeof(buf))) return false;
	for (unsigned i = 0; i < 2000; i++) {
		if (!rpm_poll()) return false;
		if (rpm_ack_id == id) return !rpm_ack_error;
		IOSleep(1);
	}
	printf("jm21-a610: RPM rail %08x level %u timeout\n", type, level);
	return false;
}
/* Shared with other JM21 drivers (hakios.jm21_wifi.c: the WCN3990's LDO votes): one rpm-smd request,
 * key/length/value words, on this driver's APPS/RPM FIFO, serialized by a610_lock. */
static bool rpm_connected;
bool hakios_jm21_rpm_request(uint32_t type, uint32_t id, const uint32_t *kvp, uint32_t words);
bool
hakios_jm21_rpm_request(uint32_t type, uint32_t id, const uint32_t *kvp, uint32_t words)
{
	uint32_t buf[64], data = 4 * words, msg;
	bool ok = false;

	if (words > sizeof(buf) / 4 - 11) return false;
	lck_mtx_lock(&a610_lock);
	if (!rpm_connected) rpm_connected = rpm_connect();
	if (rpm_connected) {
		msg = ++rpm_msg_id;
		buf[0] = 9 | (1u << 16); buf[1] = 0; buf[2] = 8 + 20 + data; buf[3] = 0;
		buf[4] = 0x716572; buf[5] = 20 + data; buf[6] = msg; buf[7] = 0; buf[8] = type; buf[9] = id; buf[10] = data;
		memcpy(&buf[11], kvp, data);
		rpm_ack_id = 0;
		if (rpm_send(buf, 4 * (11 + words))) {
			for (unsigned i = 0; i < 2000 && !ok; i++) {
				if (!rpm_poll()) break;
				if (rpm_ack_id == msg) { ok = !rpm_ack_error; break; }
				IOSleep(1);
			}
		}
	}
	lck_mtx_unlock(&a610_lock);
	return ok;
}

static void performance_max(void)
{
	volatile uint8_t *fuse = (volatile uint8_t *)ml_io_map(0x01b46000, 0x1000);
	if (!fuse) return;
	uint32_t bin = (rd(fuse, 4) >> 21) & 0xff;
	uint32_t mhz, level;
	switch (bin) {
	case 0: mhz = 1260; level = 416; break;
	case 235: mhz = 1114; level = 416; break;
	case 216: mhz = 1025; level = 384; break;
	case 157: mhz = 745; level = 256; break;
	case 127: mhz = 600; level = 192; break;
	default: printf("jm21-a610: unknown speed bin %u, keeping 600 MHz\n", bin); return;
	}
	/* Khaje gpucc: 1260 MHz requires SUPER_TURBO (464), not 416.
	 * Only the measured JM21 bin may opt in; stock remains the default.
	 * CPU LMH, storage clocks and charging thermal protection are unchanged. */
	uint32_t requested = 0;
	PE_parse_boot_argn("goc", &requested, sizeof(requested));
	if (requested == 1) requested = 1260;
	if (requested == 1260 && bin == 235) {
		mhz = 1260;
		level = 464;
	} else if (requested) {
		printf("jm21-a610: unsupported OC %u/bin %u; using stock\n", requested, bin);
	}
	printf("jm21-a610: speed bin %u maximum %u MHz, CX level %u; PLL mode %08x L %08x alpha %08x user %08x\n",
	    bin, mhz, level, rd(gpucc, 0), rd(gpucc, 4), rd(gpucc, 8), rd(gpucc, 12));
	if (mhz <= 600) return;
	if (!(rpm_connected || (rpm_connected = rpm_connect())) || !rpm_rail(0x786d7772, 256) || !rpm_rail(0x78637772, level)) {
		printf("jm21-a610: max performance rail not acknowledged; keeping 600 MHz\n"); return;
	}
	cx_level = level;
	/* PLL0 output /2. 19.2 MHz reference, Zonda alpha16. */
	uint64_t hz = bin == 235 && mhz == 1114 ? 1114800000ull : (uint64_t)mhz * 1000000;
	uint64_t scaled = (hz * 2 * 65536 + 19200000 / 2) / 19200000;
	wr(gpucc, 0, 0);
	wr(gpucc, 0x28, 0);
	wr(gpucc, 4, (uint32_t)(scaled >> 16));
	wr(gpucc, 8, (uint32_t)scaled & 0xffff);
	wr(gpucc, 0x10, 0x08200800);
	wr(gpucc, 0x14, 0x05022001);
	wr(gpucc, 0x18, 0x10);
	wr(gpucc, 0x0c, 0x01000101);
	wr(gpucc, 0, 2); delay(1);
	wr(gpucc, 0, 6);
	wr(gpucc, 0x28, 1);
	if (!poll_bits(gpucc, 0, 1u << 31, 1u << 31, 500)) {
		printf("jm21-a610: PLL0 lock failed, keeping 600 MHz\n"); return;
	}
	rmw(gpucc, 0x0c, 0xf, 0xf);
	rmw(gpucc, 0, 0, 1);
	if (!rcg_set(0x101c, 1, 1)) {
		rcg_set(0x101c, SRC_GPLL0, 1);
		printf("jm21-a610: PLL0 mux failed, restored 600 MHz\n"); return;
	}
	core_mhz = mhz;
	if (requested == 1260 && bin == 235) oc_mhz = mhz;
	printf("jm21-a610: fixed maximum %llu Hz PLL0 mode %08x L %08x alpha %08x CFG %08x\n",
	    hz, rd(gpucc, 0), rd(gpucc, 4), rd(gpucc, 8), rd(gpucc, 0x1020));
}

static bool
power_up(void)
{
	printf("jm21-a610: before: GX %08x CX %08x\n", rd(gpucc, 0x100c), rd(gpucc, 0x1540));
	rmw(gcc, 0x79004, 0, 1u << 15);                 /* GPLL0 to the GPU CC */
	branch_on(gcc, 0x71154, "gcc_bimc_gpu_axi");
	branch_on(gcc, 0x3600c, "gcc_gpu_memnoc_gfx");
	branch_on(gcc, 0x36018, "gcc_gpu_snoc_dvm_gfx");
	if (!gdsc_on(0x106c, 0, 0, 0x1540)) {
		printf("jm21-a610: CX GDSC: no PWR_ON (%08x)\n", rd(gpucc, 0x1540));
		return false;
	}
	if (!branch_on(gpucc, 0x109c, "gpucc_cxo") || !branch_on(gpucc, 0x1078, "gpucc_ahb")) {
		return false;
	}
	rcg_set(0x1120, SRC_GPLL0, 3);                  /* gmu clock, 200 MHz */
	rmw(gpucc, 0x1098, 0xFF0, 0xFF0);
	branch_on(gpucc, 0x1098, "gpucc_cx_gmu");
	branch_on(gpucc, 0x5000, "gpucc_hlos1_vote_gpu_smmu");
	if (!gdsc_on(0x100c, 0x1008, 0x1508, 0x100c)) {
		printf("jm21-a610: GX GDSC: no PWR_ON (%08x)\n", rd(gpucc, 0x100c));
		return false;
	}
	if (!rcg_set(0x101c, SRC_GPLL0, core_mhz >= 600 ? 1 : 2)) {   /* gfx3d: GPLL0 600 MHz /2 or /1 */
		printf("jm21-a610: gfx3d RCG does not update\n");
		return false;
	}
	rmw(gpucc, 0x1054, 0, (1u << 14) | (1u << 13));
	if (!branch_on(gpucc, 0x1054, "gpucc_gx_gfx3d")) {
		return false;
	}
	branch_on(gpucc, 0x1060, "gpucc_gx_cxo");
	performance_max();
	return true;
}

/* ---------------------------------------------------------------- TrustZone: the zap shader */

static void
smc8(uint64_t a[8])
{
	register uint64_t x0 __asm__("x0") = a[0], x1 __asm__("x1") = a[1], x2 __asm__("x2") = a[2],
	    x3 __asm__("x3") = a[3], x4 __asm__("x4") = a[4], x5 __asm__("x5") = a[5], x6 __asm__("x6") = a[6],
	    x7 __asm__("x7") = a[7];
	__asm__ volatile ("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
            :
            : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	a[0] = x0; a[1] = x1; a[2] = x2; a[3] = x3; a[4] = x4; a[5] = x5; a[6] = x6; a[7] = x7;
}

static uint64_t
scm(uint32_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *res1)
{
	uint64_t args[8] = { fn, arginfo, a, b, c, 0, 0, 0 };

	smc8(args);
	for (int n = 0; args[0] == 1 && n < 100000; n++) {     /* interrupted: resume with the session in x6 */
		uint64_t session = args[6];
		uint64_t again[8] = { 1, arginfo, a, b, c, 0, session, 0 };
		memcpy(args, again, sizeof(args));
		smc8(args);
	}
	if (res1 != NULL) {
		*res1 = args[1];
	}
	return args[0];
}

static bool
zap_load(void)
{
	uint64_t mdt_size = 0, seg_size = 0, st, res = 0;
	const uint8_t *m = firmware("a610_zap.mdt", &mdt_size), *seg = firmware("a610_zap.b02", &seg_size);
	struct kbuf meta = {};

	if (m == NULL || seg == NULL || mdt_size < 52) {
		return false;
	}
	uint32_t phoff, phnum16, min_a = 0xffffffff, max_a = 0, seg_paddr = 0;
	uint16_t phnum;
	memcpy(&phoff, m + 28, 4);
	memcpy(&phnum, m + 44, 2);
	phnum16 = phnum;
	for (uint32_t i = 0; i < phnum16 && phoff + 32 * (i + 1) <= mdt_size; i++) {
		const uint8_t *ph = m + phoff + 32 * i;
		uint32_t type, paddr, memsz, flags;
		memcpy(&type, ph, 4);
		memcpy(&paddr, ph + 12, 4);
		memcpy(&memsz, ph + 20, 4);
		memcpy(&flags, ph + 24, 4);
		if (type == 1 && memsz != 0 && ((flags >> 24) & 7) != 2) {
			min_a = paddr < min_a ? paddr : min_a;
			uint32_t end = (paddr + memsz + 0xfff) & ~0xfffu;
			max_a = end > max_a ? end : max_a;
			seg_paddr = paddr;
		}
	}
	if (max_a <= min_a || max_a - min_a > ZAP_REGION_SIZE) {
		printf("jm21-a610: zap layout unexpected (%x..%x)\n", min_a, max_a);
		return false;
	}
	if (!kbuf_alloc(&meta, mdt_size)) {
		return false;
	}
	kbuf_copy(&meta, m, mdt_size);
	scm(SCM_FN(SCM_SVC_PIL, PIL_SHUTDOWN), 1, GPU_PAS_ID, 0, 0, &res);
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_INIT_IMAGE), 2 | (SCM_ARG_RW << 6), GPU_PAS_ID, meta.pa, 0, &res);
	if (st != 0 || res != 0) {
		printf("jm21-a610: zap init_image: %llx/%llx\n", st, res);
		return false;
	}
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_MEM_SETUP), 3, GPU_PAS_ID, ZAP_REGION, max_a - min_a, &res);
	if (st != 0 || res != 0) {
		printf("jm21-a610: zap mem_setup: %llx/%llx\n", st, res);
		return false;
	}
	volatile uint8_t *dst = (volatile uint8_t *)ml_io_map(ZAP_REGION, ZAP_REGION_SIZE);
	if (dst == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < ZAP_REGION_SIZE; i += 4) {
		*(volatile uint32_t *)(dst + i) = 0;
	}
	uint32_t at = seg_paddr - min_a, n = (uint32_t)(seg_size < ZAP_REGION_SIZE - at ? seg_size : ZAP_REGION_SIZE - at);
	for (uint32_t i = 0; i + 4 <= n; i += 4) {
		uint32_t w;
		memcpy(&w, seg + i, 4);
		*(volatile uint32_t *)(dst + at + i) = w;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_AUTH_RESET), 1, GPU_PAS_ID, 0, 0, &res);
	printf("jm21-a610: zap auth_and_reset: %llx/%llx\n", st, res);
	return st == 0 && res == 0;
}

/* ---------------------------------------------------------------- SMMU stage-1 tables (4 KB granule) */

#define PTE_VALID       (1ull << 0)
#define PTE_TABLE       (1ull << 1)
#define PTE_AP_UNPRIV   (1ull << 6)
#define PTE_AP_RO       (1ull << 7)
#define PTE_SH_OUTER    (2ull << 8)
#define PTE_AF          (1ull << 10)
#define PTE_PAGE        (PTE_VALID | PTE_TABLE | PTE_AP_UNPRIV | PTE_SH_OUTER | PTE_AF)

struct table { volatile uint64_t *va; uint64_t pa; };
static struct table l1, l2[4];
static struct table *l3[2048];
static struct kbuf table_chunk;
static uint32_t table_chunk_used;
static bool tables_dirty;

static bool
table_alloc(struct table *t)
{
	if (table_chunk.va == NULL || table_chunk_used == PAGE_SIZE / 4096) {
		struct kbuf b = {};
		if (!kbuf_alloc(&b, PAGE_SIZE)) {
			return false;
		}
		table_chunk = b;
		table_chunk_used = 0;
	}
	t->va = (volatile uint64_t *)(table_chunk.va + 4096 * table_chunk_used);
	t->pa = table_chunk.pa + 4096 * table_chunk_used;
	table_chunk_used++;
	return true;
}

static volatile uint64_t *
l3_for(uint64_t va, bool create)
{
	uint32_t i1 = (uint32_t)(va >> 30), i2 = (uint32_t)((va >> 21) & 511), i3 = (uint32_t)(va >> 21);

	if (i1 >= 4) {
		return NULL;
	}
	if (l2[i1].va == NULL) {
		if (!create || !table_alloc(&l2[i1])) {
			return NULL;
		}
		l1.va[i1] = l2[i1].pa | PTE_VALID | PTE_TABLE;
	}
	if (l3[i3] == NULL) {
		struct table *t;
		if (!create || (t = kalloc_type(struct table, Z_WAITOK | Z_ZERO)) == NULL) {
			return NULL;
		}
		if (!table_alloc(t)) {
			kfree_type(struct table, t);
			return NULL;
		}
		l3[i3] = t;
		l2[i1].va[i2] = t->pa | PTE_VALID | PTE_TABLE;
	}
	return l3[i3]->va;
}

static void
tlb_flush(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(smmu, SMMU_CB0 + 0x618, 0);                  /* CB_TLBIALL */
	wr(smmu, SMMU_CB0 + 0x7F0, 0);                  /* CB_TLBSYNC */
	poll_bits(smmu, SMMU_CB0 + 0x7F4, 1, 0, 1000);
}

static bool
mmu_map(uint64_t va, uint64_t pa, bool ro)
{
	volatile uint64_t *t = l3_for(va, true);

	if (t == NULL || (pa >> 32) != 0) {
		return false;
	}
	t[(va >> 12) & 511] = pa | PTE_PAGE | (ro ? PTE_AP_RO : 0);
	tables_dirty = true;
	return true;
}

static void
smmu_fault(const char *tag)
{
	printf("jm21-a610: [%s] SMMU gfsr %08x CB0 FSR %08x FAR %08x%08x FSYNR0 %08x\n", tag, rd(smmu, 0x48),
	    rd(smmu, SMMU_CB0 + 0x58), rd(smmu, SMMU_CB0 + 0x64), rd(smmu, SMMU_CB0 + 0x60), rd(smmu, SMMU_CB0 + 0x68));
}

static bool
smmu_setup(void)
{
	if (l1.va == NULL && !table_alloc(&l1)) {
		return false;
	}
	wr(smmu, SMMU_CB0 + 0x000, 0);                  /* SCTLR off while programming */
	wr(smmu, SMMU_CB0 + 0x058, 0xFFFFFFFF);         /* FSR clear */
	wr(smmu, 0x1000 + 0x800, 1);                    /* CBA2R0: AArch64 descriptors */
	wr(smmu, 0x1000, 0x0001f300);                   /* CBAR0: S1 translate, S2 bypass */
	wr(smmu, SMMU_CB0 + 0x020, (uint32_t)l1.pa);
	wr(smmu, SMMU_CB0 + 0x024, (uint32_t)(l1.pa >> 32));
	wr(smmu, SMMU_CB0 + 0x030, 25 | (1u << 23));    /* TCR: 39-bit IOVA, three levels, NC walks, EPD1 */
	wr(smmu, SMMU_CB0 + 0x010, 1u | (1u << 4) | (7u << 15));                  /* TCR2: PASize 36 bits */
	wr(smmu, SMMU_CB0 + 0x038, 0x44);               /* MAIR0: attr0 = Normal non-cacheable */
	wr(smmu, SMMU_CB0 + 0x03C, 0);
	wr(smmu, SMMU_CB0 + 0x000, 0x1027);               /* SCTLR: CFIE CFRE AFE TRE M */
	wr(smmu, 0xC00, 0);                             /* S2CR0 -> CB0 */
	wr(smmu, 0x800, (1u << 31) | (1u << 16));       /* SMR0: SID 0 mask 1 */
	tlb_flush();
	printf("jm21-a610: SMMU TTBR0 %08x%08x SCTLR %08x TCR %08x IDR2 %08x\n", rd(smmu, SMMU_CB0 + 0x24),
	    rd(smmu, SMMU_CB0 + 0x20), rd(smmu, SMMU_CB0), rd(smmu, SMMU_CB0 + 0x30), rd(smmu, 0x28));
	return true;
}

static bool
kbuf_map(struct kbuf *b)
{
	b->iova = kernel_va_next;
	kernel_va_next += b->size + 4096;
	for (uint64_t off = 0; off < b->size; off += 4096) {
		if (!mmu_map(b->iova + off, b->pa + off, false)) {
			return false;
		}
	}
	return true;
}

/* ---------------------------------------------------------------- GPU VA allocator */

static struct { uint64_t start, size; } va_free[1024];
static unsigned va_free_count;

uint64_t
a610_address(uint64_t size)
{
	size = (size + 0xffff) & ~0xffffull;
	for (unsigned i = 0; i < va_free_count; i++) {
		if (va_free[i].size >= size) {
			uint64_t address = va_free[i].start;
			va_free[i].start += size;
			va_free[i].size -= size;
			if (va_free[i].size == 0) {
				memmove(&va_free[i], &va_free[i + 1], (--va_free_count - i) * sizeof(va_free[0]));
			}
			return address;
		}
	}
	return 0;
}

void
a610_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = (size + 0xffff) & ~0xffffull;
	for (at = 0; at < va_free_count && va_free[at].start < address; at++) {
	}
	if (at > 0 && va_free[at - 1].start + va_free[at - 1].size == address) {
		va_free[at - 1].size += size;
		if (at < va_free_count && address + size == va_free[at].start) {
			va_free[at - 1].size += va_free[at].size;
			memmove(&va_free[at], &va_free[at + 1], (--va_free_count - at) * sizeof(va_free[0]));
		}
	} else if (at < va_free_count && address + size == va_free[at].start) {
		va_free[at].start = address;
		va_free[at].size += size;
	} else if (va_free_count < sizeof(va_free) / sizeof(va_free[0])) {
		memmove(&va_free[at + 1], &va_free[at], (va_free_count++ - at) * sizeof(va_free[0]));
		va_free[at].start = address;
		va_free[at].size = size;
	}
}

/* ---------------------------------------------------------------- CP */

static uint32_t
par(uint32_t v)
{
	return (0x9669 >> (0xF & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (par(cnt) << 15) | ((op & 0x7F) << 16) | (par(op) << 23);
}

static void
emit(uint32_t v)
{
	((volatile uint32_t *)ring.va)[wptr % RB_DWORDS] = v;
	wptr++;
}

static void
kick(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	gpu_wr(0x807, wptr % RB_DWORDS);                /* CP_RB_WPTR */
}

static uint32_t
ring_free(void)
{
	uint32_t r = gpu_rd(0x806), w = wptr % RB_DWORDS;
	return (r + RB_DWORDS - w - 1) % RB_DWORDS;
}

static bool
ring_idle(uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 100; i++) {
		if (gpu_rd(0x806) == wptr % RB_DWORDS) {
			return true;
		}
		delay(10);
	}
	printf("jm21-a610: ring stuck: rptr %u wptr %u RBBM_STATUS %08x CP_HW_FAULT %08x\n", gpu_rd(0x806),
	    wptr % RB_DWORDS, gpu_rd(0x210), gpu_rd(0x821));
	smmu_fault("ring");
	return false;
}

static const uint32_t a612_hwcg[][2] = {        /* Linux a6xx_catalog.c a612_hwcg (the A610 uses it) */
	{ 0x000B0, 0x22222222 }, { 0x000B4, 0x02222220 }, { 0x000B8, 0x00000081 }, { 0x000BC, 0x0000f3cf },
	{ 0x000C0, 0x22222222 }, { 0x000C4, 0x22222222 }, { 0x000C8, 0x22222222 }, { 0x000CC, 0x00022222 },
	{ 0x000D0, 0x11111111 }, { 0x000D4, 0x11111111 }, { 0x000D8, 0x11111111 }, { 0x000DC, 0x00011111 },
	{ 0x000E0, 0x77777777 }, { 0x000E4, 0x77777777 }, { 0x000E8, 0x77777777 }, { 0x000EC, 0x00077777 },
	{ 0x000F0, 0x22222222 }, { 0x000F4, 0x01202222 }, { 0x000F8, 0x00002220 }, { 0x00100, 0x00040f00 },
	{ 0x00104, 0x05522022 }, { 0x00105, 0x00005555 }, { 0x00106, 0x00000011 }, { 0x00107, 0x00445044 },
	{ 0x00108, 0x04222222 }, { 0x00111, 0x00002222 }, { 0x00114, 0x02222222 }, { 0x00117, 0x00000002 },
	{ 0x0011B, 0x00002222 }, { 0x00109, 0x00004000 }, { 0x00112, 0x00002222 }, { 0x00115, 0x00000200 },
	{ 0x0011C, 0x00000000 }, { 0x0010A, 0x00000000 }, { 0x00113, 0x00000000 }, { 0x00116, 0x04104004 },
	{ 0x0011D, 0x00000000 }, { 0x0010B, 0x22222222 }, { 0x00110, 0x00000004 }, { 0x0010F, 0x00000002 },
	{ 0x00533, 0x00000182 }, { 0x00044, 0x00000000 }, { 0x00042, 0x00000000 }, { 0x00118, 0x00000222 },
	{ 0x00119, 0x00000111 }, { 0x0011A, 0x00000555 },
};

static void
hw_init_regs(void)
{
	static const uint32_t addr_mode[] = { 0x842, 0xC01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0xE00,
		                              0xae01, 0xb601, 0xF810 };

	gpu_wr(0x3c45, 0);
	(void)gpu_rd(0x3c45);
	gpu_wr(0x16, 0);
	(void)gpu_rd(0x16);
	gpu_wr(0xF803, 0);
	gpu_wr64(0xF800, 0);
	gpu_wr(0xF802, 0);
	for (unsigned i = 0; i < sizeof(addr_mode) / sizeof(addr_mode[0]); i++) {
		gpu_wr(addr_mode[i], 1);
	}
	for (unsigned i = 0; i < sizeof(a612_hwcg) / sizeof(a612_hwcg[0]); i++) {
		gpu_wr(a612_hwcg[i][0], a612_hwcg[i][1]);
	}
	gpu_wr(0xAE, 0xaaa8aa82);                       /* RBBM_CLOCK_CNTL */
	for (unsigned i = 0; i < 4; i++) {
		gpu_wr(0x3c03 + i, 0x00071620);
	}
	gpu_wr(0x11, 3);
	gpu_wr64(0xE05, 0x1fffffffff000ull + 0xfc0);
	gpu_wr64(0xE09, 0x1fffffffff000ull);
	gpu_wr64(0xE07, 0x1fffffffff000ull);
	gpu_wr64(0xE0B, GMEM_BASE);
	gpu_wr64(0xE0D, GMEM_BASE + GMEM_SIZE - 1);
	gpu_wr(0xE18, 0x804);
	gpu_wr(0xE17, 4);
	gpu_wr(0x8c2, 0x00800060);
	gpu_wr(0x8c1, 0x40201b16);
	gpu_wr(0x8C3, 48);
	gpu_wr(0x9e00, 0x00080000);
	gpu_wr(0x98d, 1);
	gpu_wr(0x1f, (1u << 30) | 0x3ffff);
	gpu_wr(0xe19, 0x81);
	gpu_wr(0x500, 1);
	gpu_wr(0x90E, 47);
	gpu_wr(0x50B, 0xffffffff);
	gpu_wr(0x8D0, 0);
	gpu_wr(0x8e08, 0x9);
	gpu_wr(0xb604, 0x9);
	gpu_wr(0xae02, 0x9);
	gpu_wr(0xE01, 1u << 23);
	gpu_wr(0x534, 0);
}

static bool
cp_start(void)
{
	uint64_t fw_size = 0;
	const uint8_t *fw = firmware("a630_sqe.fw", &fw_size);

	if (fw == NULL || fw_size <= 4) {
		return false;
	}
	if ((sqe.va == NULL && (!kbuf_alloc(&sqe, fw_size - 4) || !kbuf_map(&sqe))) ||
	    (ring.va == NULL && (!kbuf_alloc(&ring, RB_BYTES) || !kbuf_map(&ring))) ||
	    (mem.va == NULL && (!kbuf_alloc(&mem, PAGE_SIZE) || !kbuf_map(&mem)))) {
		printf("jm21-a610: CP buffers failed\n");
		return false;
	}
	kbuf_copy(&sqe, fw + 4, fw_size - 4);
	tlb_flush();
	printf("jm21-a610: PT root %llx L1[3] %llx L2[384] %llx L3[0] %llx SQE %llx->%llx IDR1 %08x TCR2 %08x CBAR %08x CBA2R %08x\n",
	    l1.pa, l1.va[3], l2[3].va[384], l3[1920]->va[0], sqe.iova, sqe.pa,
	    rd(smmu, 0x24), rd(smmu, SMMU_CB0 + 0x10), rd(smmu, 0x1000), rd(smmu, 0x1800));
	hw_init_regs();
	gpu_wr64(0x830, sqe.iova);                      /* CP_SQE_INSTR_BASE */
	gpu_wr64(0x800, ring.iova);                     /* CP_RB_BASE */
	gpu_wr(0x802, 12 | (2 << 8) | (1u << 27));      /* CP_RB_CNTL */
	wptr = 0;
	gpu_wr(0x807, 0);
	__asm__ volatile ("dsb sy" ::: "memory");
	gpu_wr(0x808, 1);                               /* CP_SQE_CNTL */

	emit(pkt7(CP_ME_INIT, 8));
	emit(0x2f);
	emit(3);
	emit(0x20000000);
	emit(0);
	emit(0);
	emit(0);
	emit(0);
	emit(0);
	kick();
	if (!ring_idle(100)) {
		return false;
	}
	emit(pkt7(CP_SET_SECURE_MODE, 1));
	emit(0);
	kick();
	if (!ring_idle(100)) {
		return false;
	}
	printf("jm21-a610: CP up (ME_INIT, SET_SECURE_MODE 0), core %u MHz, fixed performance (RCG CFG=%08x)\n", core_mhz, rd(gpucc, 0x1020));
	return true;
}

/* ---------------------------------------------------------------- the hakios_gpu_* interface */

bool
a610_ready(void)
{
	return ready;
}

bool
a610_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01: *value = 610; return true;                   /* GPU_ID */
	case 0x02: *value = GMEM_SIZE; return true;             /* GMEM_SIZE */
	case 0x03: *value = 0x06010001; return true;            /* CHIP_ID */
	case 0x04: *value = core_mhz * 1000000ull; return true; /* MAX_FREQ */
	case 0x05:                                              /* TIMESTAMP: CP always-on counter */
		*value = ready ? ((uint64_t)gpu_rd(0x981) << 32 | gpu_rd(0x980)) : 0;
		return true;
	case 0x06: *value = GMEM_BASE; return true;
	case 0x07: *value = 1; return true;                     /* PRIORITIES */
	case 0x08: case 0x09: case 0x0a: case 0x11:
		*value = 0;
		return true;
	case 0x10: *value = 13; return true;                    /* HIGHEST_BANK_BIT */
	case 0x12: *value = 7; return true;                     /* UBWC_SWIZZLE */
	case 0x13: *value = 0; return true;                     /* MACROTILE_MODE */
	default:
		return false;
	}
}

bool
a610_map(uint64_t address, uint64_t phys, bool read_only)
{
	return mmu_map(address, phys, read_only);
}

void
a610_mapped(void)
{
	if (tables_dirty) {
		tables_dirty = false;
		tlb_flush();
	}
}

void
a610_unmap(uint64_t address, uint64_t size)
{
	for (uint64_t off = 0; off < size; off += 4096) {
		volatile uint64_t *t = l3_for(address + off, false);
		if (t != NULL) {
			t[((address + off) >> 12) & 511] = 0;
		}
	}
	tlb_flush();
}

void
a610_to_memory(void *start, uint64_t size)
{
	if (start != NULL && size != 0) {
		FlushPoC_DcacheRegion((vm_offset_t)start, (size_t)size);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

bool
a610_done(uint32_t fence)
{
	return mem.va != NULL && (int32_t)(*(volatile uint32_t *)mem.va - fence) >= 0;
}

uint32_t
a610_last(void)
{
	return seqno;
}

void
a610_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	*microseconds = 0;
	*submissions = 0;
	bzero(by, 7 * sizeof(uint32_t));
}

uint32_t
a610_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t need;

	if (count > (RB_DWORDS - 6) / 4) return 0;
	need = 4 * count + 5;

	if (!ready) {
		return 0;
	}
	lck_mtx_lock(&a610_lock);
	for (int spin = 0; ring_free() < need && spin < 20000; spin++) {
		delay(100);
	}
	if (ring_free() < need) {
		lck_mtx_unlock(&a610_lock);
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		emit(pkt7(CP_INDIRECT_BUFFER, 3));
		emit((uint32_t)addresses[i]);
		emit((uint32_t)(addresses[i] >> 32));
		emit(words[i]);
	}
	if (++seqno == 0) {
		seqno = 1;
	}
	uint32_t fence = seqno;
	emit(pkt7(CP_EVENT_WRITE, 4));
	emit(CACHE_FLUSH_TS);
	emit((uint32_t)mem.iova);
	emit((uint32_t)(mem.iova >> 32));
	emit(fence);
	kick();
	lck_mtx_unlock(&a610_lock);
	return fence;
}

void
a610_report(const char *why)
{
	uint64_t ib1 = gpu_rd(0x928) | ((uint64_t)gpu_rd(0x929) << 32);

	printf("jm21-a610: %s: fence %u done %u rptr %u wptr %u RBBM_STATUS %08x CP_HW_FAULT %08x IB1 %llx rem %u\n", why,
	    seqno, mem.va ? *(volatile uint32_t *)mem.va : 0, gpu_rd(0x806), wptr % RB_DWORDS, gpu_rd(0x210),
	    gpu_rd(0x821), ib1, gpu_rd(0x92A));
	smmu_fault(why);
}

/* Stop command fetching and translation before the loader reuses kernel memory. */
void
a610_stop(void)
{
	ready = false;
	if (gpu != NULL && phase >= 4) {
		gpu_wr(0x808, 0); /* CP_SQE_CNTL: halt */
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	if (smmu != NULL && phase >= 3) {
		wr(smmu, SMMU_CB0, 0);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static void
a610_thread(__unused void *param, __unused wait_result_t w)
{
	bool ok;

	IOSleep(2000);                                  /* after the RAM disk and the early drivers */
	gcc = (volatile uint8_t *)ml_io_map(GCC_BASE, GCC_SIZE);
	gpucc = (volatile uint8_t *)ml_io_map(GPUCC_BASE, GPUCC_SIZE);
	gpu = (volatile uint8_t *)ml_io_map(GPU_BASE, GPU_SIZE);
	smmu = (volatile uint8_t *)ml_io_map(SMMU_BASE, SMMU_SIZE);
	if (gcc == NULL || gpucc == NULL || gpu == NULL || smmu == NULL) {
		printf("jm21-a610: map failed\n");
		phase = -1;
		return;
	}
	va_free_count = 0;
	a610_address_free(VA_USER_START, VA_KERNEL - VA_USER_START);
	lck_mtx_lock(&a610_lock);
	phase = 1;
	ok = power_up();
	if (ok) {
		printf("jm21-a610: powered: RBBM_STATUS %08x\n", gpu_rd(0x210));
		phase = 2;
		ok = zap_load();
	}
	if (ok) {
		phase = 3;
		ok = smmu_setup();
	}
	if (ok) {
		phase = 4;
		ok = cp_start();
	}
	lck_mtx_unlock(&a610_lock);
	if (!ok) {
		a610_stop();
		phase = -phase;
		printf("jm21-a610: THE GPU IS NOT STARTED (phase %d); CoreAnimation keeps the software renderer\n", phase);
		return;
	}
	ready = true;
	a610_active = true;
	phase = 5;
	HakiosGPUStart();
	printf("jm21-a610: Adreno 610 ready, HakiosGPU published\n");
}

void
hakios_jm21_a610_init(void)
{
	uint32_t on = 0, ios = 0;
	thread_t t;

	if (!PE_parse_boot_argn("jm21_ios", &ios, sizeof(ios)) || !ios ||
	    !PE_parse_boot_argn("jm21_a610", &on, sizeof(on)) || !on) {
		return;
	}
	a610_active = true;                     /* route hakios_gpu_* here from now on (ready says when it works) */
	if (kernel_thread_start(a610_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}
