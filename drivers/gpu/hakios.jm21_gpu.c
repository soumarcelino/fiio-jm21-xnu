/* FiiO JM21 (khaje, Adreno 610) GPU/display probe, phase 0: READ-ONLY.
 * Opt-in jm21_ios=1 jm21_gpu=1. Never writes a register. Every read is checked against
 * the DT region it belongs to (postmarketos-fiio-jm21/evidence/vendor.dts, lines in comments).
 * GPUCC is read only if GCC_GPU_CFG_AHB is running; kgsl-smmu and RBBM only if the CX GDSC
 * (and for RBBM also GX) reports PWR_ON, so a powered-down block is never touched.
 * Results: printf "jm21-gpu: ..." and sysctl kern.jm21_gpu_*. */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

static int phase, gdsc = -1, intf_hz, intf_mhz, scanout;
static char regs[3072];
SYSCTL_INT(_kern, OID_AUTO, jm21_gpu_phase, CTLFLAG_RD | CTLFLAG_LOCKED, &phase, 0,
    "JM21 GPU probe: 1 running, 2 done, negative = failed at that step");
SYSCTL_INT(_kern, OID_AUTO, jm21_gpu_gdsc, CTLFLAG_RD | CTLFLAG_LOCKED, &gdsc, 0,
    "bit0 GPU CX PWR_ON, bit1 GPU GX PWR_ON, bit2 MDSS PWR_ON, -1 unknown (GPUCC not readable)");
SYSCTL_INT(_kern, OID_AUTO, jm21_intf_hz, CTLFLAG_RD | CTLFLAG_LOCKED, &intf_hz, 0, "INTF1 frames/s (rounded)");
SYSCTL_INT(_kern, OID_AUTO, jm21_intf_mhz, CTLFLAG_RD | CTLFLAG_LOCKED, &intf_mhz, 0, "INTF1 frames/s x1000");
SYSCTL_INT(_kern, OID_AUTO, jm21_gpu_scanout, CTLFLAG_RD | CTLFLAG_LOCKED, &scanout, 0, "first enabled SSPP SRC0_ADDR");
SYSCTL_STRING(_kern, OID_AUTO, jm21_gpu_regs, CTLFLAG_RD | CTLFLAG_LOCKED, regs, sizeof(regs),
    "name=value of every register read by the probe");

/* DT regions. */
enum { GCC, GPUCC, QFPROM, KGSL, KGSL_SMMU, MDP, DISPCC, APPS_SMMU, NREG };
static const struct { const char *name; uint64_t base, size; } region[NREG] = {
	[GCC]       = {"gcc",       0x01400000, 0x1f0000}, /* DTS:5275-5277 */
	[GPUCC]     = {"gpucc",     0x05990000, 0x9000},   /* DTS:5300-5311 */
	[QFPROM]    = {"qfprom",    0x01b40000, 0x7000},   /* DTS:5725-5727 */
	[KGSL]      = {"kgsl",      0x05900000, 0x90000},  /* DTS:6429 */
	[KGSL_SMMU] = {"kgsl_smmu", 0x059a0000, 0x10000},  /* DTS:7165-7169 */
	[MDP]       = {"mdp",       0x05e00000, 0x8f030},  /* DTS:9498-9499 */
	[DISPCC]    = {"dispcc",    0x05f00000, 0x20000},  /* DTS:5288-5294; mdss gdsc 0x5f01004 DTS:6953 */
	[APPS_SMMU] = {"apps_smmu", 0x0c600000, 0x80000},  /* DTS:7198-7202 */
};
static struct { uint64_t pa; vm_offset_t va; } maps[48];
static unsigned nmaps;
static bool bad;

static size_t used;
static void note(const char *name, uint32_t v) {
	printf("jm21-gpu: %s=%#x\n", name, v);
	if (used + 40 < sizeof(regs)) used += (size_t)snprintf(regs + used, sizeof(regs) - used, "%s=%#x ", name, v);
}

/* Read 32 bits at region r + off; refuses anything outside the DT region. */
static bool rd(unsigned r, uint64_t off, uint32_t *v) {
	if (r >= NREG || (off & 3) || off + 4 > region[r].size) {
		printf("jm21-gpu: refused %s+%#llx (outside DT region)\n", r < NREG ? region[r].name : "?", off);
		bad = true;
		return false;
	}
	uint64_t pa = region[r].base + off, page = pa & ~0xfffULL;
	vm_offset_t va = 0;
	for (unsigned i = 0; i < nmaps; i++) if (maps[i].pa == page) { va = maps[i].va; break; }
	if (!va) {
		if (nmaps == sizeof(maps) / sizeof(maps[0])) return false;
		va = ml_io_map((vm_offset_t)page, 0x1000);
		if (!va) return false;
		maps[nmaps].pa = page; maps[nmaps].va = va; nmaps++;
	}
	*v = *(volatile uint32_t *)(va + (pa - page));
	__asm__ volatile("dsb sy" ::: "memory");
	return true;
}
static uint32_t rn(unsigned r, uint64_t off, const char *name) {
	uint32_t v = 0xdeadbeef;
	if (rd(r, off, &v)) note(name, v);
	return v;
}

static uint64_t now_ns(void) {
	uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;
}

/* SMMUv2 global space: SMR 0x800, S2CR 0xc00, GR1 CBAR at +pagesize, CBs in the upper half. */
static void smmu_sid(uint16_t sid) {
	uint32_t idr0 = rn(APPS_SMMU, 0x20, "smmu_idr0"), idr1 = rn(APPS_SMMU, 0x24, "smmu_idr1");
	rn(APPS_SMMU, 0x0, "smmu_scr0");
	unsigned nsmr = idr0 & 0xff, ncb = idr1 & 0xff;
	uint64_t pg = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
	uint64_t cbbase = pg << (((idr1 >> 28) & 7) + 1);
	printf("jm21-gpu: apps_smmu smr=%u cb=%u page=%#llx cbbase=%#llx\n", nsmr, ncb, pg, cbbase);
	bool found = false;
	for (unsigned n = 0; n < nsmr && n < 128; n++) {
		uint32_t smr;
		if (!rd(APPS_SMMU, 0x800 + 4 * n, &smr)) break;
		if (!(smr & (1u << 31))) continue;
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		if (((id ^ sid) & ~mask & 0x7fff) != 0) continue;
		char nm[32];
		found = true;
		snprintf(nm, sizeof(nm), "smr%u", n); note(nm, smr);
		uint32_t s2cr = 0;
		snprintf(nm, sizeof(nm), "s2cr%u", n); s2cr = rn(APPS_SMMU, 0xc00 + 4 * n, nm);
		unsigned type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		printf("jm21-gpu: SID %#x matches SMR%u id=%#x mask=%#x -> S2CR type=%s cb=%u\n", sid, n, id, mask,
		    type == 0 ? "translate" : type == 1 ? "bypass" : "fault", cb);
		if (type != 0 || cb >= ncb) continue;
		snprintf(nm, sizeof(nm), "cbar%u", cb); rn(APPS_SMMU, pg + 4 * cb, nm);
		uint64_t c = cbbase + pg * cb;
		snprintf(nm, sizeof(nm), "cb%u_sctlr", cb); rn(APPS_SMMU, c + 0x0, nm);
		snprintf(nm, sizeof(nm), "cb%u_ttbr0_lo", cb); rn(APPS_SMMU, c + 0x20, nm);
		snprintf(nm, sizeof(nm), "cb%u_ttbr0_hi", cb); rn(APPS_SMMU, c + 0x24, nm);
		snprintf(nm, sizeof(nm), "cb%u_tcr", cb); rn(APPS_SMMU, c + 0x30, nm);
		snprintf(nm, sizeof(nm), "cb%u_fsr", cb); rn(APPS_SMMU, c + 0x58, nm);
	}
	if (!found) printf("jm21-gpu: SID %#x matches no valid SMR\n", sid);
}

static void sspp(const char *tag, uint64_t off) {
	char nm[40];
#define R(o, s) (snprintf(nm, sizeof(nm), "%s_%s", tag, s), rn(MDP, off + (o), nm))
	uint32_t size = R(0x00, "src_size"), img = R(0x04, "src_img_size"), xy = R(0x08, "src_xy");
	uint32_t out = R(0x0c, "out_size"), oxy = R(0x10, "out_xy"), a0 = R(0x14, "src0_addr");
	uint32_t st = R(0x24, "ystride0"), fmt = R(0x30, "src_format");
	R(0x34, "unpack"); R(0x38, "op_mode");
#undef R
	printf("jm21-gpu: %s src %ux%u (img %ux%u at %u,%u) out %ux%u at %u,%u addr=%#x stride=%u bpp=%u fmt=%#x\n",
	    tag, size & 0xffff, size >> 16, img & 0xffff, img >> 16, xy & 0xffff, xy >> 16,
	    out & 0xffff, out >> 16, oxy & 0xffff, oxy >> 16, a0, st & 0xffff, ((fmt >> 9) & 3) + 1, fmt);
	if (a0 && size && !scanout) scanout = (int)a0;
}

static void probe(__unused void *p, __unused wait_result_t w) {
	uint32_t v;
	phase = 1;
	printf("jm21-gpu: phase 0 read-only probe (no register is written)\n");

	/* 1. GCC: GPU branches, CBCR bit31 = CLK_OFF. */
	uint32_t cfg_ahb = rn(GCC, 0x36004, "gcc_gpu_cfg_ahb_cbcr");
	rn(GCC, 0x3600c, "gcc_gpu_memnoc_gfx_cbcr");
	rn(GCC, 0x36018, "gcc_gpu_snoc_dvm_gfx_cbcr");
	rn(GCC, 0x79004, "gcc_branch_vote_gpll0_gpu");
	if (bad) { phase = -1; goto out; }

	/* 2. GPUCC only if its AHB interface clock runs. */
	bool cx = false, gx = false;
	if (cfg_ahb != 0xdeadbeef && !(cfg_ahb & (1u << 31))) {
		uint32_t gxr = rn(GPUCC, 0x100c, "gpucc_gx_gdscr"), cxr = rn(GPUCC, 0x106c, "gpucc_cx_gdscr");
		rn(GPUCC, 0x1540, "gpucc_cx_hw_ctrl"); rn(GPUCC, 0x1508, "gpucc_gx_domain");
		rn(GPUCC, 0x1008, "gpucc_gx_sw_reset");
		/* [H] CBCR offsets from mainline gpucc-sm6115.c, still to confirm in gpucc-khaje.ko. */
		rn(GPUCC, 0x0000, "gpucc_pll0_mode_h"); rn(GPUCC, 0x1078, "gpucc_ahb_cbcr_h");
		rn(GPUCC, 0x10a4, "gpucc_cx_gfx3d_cbcr_h"); rn(GPUCC, 0x1098, "gpucc_cx_gmu_cbcr_h");
		rn(GPUCC, 0x109c, "gpucc_cxo_cbcr_h"); rn(GPUCC, 0x1054, "gpucc_gx_gfx3d_cbcr_h");
		rn(GPUCC, 0x7000, "gpucc_hlos1_vote_smmu_h");
		cx = (cxr & (1u << 31)) && cxr != 0xdeadbeef;
		gx = (gxr & (1u << 31)) && gxr != 0xdeadbeef;
		gdsc = (cx ? 1 : 0) | (gx ? 2 : 0);
		printf("jm21-gpu: GPU CX GDSC %s, GX GDSC %s\n", cx ? "ON" : "off", gx ? "ON" : "off");
	} else
		printf("jm21-gpu: GCC_GPU_CFG_AHB off: GPUCC/GDSC not read (state unknown)\n");

	/* 3. Speed bin: QFPROM byte 0x6006 bits 5..12 (2 bytes), gaming bin 0x602d bit5. */
	if (rd(QFPROM, 0x6004, &v)) {
		note("qfprom_6004", v);
		printf("jm21-gpu: gpu_speed_bin=%u\n", (v >> 21) & 0xff);
		note("gpu_speed_bin", (v >> 21) & 0xff);
	}
	if (rd(QFPROM, 0x602c, &v)) {
		note("qfprom_602c", v);
		note("gpu_gaming_bin", (v >> 13) & 1);
	}

	/* 4. kgsl-smmu / RBBM only with the CX (and GX) domains powered. */
	if (cx) {
		rn(KGSL_SMMU, 0x20, "kgsl_smmu_idr0");
		rn(KGSL_SMMU, 0x24, "kgsl_smmu_idr1");
		if (gx) {
			rn(KGSL, 0x210 * 4, "rbbm_status");
			rn(KGSL, 0x980 * 4, "cp_always_on_lo");
		} else
			printf("jm21-gpu: GX off: RBBM not read\n");
	} else
		printf("jm21-gpu: CX off or unknown: kgsl-smmu and RBBM not read\n");

	/* 5. Display: MDSS GDSC first, then MDP only if powered. */
	uint32_t mdss = rn(DISPCC, 0x1004, "mdss_core_gdscr");
	if (mdss == 0xdeadbeef || !(mdss & (1u << 31))) {
		printf("jm21-gpu: MDSS GDSC off: MDP not read\n");
		phase = -5; goto out;
	}
	if (gdsc >= 0) gdsc |= 4;
	rn(MDP, 0x0, "mdss_hw_version");
	rn(MDP, 0x1000, "mdp_top_hw_version");
	rn(MDP, 0x2000, "ctl0_layer_lm0"); rn(MDP, 0x2014, "ctl0_top"); rn(MDP, 0x2018, "ctl0_flush");
	rn(MDP, 0x45000, "lm0_op_mode"); rn(MDP, 0x45004, "lm0_out_size");
	sspp("vig0", 0x5000);
	sspp("dma0", 0x25000);

	/* INTF1 (DSI) at 0x6b800: timing and counters. */
	const uint64_t I = 0x6b800;
	rn(MDP, I + 0x000, "intf1_timing_en"); rn(MDP, I + 0x004, "intf1_config");
	uint32_t hs = rn(MDP, I + 0x008, "intf1_hsync_ctl"), vp = rn(MDP, I + 0x00c, "intf1_vsync_period_f0");
	rn(MDP, I + 0x01c, "intf1_disp_v_start"); rn(MDP, I + 0x024, "intf1_disp_v_end");
	rn(MDP, I + 0x03c, "intf1_disp_hctl"); rn(MDP, I + 0x090, "intf1_panel_format");
	uint32_t en = rn(MDP, I + 0x0a8, "intf1_frame_line_count_en");
	if (hs >> 16) printf("jm21-gpu: INTF1 htotal=%u vtotal=%u lines\n", hs >> 16, vp / (hs >> 16));
	uint32_t f0 = 0, f1 = 0, l = 0, prev = 0, wraps = 0;
	rd(MDP, I + 0x0ac, &f0);
	uint64_t t0 = now_ns();
	/* Count line-counter wraps for 1 s as a second Hz estimate (reads only). */
	rd(MDP, I + 0x0b0, &prev);
	while (now_ns() - t0 < 1000000000ULL) {
		if (!rd(MDP, I + 0x0b0, &l)) break;
		if (l < prev) wraps++;
		prev = l;
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	rd(MDP, I + 0x0ac, &f1);
	uint64_t dt = now_ns() - t0;
	note("intf1_frame_count_0", f0); note("intf1_frame_count_1", f1); note("intf1_line_wraps", wraps);
	uint32_t df = f1 - f0;
	if (!df) df = wraps;
	intf_mhz = (int)((uint64_t)df * 1000000000000ULL / dt);
	intf_hz = (intf_mhz + 500) / 1000;
	printf("jm21-gpu: INTF1 en=%#x frames %u->%u, line wraps=%u in %llu us -> %d.%03d Hz\n",
	    en, f0, f1, wraps, dt / 1000, intf_mhz / 1000, intf_mhz % 1000);

	/* 6. apps_smmu entry for the MDP's SID 0x420. */
	smmu_sid(0x420);
	phase = bad ? -6 : 2;
out:
	printf("jm21-gpu: done phase=%d gdsc=%d scanout=%#x hz=%d\n", phase, gdsc, scanout, intf_hz);
	thread_terminate(current_thread());
}

void hakios_jm21_gpu_init(void) {
	unsigned on = 0, jm21 = 0; thread_t t;
	if (!PE_parse_boot_argn("jm21_ios", &jm21, sizeof(jm21)) || !jm21 ||
	    !PE_parse_boot_argn("jm21_gpu", &on, sizeof(on)) || on != 1) return;
	if (kernel_thread_start(probe, NULL, &t) == KERN_SUCCESS) thread_deallocate(t);
}
