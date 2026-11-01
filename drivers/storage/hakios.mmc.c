/*
 * The FiiO JM21's eMMC (Qualcomm khaje, sdhc_1: "qcom,sdhci-msm-v5" at 0x4744000) as block devices:
 *
 *   /dev/rmmc0                 the whole user area, read only, for looking at the GPT
 *   /dev/mmcroot, /dev/rmmcroot the GPT partition named "userdata", where the iOS root lives
 *
 * Only with boot-arg mmc=1 (read only) or mmc=2 (writes, and only inside "userdata"). Nothing else on the
 * eMMC is ever written: the boot chain, the modem's and the other partitions are outside every write.
 *
 * The boot loader (ABL) switches the controller off on its way out: bus power and the card clock are off,
 * and the card has to be initialised again, the way Linux's mmc core does it (mmc_attach_mmc): 1.8 V bus
 * power, 400 kHz, CMD0, CMD1 until ready, CMD2, CMD3 (RCA 1), CMD7, then a faster clock. The sequence was
 * first proved by the loader's USB monitor (fiio-xnu-poc-display, usb.c emmc_*) on the phone; the order
 * of the power writes matches lechaukha12/xnu-xzs (an XNU eMMC driver on MSM8996's SDCC1).
 *
 * Transfers use PIO or opt-in ADMA2 through a contiguous bounce buffer, one request at a time.
 * Runtime waits may sleep after a short spin; interrupt signals remain disabled. GCC's SDCC1 clocks are left as ABL
 * leaves them, on; the driver checks that they are before it touches the controller, because a gated block
 * can hang the bus instead of faulting.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/hakios.mmc.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void IOSleep(unsigned int milliseconds);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define SDHC_PHYS       0x4744000UL     /* sdhc_1 "hc" */
#define CQHCI_PHYS      0x4745000UL     /* sdhc_1 "cqhci": legacy commands only while CQE is off */
#define GCC_SDCC1_PHYS  0x1438000UL     /* khaje-gcc + 0x38000: APPS branch at +4, AHB at +8 (CBCR bit 31: off) */

/* SDHCI registers (SD Host Controller Simplified Specification 3.00, as in Linux's sdhci.h) */
#define SDHCI_BLOCK_SIZE        0x04    /* and BLOCK_COUNT at 0x06 */
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0c    /* and COMMAND at 0x0e */
#define SDHCI_RESPONSE          0x10
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28    /* and POWER_CONTROL at 0x29 */
#define SDHCI_CLOCK_CONTROL     0x2c    /* and TIMEOUT_CONTROL at 0x2e, SOFTWARE_RESET at 0x2f */
#define SDHCI_INT_STATUS        0x30
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_SIGNAL_ENABLE     0x38
#define MSM_PWRCTL_STATUS       0x240   /* sdhci-msm v5 core registers, in the same space */
#define MSM_PWRCTL_CLEAR        0x248
#define MSM_PWRCTL_CTL          0x24c

#define INT_CMD_DONE            0x0001
#define INT_XFER_DONE           0x0002
#define INT_WRITE_READY         0x0010
#define INT_READ_READY          0x0020
#define INT_ERROR               0x8000
#define PRESENT_CMD_INHIBIT     0x0001
#define PRESENT_DAT_INHIBIT     0x0002
#define PRESENT_DAT0            (1u << 20)

/* COMMAND register flags: response type, CRC and index checks, data present */
#define RSP_NONE        0x00
#define RSP_R2          0x09
#define RSP_R3          0x02
#define RSP_R1          0x1a
#define RSP_R1B         0x1b
#define CMD_DATA        0x20
/* TRANSFER_MODE */
#define XFER_BLKCNT     0x02
#define XFER_AUTO_CMD12 0x04
#define XFER_READ       0x10
#define XFER_MULTI      0x20
#define XFER_DMA        0x01
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_ADMA_ADDRESS      0x58
#define SDHCI_ADMA_ADDRESS_HI   0x5c
#define CTRL_DMA_MASK   0x18
#define CTRL_ADMA64     0x18
#define ADMA_CHUNK      (32u << 10)     /* bytes per descriptor (16-bit length field, kept below 64 KB) */
#define ADMA_DESC       12              /* 64-bit ADMA2 descriptor without host v4 / CQE (sdhci.h) */

#define MMC_SECTOR      512
#define MMC_MAX_BLOCKS  1024            /* per command: 512 KB */
#define MMC_RCA         1
#define MMC_R1_ERRORS   0xfdffa080u /* card errors, including SWITCH_ERROR */

struct mmc_slice {
	uint64_t        first;          /* sectors */
	uint64_t        count;
	bool            writable;
	int             bmajor_minor;
};

static vm_offset_t      hc, gcc, cqhci;
static bool             mmc_ready, mmc_writes, mmc_hs400;
static uint32_t         mmc_source_hz = 200000000;
static uint32_t         mmc_ocr, mmc_cid[4], mmc_last_error;
static uint64_t         mmc_sectors;    /* physical user area, EXT_CSD SEC_COUNT */
static struct mmc_slice mmc_whole, mmc_userdata, mmc_data, mmc_super;
static int              mmc_bmajor = -1, mmc_cmajor = -1;
static LCK_GRP_DECLARE(mmc_lck_grp, "hakios-mmc");
static LCK_MTX_DECLARE(mmc_lock, &mmc_lck_grp);
/* ADMA2 reads (boot-arg mmc_dma=1): a 512 KB bounce buffer and its descriptor table, both in one
 * physical piece; enabled only after a PIO/DMA comparison at start. */
static bool             mmc_dma;
static vm_offset_t      mmc_dma_buf, mmc_dma_desc;
static uint64_t         mmc_dma_buf_pa, mmc_dma_desc_pa, mmc_dma_reads;

static bool mmc_dma_write, mmc_sleep;
static uint32_t mmc_cache_ctrl, mmc_cache_kib;
static uint64_t mmc_dma_writes, mmc_write_errors, mmc_flushes, mmc_flush_errors, mmc_sleeps;
SYSCTL_DECL(_kern_hakios);
SYSCTL_UINT(_kern_hakios, OID_AUTO, mmc_cache_ctrl, CTLFLAG_RD | CTLFLAG_LOCKED, &mmc_cache_ctrl, 0, "EXT_CSD CACHE_CTRL");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mmc_cache_kib, CTLFLAG_RD | CTLFLAG_LOCKED, &mmc_cache_kib, 0, "EXT_CSD CACHE_SIZE KiB");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, mmc_dma_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &mmc_dma_writes, "completed ADMA2 writes");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, mmc_write_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &mmc_write_errors, "failed writes; never replayed automatically");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, mmc_flushes, CTLFLAG_RD | CTLFLAG_LOCKED, &mmc_flushes, "successful media barriers");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, mmc_flush_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &mmc_flush_errors, "failed media barriers");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, mmc_sleeps, CTLFLAG_RD | CTLFLAG_LOCKED, &mmc_sleeps, "runtime wait sleeps");

static inline uint32_t
rd(uint32_t reg)
{
	return *(volatile uint32_t *)(hc + reg);
}

static inline void
wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(hc + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
deadline_ms(uint32_t ms)
{
	uint64_t d;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &d);
	return d;
}

/* Wait for one of the interrupt-status bits in mask, and acknowledge it. False on an error or a timeout. */
static bool
mmc_wait(uint32_t mask, uint32_t ms)
{
	uint64_t end = deadline_ms(ms), spin = deadline_ms(1);

	for (;;) {
		uint32_t status = rd(SDHCI_INT_STATUS);

		if (status & INT_ERROR) {
			mmc_last_error = status;
			return false;
		}
		if (status & mask) {
			wr(SDHCI_INT_STATUS, status & mask);
			return true;
		}
		if (mmc_sleep && mach_absolute_time() > spin) {
			IOSleep(1);
			mmc_sleeps++;
		}
		if (mach_absolute_time() > end) {
			mmc_last_error = 0xdead0000u | (status & 0xffff);
			return false;
		}
	}
}

static void
mmc_reset_lines(void)
{
	wr(SDHCI_CLOCK_CONTROL, rd(SDHCI_CLOCK_CONTROL) | (6u << 24));  /* SOFTWARE_RESET: CMD and DAT lines */
	uint64_t end = deadline_ms(100);
	while ((rd(SDHCI_CLOCK_CONTROL) >> 24) & 6) {
		if (mach_absolute_time() > end) {
			break;
		}
	}
	wr(SDHCI_INT_STATUS, 0xffffffffu);
}

static bool
mmc_command(uint32_t index, uint32_t flags, uint32_t arg, uint32_t mode)
{
	uint32_t inhibit = (flags & CMD_DATA) || (flags & 3) == 3 ? PRESENT_CMD_INHIBIT | PRESENT_DAT_INHIBIT : PRESENT_CMD_INHIBIT;
	uint64_t end = deadline_ms(100);

	while (rd(SDHCI_PRESENT_STATE) & inhibit) {
		if (mach_absolute_time() > end) {
			mmc_last_error = 0xb5590000u | index;
			return false;
		}
	}
	wr(SDHCI_INT_STATUS, 0xffffffffu);
	wr(SDHCI_ARGUMENT, arg);
	wr(SDHCI_TRANSFER_MODE, ((index << 8 | flags) << 16) | mode);
	if (!mmc_wait(INT_CMD_DONE, 100)) {
		mmc_last_error |= index << 24;
		mmc_reset_lines();
		return false;
	}
	if ((flags & 0x18) == 0x18 && (rd(SDHCI_RESPONSE) & MMC_R1_ERRORS)) {
		mmc_last_error = rd(SDHCI_RESPONSE);
		mmc_reset_lines();
		return false;
	}
	if ((flags & 3) == 3) {         /* R1b: the card holds DAT0 low while it is busy */
		end = deadline_ms(5000);
		while (!(rd(SDHCI_PRESENT_STATE) & PRESENT_DAT0)) {
			if (mach_absolute_time() > end) {
				mmc_last_error = 0xb0590000u | index;
				return false;
			}
			if (mmc_sleep) IOSleep(1);
		}
	}
	return true;
}

/* SDHCI divider: f = source / (2 * div); zero bypasses the divider. */
static bool
mmc_clock(uint32_t div)
{
	uint32_t c = rd(SDHCI_CLOCK_CONTROL) & 0xff000000u;

	wr(SDHCI_CLOCK_CONTROL, c | (0xeu << 16) | ((div & 0xff) << 8) | (((div >> 8) & 3) << 6) | 1);
	uint64_t end = deadline_ms(100);
	while (!(rd(SDHCI_CLOCK_CONTROL) & 2)) {
		if (mach_absolute_time() > end) {
			mmc_last_error = 0xc10c0000u;
			return false;
		}
	}
	wr(SDHCI_CLOCK_CONTROL, rd(SDHCI_CLOCK_CONTROL) | 4);
	delay(1000);
	return true;
}

/* Bus power on at 1.8 V, acknowledging the msm power request as Linux's pwr_irq handler does. */
static void
mmc_power(void)
{
	uint32_t h = rd(SDHCI_HOST_CONTROL) & ~0xff00u;

	wr(SDHCI_HOST_CONTROL, h | (0x0au << 8));
	wr(SDHCI_HOST_CONTROL, h | (0x0bu << 8));
	for (int i = 0; i < 10; i++) {
		uint32_t req = rd(MSM_PWRCTL_STATUS) & 0xf;

		if (req) {
			wr(MSM_PWRCTL_CLEAR, req);
			wr(MSM_PWRCTL_CTL, ((req & 3) ? 1 : 0) | ((req & 0xc) ? 4 : 0));     /* bus / io success */
		}
		delay(1000);
	}
}

/* CMD6 SWITCH, write byte: EXT_CSD[index] = value, then wait for the card and check SWITCH_ERROR (CMD13). */
static bool
mmc_switch(uint32_t index, uint32_t value)
{
	if (!mmc_command(6, RSP_R1B, (3u << 24) | (index << 16) | (value << 8), 0) ||
	    !mmc_command(13, RSP_R1, MMC_RCA << 16, 0)) {
		return false;
	}
	if (rd(SDHCI_RESPONSE) & (1u << 7)) {
		mmc_last_error = 0x5e000000u | index;
		return false;
	}
	return true;
}

/* CMD13 confirms programming completion and catches card errors, not just host status.
 * The lock remains held across sleeps so no other command can consume our status. */
static int
mmc_programmed(void)
{
	uint64_t end = deadline_ms(5000);
	do {
		if (!mmc_command(13, RSP_R1, MMC_RCA << 16, 0)) return EIO;
		uint32_t status = rd(SDHCI_RESPONSE);
		if (status & MMC_R1_ERRORS) {
			mmc_last_error = status;
			return EIO;
		}
		if ((status & (1u << 8)) && ((status >> 9) & 15) == 4) return 0;
		if (mmc_sleep) IOSleep(1); else delay(50);
	} while (mach_absolute_time() < end);
	mmc_last_error = 0xb059000du;
	return EIO;
}

static int
mmc_flush(void)
{
	int error = mmc_programmed();
	/* CPU cache maintenance is unrelated to this internal eMMC cache. Never enable
	 * CACHE_CTRL here: preserve the boot firmware's policy. */
	if (!error && mmc_cache_kib && (mmc_cache_ctrl & 1)) {
		if (!mmc_switch(32, 1)) error = EIO; /* EXT_CSD FLUSH_CACHE */
		if (!error) error = mmc_programmed();
	}
	if (error) { mmc_flush_errors++; mmc_writes = false; } else mmc_flushes++;
	return error;
}

/* CMD8 reads EXT_CSD in transfer state; it does not modify the card. */
static bool
mmc_ext_csd(uint8_t *ext)
{
	wr(SDHCI_BLOCK_SIZE, (1u << 16) | MMC_SECTOR);
	if (!mmc_command(8, RSP_R1 | CMD_DATA, 0, XFER_READ) ||
	    !mmc_wait(INT_READ_READY, 1000)) {
		mmc_reset_lines();
		return false;
	}
	for (unsigned i = 0; i < MMC_SECTOR; i += 4) {
		uint32_t value = rd(SDHCI_BUFFER);
		memcpy(ext + i, &value, sizeof(value));
	}
	return mmc_wait(INT_XFER_DONE, 1000);
}

/* khaje SDCC v5.1 (MCI 1.0x72), HS400 enhanced strobe, PIO.
 * Register sequence follows Qualcomm sdhci-msm and the JM21 vendor DTS HSR
 * values. No GCC/PLL or voltage writes: only accept the inherited 384 MHz root.
 */
static bool
mmc_enter_hs400(void)
{
	uint8_t ext[MMC_SECTOR];
	if (mmc_source_hz != 384000000 || rd(0x318) != 0x10000072 ||
	    !mmc_ext_csd(ext) || !(ext[196] & 0x40) || !(ext[184] & 1)) {
		printf("mmc: HS400 ES prerequisites absent\n");
		return false;
	}
	/* Switch width first. Status must wait until host DDR signaling is set. */
	if (!mmc_command(6, RSP_R1B, (3u << 24) | (183u << 16) | (134u << 8), 0)) {
		return false;
	}
	volatile uint16_t *host2 = (volatile uint16_t *)(hc + 0x3e);
	*host2 = (*host2 & ~7u) | 4u;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!mmc_command(13, RSP_R1, MMC_RCA << 16, 0) ||
	    (rd(SDHCI_RESPONSE) & (1u << 7)) ||
	    !mmc_command(6, RSP_R1B, (3u << 24) | (185u << 16) | (3u << 8), 0)) {
		return false;
	}
	wr(SDHCI_CLOCK_CONTROL, rd(SDHCI_CLOCK_CONTROL) & ~4u);
	/* HS400 MCLK/2 and explicit HS400 input selection; no power saving. */
	wr(0x20c, (rd(0x20c) & ~((3u << 8) | (7u << 19) | 2u)) |
	    (3u << 8) | (6u << 19) | (1u << 18));
	*host2 = (*host2 & ~7u) | 3u;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!mmc_clock(0)) {
		return false;
	}
	/* 14lpp reset + 7nm/Tassadar DLL initialization at 192 MHz. */
	wr(0x250, rd(0x250) & ~8u);
	wr(0x200, rd(0x200) & ~(1u << 18));
	wr(0x254, rd(0x254) | (1u << 21));
	wr(0x200, rd(0x200) | (1u << 30));
	wr(0x200, rd(0x200) | (1u << 29));
	wr(0x258, 0x10);
	wr(0x388, 0x2c010800);
	wr(0x25c, 0x80040868);
	uint32_t cycles = rd(0x254) & (1u << 18) ? 80 : 40;
	wr(0x254, (rd(0x254) & ~(0xffu << 10)) | (cycles << 10));
	delay(5);
	wr(0x200, rd(0x200) | 0x642c);
	delay(55);
	wr(0x200, rd(0x200) & ~(1u << 30));
	wr(0x200, rd(0x200) & ~(1u << 29));
	wr(0x200, rd(0x200) | (1u << 30));
	wr(0x200, rd(0x200) & ~(1u << 30));
	wr(0x254, rd(0x254) & ~(1u << 21));
	wr(0x200, rd(0x200) | (1u << 16));
	delay(45);
	wr(0x200, rd(0x200) | (1u << 18));
	for (unsigned i = 0; !(rd(0x208) & (1u << 7)); i++) {
		if (i == 1000) {
			printf("mmc: HS400 DLL lock failed: %x\n", rd(0x208));
			return false;
		}
		delay(1);
	}
	wr(0x224, rd(0x224) | 2u); /* Enhanced strobe for command input. */
	wr(0x254, rd(0x254) | 1u); /* DDR calibration enable. */
	for (unsigned i = 0; !(rd(0x208) & (1u << 11)); i++) {
		if (i == 1000) {
			printf("mmc: HS400 DDR lock failed: %x\n", rd(0x208));
			return false;
		}
		delay(1);
	}
	/* Ungating write-clock FIFO pulse, as required for ES on this IP. */
	wr(0x258, rd(0x258) | 2u);
	delay(2);
	wr(0x258, rd(0x258) & ~2u);
	if (!mmc_command(13, RSP_R1, MMC_RCA << 16, 0) ||
	    (rd(SDHCI_RESPONSE) & (1u << 7)) || !mmc_ext_csd(ext) ||
	    ext[183] != 134 || ext[185] != 3) {
		printf("mmc: HS400 status/EXT_CSD verification failed: %x\n", mmc_last_error);
		return false;
	}
	printf("mmc: HS400 ES negotiated, DLL status=0x%x, 192 MHz\n", rd(0x208));
	return true;
}

static void
mmc_leave_hs400(void)
{
	wr(SDHCI_CLOCK_CONTROL, rd(SDHCI_CLOCK_CONTROL) & ~4u);
	volatile uint16_t *host2 = (volatile uint16_t *)(hc + 0x3e);
	*host2 &= ~7u;
	wr(0x20c, (rd(0x20c) & ~((3u << 8) | (7u << 19) | (1u << 18))) | (2u << 8));
	wr(0x224, rd(0x224) & ~2u);
	wr(0x254, rd(0x254) & ~1u);
	wr(0x200, (rd(0x200) & ~((1u << 16) | (1u << 18))) | (3u << 29));
	wr(SDHCI_HOST_CONTROL, rd(SDHCI_HOST_CONTROL) & ~0x26u);
	mmc_reset_lines();
}

static bool
mmc_init_card(bool fast)
{
	uint32_t apps = *(volatile uint32_t *)(gcc + 4), ahb = *(volatile uint32_t *)(gcc + 8);

	if ((apps >> 31) || (ahb >> 31)) {
		printf("mmc: SDCC1 clocks are off (apps 0x%x ahb 0x%x): not touching the controller\n", apps, ahb);
		return false;
	}
	if (*(volatile uint32_t *)(cqhci + 8) & 1) {
		printf("mmc: command queueing is on: not touching the controller\n");
		return false;
	}
	wr(SDHCI_INT_ENABLE, 0xffff00ffu);      /* latch everything ... */
	wr(SDHCI_SIGNAL_ENABLE, 0);             /* ... and interrupt nothing: this driver polls */
	mmc_power();
	if (!mmc_clock((mmc_source_hz + 799999) / 800000)) {                  /* 400 kHz for identification */
		return false;
	}
	delay(2000);                            /* at least 74 clocks before CMD0 */
	mmc_command(0, RSP_NONE, 0, 0);
	delay(2000);
	uint64_t end = deadline_ms(1000);
	do {
		if (!mmc_command(1, RSP_R3, 0x40ff8080u, 0)) {
			return false;
		}
		mmc_ocr = rd(SDHCI_RESPONSE);
		if (mach_absolute_time() > end) {
			mmc_last_error = 0x0c100000u;
			return false;
		}
		delay(5000);
	} while (!(mmc_ocr >> 31));
	if (!(mmc_ocr & (1u << 30))) {
		printf("mmc: the card is not sector addressed (OCR 0x%x)\n", mmc_ocr);
		return false;
	}
	if (!mmc_command(2, RSP_R2, 0, 0)) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		mmc_cid[i] = rd(SDHCI_RESPONSE + 4 * i);
	}
	if (!mmc_command(3, RSP_R1, MMC_RCA << 16, 0) || !mmc_command(7, RSP_R1B, MMC_RCA << 16, 0)) {
		return false;
	}
	if (!mmc_clock((mmc_source_hz + 49999999) / 50000000) || !mmc_command(16, RSP_R1, MMC_SECTOR, 0)) {      /* 25 MHz, 1 bit */
		return false;
	}
	uint8_t ext[MMC_SECTOR];
	if (!mmc_ext_csd(ext)) {
		return false;
	}
	memcpy(&mmc_cache_kib, ext + 249, sizeof(mmc_cache_kib));
	mmc_cache_ctrl = ext[33];
	uint32_t sectors;
	memcpy(&sectors, ext + 212, sizeof(sectors));
	if (!sectors) return false; /* sector-addressed cards must provide SEC_COUNT */
	mmc_sectors = sectors;
	printf("mmc: physical sectors=%llu cache=%u KiB CACHE_CTRL=0x%x\n", mmc_sectors, mmc_cache_kib, mmc_cache_ctrl);
	printf("mmc: EXT_CSD rev=%u type=0x%x strobe=%u; MCI=0x%x GCC cmd/cfg/m/n/d=%x/%x/%x/%x/%x\n",
	    ext[192], ext[196], ext[184], rd(0x318),
	    *(volatile uint32_t *)(gcc + 0x28), *(volatile uint32_t *)(gcc + 0x2c),
	    *(volatile uint32_t *)(gcc + 0x30), *(volatile uint32_t *)(gcc + 0x34),
	    *(volatile uint32_t *)(gcc + 0x38));
	if (fast) {
		/* 8-bit bus (EXT_CSD BUS_WIDTH 183 = 2), then high speed (HS_TIMING 185 = 1) at 50 MHz: SDR, no tuning */
		if (!mmc_switch(183, 2)) {
			return false;
		}
		wr(SDHCI_HOST_CONTROL, (rd(SDHCI_HOST_CONTROL) & ~0x02u) | 0x20u);
		if (!mmc_switch(185, 1)) {
			return false;
		}
		wr(SDHCI_HOST_CONTROL, rd(SDHCI_HOST_CONTROL) | 0x04u);
		if (!mmc_clock((mmc_source_hz + 99999999) / 100000000)) {
			return false;
		}
	}
	return true;
}

/* Sectors [lba, lba + count) of the user area, PIO. count <= MMC_MAX_BLOCKS. */
static int
mmc_transfer(bool write, uint64_t lba, uint32_t count, uint8_t *data)
{
	uint32_t index = write ? (count > 1 ? 25 : 24) : (count > 1 ? 18 : 17);
	uint32_t mode = (write ? 0 : XFER_READ) | (count > 1 ? XFER_MULTI | XFER_BLKCNT | XFER_AUTO_CMD12 : 0);

	if (lba > UINT32_MAX || count == 0 || count > MMC_MAX_BLOCKS ||
	    lba >= mmc_sectors || count > mmc_sectors - lba) {
		return EINVAL;
	}
	wr(SDHCI_BLOCK_SIZE, (count << 16) | MMC_SECTOR);
	if (!mmc_command(index, RSP_R1 | CMD_DATA, (uint32_t)lba, mode)) {
		return EIO;
	}
	for (uint32_t b = 0; b < count; b++) {
		uint32_t *words = (uint32_t *)(void *)(data + (uint64_t)b * MMC_SECTOR);

		if (!mmc_wait(write ? INT_WRITE_READY : INT_READ_READY, 1000)) {
			mmc_reset_lines();
			return EIO;
		}
		for (int i = 0; i < MMC_SECTOR / 4; i++) {
			if (write) {
				*(volatile uint32_t *)(hc + SDHCI_BUFFER) = words[i];
			} else {
				words[i] = *(volatile uint32_t *)(hc + SDHCI_BUFFER);
			}
		}
	}
	if (!mmc_wait(INT_XFER_DONE, write ? 5000 : 1000)) {
		mmc_reset_lines();
		return EIO;
	}
	if (write) {                    /* wait until the card has programmed it (DAT0 high) */
		uint64_t end = deadline_ms(5000);
		while (!(rd(SDHCI_PRESENT_STATE) & PRESENT_DAT0)) {
			if (mach_absolute_time() > end) {
				mmc_last_error = 0xb0590019u;
				return EIO;
			}
			if (mmc_sleep) IOSleep(1);
		}
		return mmc_programmed();
	}
	return 0;
}

static struct mmc_slice *
mmc_slice_of(dev_t dev)
{
	return minor(dev) == 0 ? &mmc_whole : minor(dev) == 1 ? &mmc_userdata :
	       minor(dev) == 2 && mmc_data.count ? &mmc_data : NULL;
}

/* Explicit window in MiB relative to userdata; never discover or format it.
 * [0,12 GiB) is reserved for the system/cryptexes and existing archive slots.
 * The provisioning caller must establish ownership of the remaining region. */
static void
mmc_data_start(void)
{
	char arg[48] = {0};
	uint64_t part[2] = {0, 0};
	unsigned at = 0;
	if (!PE_parse_boot_argn("mmc_data", arg, sizeof(arg))) return;
	/* Reuse the complete Android super partition without rewriting GPT. */
	if (strcmp(arg, "super") == 0) {
		if (!mmc_writes || !mmc_super.count) goto bad;
		mmc_data = mmc_super;
		mmc_data.writable = true;
		printf("mmc: Data uses GPT super, first=%llu count=%llu sectors\n", mmc_data.first, mmc_data.count);
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		unsigned digits = 0;
		while (at < sizeof(arg) && arg[at] >= '0' && arg[at] <= '9') {
			if (++digits > 7) goto bad;
			part[i] = part[i] * 10 + (unsigned)(arg[at++] - '0');
		}
		if (!digits || at >= sizeof(arg) || (i == 0 ? arg[at++] != ':' : arg[at] != 0)) goto bad;
	}
	uint64_t first = part[0] * 2048, count = part[1] * 2048;
	if (!mmc_writes || part[0] < 12288 || part[1] < 64 ||
	    first >= mmc_userdata.count || count > mmc_userdata.count - first) goto bad;
	mmc_data.first = mmc_userdata.first + first;
	mmc_data.count = count;
	mmc_data.writable = true;
	printf("mmc: Data window [%llu,%llu) MiB of userdata; overlapping raw writes blocked\n", part[0], part[0] + part[1]);
	return;
bad:
	printf("mmc: refusing invalid/read-only mmc_data window\n");
}

static bool
mmc_overlaps_data(struct mmc_slice *s, uint64_t first, uint64_t count)
{
	if (s != &mmc_userdata || !mmc_data.count || !count) return false;
	first += s->first;
	return first < mmc_data.first + mmc_data.count &&
	    mmc_data.first < first + count;
}

bool
hakios_mmc_dev(dev_t dev)
{
	return mmc_bmajor >= 0 && (major(dev) == mmc_bmajor || major(dev) == mmc_cmajor);
}

static int
mmc_dev_open(dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	struct mmc_slice *s = mmc_slice_of(dev);

	return s && s->count ? 0 : ENXIO;
}

/* Sectors [lba, lba + count) by ADMA2 into mmc_dma_buf. Caller holds mmc_lock. */
static int
mmc_transfer_dma(bool write, uint64_t lba, uint32_t count)
{
	uint32_t bytes = count * MMC_SECTOR, n = 0;
	uint8_t *d = (uint8_t *)mmc_dma_desc;

	if (lba > UINT32_MAX || count == 0 || count > MMC_MAX_BLOCKS ||
	    lba >= mmc_sectors || count > mmc_sectors - lba) {
		return EINVAL;
	}
	for (uint32_t off = 0; off < bytes; off += ADMA_CHUNK, n++) {
		uint32_t len = MIN(ADMA_CHUNK, bytes - off);
		uint64_t pa = mmc_dma_buf_pa + off;
		uint16_t attr = 0x21 | (off + len >= bytes ? 0x02 : 0);       /* VALID | ACT_TRAN, END on the last */

		d[n * ADMA_DESC + 0] = (uint8_t)attr;
		d[n * ADMA_DESC + 1] = (uint8_t)(attr >> 8);
		d[n * ADMA_DESC + 2] = (uint8_t)len;
		d[n * ADMA_DESC + 3] = (uint8_t)(len >> 8);
		for (int i = 0; i < 8; i++) {
			d[n * ADMA_DESC + 4 + i] = (uint8_t)(pa >> (8 * i));
		}
	}
	FlushPoC_DcacheRegion(mmc_dma_desc, PAGE_SIZE);
	FlushPoC_DcacheRegion(mmc_dma_buf, bytes);      /* no dirty line may later land over what the controller writes */
	uint32_t control = rd(SDHCI_HOST_CONTROL);
	wr(SDHCI_ADMA_ADDRESS, (uint32_t)mmc_dma_desc_pa);
	wr(SDHCI_ADMA_ADDRESS_HI, (uint32_t)(mmc_dma_desc_pa >> 32));
	wr(SDHCI_HOST_CONTROL, (control & ~CTRL_DMA_MASK) | CTRL_ADMA64);
	wr(SDHCI_BLOCK_SIZE, (count << 16) | MMC_SECTOR);
	uint32_t mode = XFER_DMA | (write ? 0 : XFER_READ) | (count > 1 ? XFER_MULTI | XFER_BLKCNT | XFER_AUTO_CMD12 : 0);
	int error = 0;
	if (!mmc_command(write ? (count > 1 ? 25 : 24) : (count > 1 ? 18 : 17), RSP_R1 | CMD_DATA, (uint32_t)lba, mode) ||
	    !mmc_wait(INT_XFER_DONE, write ? 5000 : 2000)) {
		mmc_reset_lines();
		error = EIO;
	}
	wr(SDHCI_HOST_CONTROL, control);
	if (!write) FlushPoC_DcacheRegion(mmc_dma_buf, bytes); /* invalidate speculative CPU reads */
	if (write && !error) error = mmc_programmed();
	return error;
}

/* Read sectors into dest (kernel VA): ADMA2 when enabled, PIO otherwise or after a DMA failure. */
static int
mmc_read_into(uint64_t lba, uint32_t count, uint8_t *dest)
{
	if (mmc_dma) {
		int error = mmc_transfer_dma(false, lba, count);
		if (error == 0) {
			memcpy(dest, (void *)mmc_dma_buf, (size_t)count * MMC_SECTOR);
			mmc_dma_reads++;
			return 0;
		}
		mmc_dma = false;
		printf("mmc: ADMA2 read of %u sectors at %llu failed (0x%x); PIO from now on\n", count, lba, mmc_last_error);
	}
	return mmc_transfer(false, lba, count, dest);
}

/* Write errors are ambiguous: do not retry through PIO and risk replaying data. */
static int
mmc_write_from(uint64_t lba, uint32_t count, uint8_t *src)
{
	if (!mmc_writes) return EROFS;
	int error;
	if (mmc_dma && mmc_dma_write) {
		memcpy((void *)mmc_dma_buf, src, (size_t)count * MMC_SECTOR);
		error = mmc_transfer_dma(true, lba, count);
		if (!error) mmc_dma_writes++;
	} else {
		error = mmc_transfer(true, lba, count, src);
	}
	if (error) {
		mmc_write_errors++;
		mmc_writes = false; /* unknown completion: fence further writes until reboot */
	}
	return error;
}

/* mmc_dma=1: allocate, then enable ADMA2 only if it reads exactly what PIO reads, twice. */
static void
mmc_dma_start(void)
{
	uint32_t want = 0, caps = rd(SDHCI_CAPABILITIES);
	uint8_t *pio = NULL;

	if (!PE_parse_boot_argn("mmc_dma", &want, sizeof(want)) || want == 0) {
		return;
	}
	printf("mmc: capabilities 0x%08x (ADMA2 %s, 64-bit bus %s)\n", caps, (caps >> 19) & 1 ? "yes" : "no",
	    (caps >> 28) & 1 ? "yes" : "no");
	if ((caps & ((1u << 19) | (1u << 28))) != ((1u << 19) | (1u << 28)) ||
	    kmem_alloc_contig(kernel_map, &mmc_dma_buf, MMC_MAX_BLOCKS * MMC_SECTOR, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc_contig(kernel_map, &mmc_dma_desc, PAGE_SIZE, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    (pio = kalloc_data(MMC_MAX_BLOCKS * MMC_SECTOR, Z_WAITOK)) == NULL) {
		printf("mmc: ADMA2 not available; PIO\n");
		return;
	}
	mmc_dma_buf_pa = kvtophys(mmc_dma_buf);
	mmc_dma_desc_pa = kvtophys(mmc_dma_desc);
	bool same = true;
	const uint64_t at[2] = { mmc_userdata.first, mmc_userdata.first + 2097152 };       /* 0 and 1 GiB in */
	for (int t = 0; t < 2 && same; t++) {
		lck_mtx_lock(&mmc_lock);
		int e1 = mmc_transfer(false, at[t], MMC_MAX_BLOCKS, pio);
		memset((void *)mmc_dma_buf, 0xa5, MMC_MAX_BLOCKS * MMC_SECTOR);
		int e2 = e1 ? e1 : mmc_transfer_dma(false, at[t], MMC_MAX_BLOCKS);
		lck_mtx_unlock(&mmc_lock);
		same = e1 == 0 && e2 == 0 && memcmp(pio, (void *)mmc_dma_buf, MMC_MAX_BLOCKS * MMC_SECTOR) == 0;
		printf("mmc: ADMA2 self-test %d at sector %llu: pio %d dma %d (0x%x) %s\n", t, at[t], e1, e2,
		    mmc_last_error, same ? "identical" : "DIFFERENT");
	}
	kfree_data(pio, MMC_MAX_BLOCKS * MMC_SECTOR);
	mmc_dma = same;
	if (same) {
		uint64_t start = mach_absolute_time(), ns;
		int error = 0;
		lck_mtx_lock(&mmc_lock);
		for (uint32_t i = 0; i < 64 && error == 0; i++) {       /* 32 MB */
			error = mmc_transfer_dma(false, mmc_userdata.first + (uint64_t)i * MMC_MAX_BLOCKS, MMC_MAX_BLOCKS);
		}
		lck_mtx_unlock(&mmc_lock);
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		printf("mmc: ADMA2 on: read 32 MB in %llu ms (%llu KB/s), error %d\n", ns / 1000000,
		    ns ? 32ULL * 1024 * 1000000000ULL / ns : 0, error);
		mmc_dma = error == 0;
	}
}

static int
mmc_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
mmc_dev_strategy(buf_t bp)
{
	struct mmc_slice *s = mmc_slice_of(buf_device(bp));
	bool write = !(buf_flags(bp) & B_READ);
	uint64_t first = (uint64_t)buf_blkno(bp), len = buf_count(bp);
	caddr_t mapped;
	int error = 0;

	if (s == NULL || !s->count || (len % MMC_SECTOR) != 0) {
		error = EINVAL;
	} else if (write && (!s->writable || !mmc_writes)) {
		error = EROFS;
	} else if (first >= s->count && !write) {
		buf_setresid(bp, (uint32_t)len);
		buf_biodone(bp);
		return;
	} else if (first >= s->count || len / MMC_SECTOR > s->count - first) {
		error = EINVAL;
	} else if (write && mmc_overlaps_data(s, first, len / MMC_SECTOR)) {
		error = EBUSY;
	}
	if (error == 0) {
		error = buf_map(bp, &mapped);
	}
	if (error) {
		buf_seterror(bp, error);
		buf_biodone(bp);
		return;
	}
	uint64_t done = 0;
	lck_mtx_lock(&mmc_lock);
	while (done < len && error == 0) {
		uint32_t blocks = (uint32_t)MIN((len - done) / MMC_SECTOR, (uint64_t)MMC_MAX_BLOCKS);

		error = write ? mmc_write_from(s->first + first + done / MMC_SECTOR, blocks, (uint8_t *)mapped + done) :
		    mmc_read_into(s->first + first + done / MMC_SECTOR, blocks, (uint8_t *)mapped + done);
		if (error == 0) {
			done += (uint64_t)blocks * MMC_SECTOR;
		}
	}
	/* FUA completion follows a real media flush under the same controller lock. */
	if (!error && write && (buf_flags(bp) & B_FUA)) error = mmc_flush();
	lck_mtx_unlock(&mmc_lock);
	buf_unmap(bp);
	if (error) {
		printf("mmc: %s of %llu sectors at %llu: error %d (0x%x)\n", write ? "write" : "read",
		    len / MMC_SECTOR, s->first + first, error, mmc_last_error);
		buf_seterror(bp, error);
	}
	buf_setresid(bp, (uint32_t)(len - done));
	buf_biodone(bp);
}

static int
mmc_dev_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	struct mmc_slice *s = mmc_slice_of(dev);

	if (s == NULL) {
		return ENXIO;
	}
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = MMC_SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == MMC_SECTOR ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = s->count;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = s->writable && mmc_writes;
		return 0;
	case DKIOCISSOLIDSTATE:
		*(uint32_t *)data = 1;
		return 0;
	case DKIOCGETFEATURES:
		*(uint32_t *)data = 0; /* no UNMAP or native barrier support */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = MMC_MAX_BLOCKS;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		lck_mtx_lock(&mmc_lock);
		int error = mmc_flush();
		lck_mtx_unlock(&mmc_lock);
		return error;
	default:
		return ENOTTY;
	}
}

static int
mmc_dev_size(__unused dev_t dev)
{
	return MMC_SECTOR;
}

/*
 * The raw devices go through a kernel buffer and uiomove(), not physio(): physio's buffer is the caller's
 * own address, and writing it from here faults (a kernel data abort, seen on the phone) where uiomove's
 * copyout recovers. Whole sectors only.
 */
#define MMC_RAW_CHUNK   (MMC_MAX_BLOCKS * MMC_SECTOR)

static int
mmc_dev_rw(dev_t dev, struct uio *uio, bool write)
{
	struct mmc_slice *s = mmc_slice_of(dev);
	int error = 0;

	if (s == NULL || !s->count) {
		return ENXIO;
	}
	if (write && (!s->writable || !mmc_writes)) {
		return EROFS;
	}
	if (uio_offset(uio) < 0 || (uio_offset(uio) % MMC_SECTOR) != 0 || (uio_resid(uio) % MMC_SECTOR) != 0) {
		return EINVAL;
	}
	if (write && ((uint64_t)uio_offset(uio) / MMC_SECTOR >= s->count ||
	    (uint64_t)uio_resid(uio) / MMC_SECTOR > s->count - (uint64_t)uio_offset(uio) / MMC_SECTOR)) return ENOSPC;
	if (write && mmc_overlaps_data(s, (uint64_t)uio_offset(uio) / MMC_SECTOR,
	    (uint64_t)uio_resid(uio) / MMC_SECTOR)) return EBUSY;
	uint8_t *bounce = kalloc_data(MMC_RAW_CHUNK, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (uio_resid(uio) > 0 && error == 0) {
		uint64_t sector = (uint64_t)uio_offset(uio) / MMC_SECTOR;
		uint64_t len = MIN((uint64_t)uio_resid(uio), (uint64_t)MMC_RAW_CHUNK);

		if (sector >= s->count) {
			break;          /* the end of the device */
		}
		len = MIN(len, (s->count - sector) * MMC_SECTOR);
		if (write) {
			/* Advance the caller only after the card accepts the chunk. */
			uio_t copy = uio_duplicate(uio);
			if (!copy) { error = ENOMEM; break; }
			error = uiomove((caddr_t)bounce, (int)len, copy);
			uio_free(copy);
			if (error) break;
		}
		lck_mtx_lock(&mmc_lock);
		error = write ? mmc_write_from(s->first + sector, (uint32_t)(len / MMC_SECTOR), bounce) :
		    mmc_read_into(s->first + sector, (uint32_t)(len / MMC_SECTOR), bounce);
		lck_mtx_unlock(&mmc_lock);
		if (error) {
			printf("mmc: raw %s of %llu sectors at %llu: error %d (0x%x)\n", write ? "write" : "read",
			    len / MMC_SECTOR, s->first + sector, error, mmc_last_error);
			break;
		}
		if (write) uio_update(uio, (user_size_t)len);
		if (!write) {
			error = uiomove((caddr_t)bounce, (int)len, uio);
		}
	}
	kfree_data(bounce, MMC_RAW_CHUNK);
	return error;
}

static int
mmc_dev_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return mmc_dev_rw(dev, uio, false);
}

static int
mmc_dev_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return mmc_dev_rw(dev, uio, true);
}

static const struct bdevsw mmc_bdevsw = {
	.d_open     = mmc_dev_open,
	.d_close    = mmc_dev_close,
	.d_strategy = mmc_dev_strategy,
	.d_ioctl    = mmc_dev_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = mmc_dev_size,
	.d_type     = D_DISK,
};

static const struct cdevsw mmc_cdevsw = {
	.d_open       = mmc_dev_open,
	.d_close      = mmc_dev_close,
	.d_read       = mmc_dev_read,
	.d_write      = mmc_dev_write,
	.d_ioctl      = mmc_dev_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static bool
utf16_is(const uint8_t *name, const char *ascii)
{
	for (int i = 0; i < 36; i++) {
		char c = ascii[i];

		if (name[2 * i] != (uint8_t)c || name[2 * i + 1] != 0) {
			return false;
		}
		if (c == 0) {
			return true;
		}
	}
	return false;
}

static uint32_t
mmc_crc32(const uint8_t *p, uint32_t n)
{
	uint32_t crc = ~0u;
	while (n--) {
		crc ^= *p++;
		for (unsigned i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

/* The primary GPT: the user area's size, and where "userdata" is. */
static bool
mmc_read_gpt(void)
{
	uint8_t *buf = kalloc_data(MMC_SECTOR * 33, Z_WAITOK | Z_ZERO);
	bool found = false;
	memset(&mmc_super, 0, sizeof(mmc_super));

	if (buf == NULL || mmc_transfer(false, 1, 33, buf) != 0 || memcmp(buf, "EFI PART", 8) != 0) {
		printf("mmc: no GPT at LBA 1 (0x%x)\n", mmc_last_error);
		goto out;
	}
	uint64_t last = *(uint64_t *)(void *)(buf + 48), entries_lba = *(uint64_t *)(void *)(buf + 72);
	uint32_t entries = *(uint32_t *)(void *)(buf + 80), size = *(uint32_t *)(void *)(buf + 84);

	if (entries_lba != 2 || size != 128 || entries > 128) {
		printf("mmc: unexpected GPT layout (entries at %llu, %u of %u bytes)\n", entries_lba, entries, size);
		goto out;
	}
	uint32_t hsize, hcrc, table_crc;
	uint64_t current, backup, first;
	memcpy(&hsize, buf + 12, 4);
	memcpy(&hcrc, buf + 16, 4);
	memcpy(&table_crc, buf + 88, 4);
	memcpy(&current, buf + 24, 8);
	memcpy(&backup, buf + 32, 8);
	memcpy(&first, buf + 40, 8);
	if (!entries || hsize < 92 || hsize > MMC_SECTOR || current != 1 ||
	    backup != mmc_sectors - 1 || first < 34 || first > last || last >= backup) goto out;
	memset(buf + 16, 0, 4);
	if (mmc_crc32(buf, hsize) != hcrc || mmc_crc32(buf + MMC_SECTOR, entries * size) != table_crc) {
		printf("mmc: GPT CRC mismatch; refusing publication\n");
		goto out;
	}
	for (uint32_t i = 0; i < entries; i++) {
		const uint8_t *e = buf + MMC_SECTOR + i * size;
		uint64_t start = *(const uint64_t *)(const void *)(e + 32), end = *(const uint64_t *)(const void *)(e + 40);

		if (utf16_is(e + 56, "super") && start >= first && start <= end && end <= last) {
			mmc_super.first = start;
			mmc_super.count = end - start + 1;
		}
		if (utf16_is(e + 56, "userdata") && start >= first && start <= end && end <= last) {
			mmc_userdata.first = start;
			mmc_userdata.count = end - start + 1;
			found = true;
		}
	}
	if (found && mmc_super.count &&
	    mmc_super.first < mmc_userdata.first + mmc_userdata.count &&
	    mmc_userdata.first < mmc_super.first + mmc_super.count) {
		printf("mmc: overlapping super/userdata; refusing publication\n");
		found = false;
	}
	if (!found) {
		printf("mmc: no valid partition named userdata\n");
	}
out:
	if (buf) {
		kfree_data(buf, MMC_SECTOR * 33);
	}
	return found;
}

extern void HakiosMMCReportCapacity(unsigned long long bytes);

void
hakios_mmc_init(void)
{
	uint32_t mode = 0;

	if (!PE_parse_boot_argn("mmc", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	hc = ml_io_map(SDHC_PHYS, PAGE_SIZE);
	cqhci = ml_io_map(CQHCI_PHYS, PAGE_SIZE);
	gcc = ml_io_map(GCC_SDCC1_PHYS & ~(vm_offset_t)PAGE_MASK, PAGE_SIZE) + (GCC_SDCC1_PHYS & PAGE_MASK);
	vm_offset_t pll = ml_io_map(0x1404000, PAGE_SIZE) + 0x2000;
	uint32_t pll_mode = *(volatile uint32_t *)(pll), pll_l = *(volatile uint32_t *)(pll + 4);
	uint32_t pll_user = *(volatile uint32_t *)(pll + 0xc), pll_alpha = *(volatile uint32_t *)(pll + 0x40);
	printf("mmc: GPLL6 mode/L/user/alpha=%x/%x/%x/%x\n", pll_mode, pll_l, pll_user, pll_alpha);
	if ((pll_mode & (1u << 31)) && pll_l == 40 && pll_alpha == 0 &&
	    ((pll_user >> 8) & 0xf) == 1 &&
	    (*(volatile uint32_t *)(gcc + 0x2c) & 0x371f) == 0x401) {
		mmc_source_hz = 384000000;
	}
	uint32_t hs400 = 0;
	PE_parse_boot_argn("mmc_hs400", &hs400, sizeof(hs400));
	uint32_t fast = 1;
	PE_parse_boot_argn("mmc_fast", &fast, sizeof(fast));
	lck_mtx_lock(&mmc_lock);
	mmc_ready = mmc_init_card(fast != 0) && mmc_read_gpt();
	if (mmc_ready && fast && hs400) {
		uint8_t before[MMC_SECTOR], after[MMC_SECTOR];
		bool baseline = mmc_transfer(false, 1, 1, before) == 0;
		mmc_hs400 = baseline && mmc_enter_hs400() &&
		    mmc_transfer(false, 1, 1, after) == 0 &&
		    memcmp(before, after, sizeof(before)) == 0 && mmc_read_gpt();
		if (!mmc_hs400) {
			printf("mmc: HS400 failed (0x%x), restoring high-speed SDR\n", mmc_last_error);
			mmc_leave_hs400();
			mmc_ready = mmc_init_card(true) && mmc_read_gpt();
		}
	}
	if (!mmc_ready && fast) {
		/* CMD0 takes the card back to 1 bit at default speed */
		printf("mmc: 8-bit high speed failed (0x%x); again at 1 bit, 25 MHz\n", mmc_last_error);
		wr(SDHCI_HOST_CONTROL, rd(SDHCI_HOST_CONTROL) & ~0x26u);
		fast = 0;
		mmc_ready = mmc_init_card(false) && mmc_read_gpt();
	}
	lck_mtx_unlock(&mmc_lock);
	if (!mmc_ready) {
		printf("mmc: eMMC not available (error 0x%x)\n", mmc_last_error);
		return;
	}
	mmc_writes = mode >= 2;
	mmc_whole.count = mmc_sectors;
	mmc_userdata.writable = true;
	mmc_data_start();

	mmc_bmajor = bdevsw_add(-1, &mmc_bdevsw);
	mmc_cmajor = mmc_bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &mmc_cdevsw, mmc_bmajor);
	if (mmc_bmajor < 0 || mmc_cmajor < 0 ||
	    devfs_make_node(makedev(mmc_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rmmc0") == NULL ||
	    devfs_make_node(makedev(mmc_bmajor, 1), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "mmcroot") == NULL ||
	    devfs_make_node(makedev(mmc_cmajor, 1), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rmmcroot") == NULL) {
		printf("mmc: cannot make the device nodes\n");
		return;
	}
	if (mmc_data.count &&
	    (devfs_make_node(makedev(mmc_bmajor, 2), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0600, "mmcdata") == NULL ||
	     devfs_make_node(makedev(mmc_cmajor, 2), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0600, "rmmcdata") == NULL)) {
		printf("mmc: cannot make Data device nodes; window remains protected\n");
	}
	printf("mmc: eMMC OCR 0x%x CID %08x%08x%08x%08x, %llu MB; userdata at sector %llu, %llu MB, %s, %s\n",
	    mmc_ocr, mmc_cid[3], mmc_cid[2], mmc_cid[1], mmc_cid[0], mmc_sectors / 2048, mmc_userdata.first,
	    mmc_userdata.count / 2048, mmc_writes ? "read-write" : "read only", mmc_hs400 ? "8 bit HS400 ES 192 MHz" : fast ? (mmc_source_hz == 384000000 ? "8 bit SDR 48 MHz" : "8 bit SDR assumed 50 MHz") : "1 bit default speed");

	/* MobileGestalt queries disk0 Size; this publishes metadata only. */
	HakiosMMCReportCapacity((unsigned long long)mmc_sectors * MMC_SECTOR);
	mmc_dma_start();
	uint32_t dma_write = 0, sleep = 0;
	PE_parse_boot_argn("mmc_dma_write", &dma_write, sizeof(dma_write));
	PE_parse_boot_argn("mmc_sleep", &sleep, sizeof(sleep));
	mmc_dma_write = dma_write != 0;
	mmc_sleep = sleep != 0;
	printf("mmc: runtime DMA writes=%u sleeping waits=%u\n", mmc_dma && mmc_dma_write, mmc_sleep);
	uint32_t bench_mb = 0;
	if (PE_parse_boot_argn("mmc_bench", &bench_mb, sizeof(bench_mb)) && bench_mb > 0) {
		uint8_t *buf = kalloc_data(MMC_MAX_BLOCKS * MMC_SECTOR, Z_WAITOK);
		uint64_t start = mach_absolute_time(), ns;
		int error = 0;

		for (uint32_t i = 0; buf && i < bench_mb * 2 && error == 0; i++) {     /* 512 KB per command */
			lck_mtx_lock(&mmc_lock);
			error = mmc_transfer(false, mmc_userdata.first + (uint64_t)i * MMC_MAX_BLOCKS, MMC_MAX_BLOCKS, buf);
			lck_mtx_unlock(&mmc_lock);
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		printf("mmc: read %u MB in %llu ms (%llu KB/s), error %d\n", bench_mb, ns / 1000000,
		    ns ? (uint64_t)bench_mb * 1024 * 1000000000ULL / ns : 0, error);
		if (buf) {
			kfree_data(buf, MMC_MAX_BLOCKS * MMC_SECTOR);
		}
	}
}
