/*
 * FiiO JM21 (Qualcomm khaje / SM6225) Wi-Fi, phase 1: the WLAN firmware brought up.
 *
 * The radio is a WCN3990-family chip ("ICNSS", dt qcom,icnss@C800000) whose firmware runs as a
 * protection domain of the modem (MSS): wlanmdsp.mbn is fetched by the modem itself over TFTP.
 * So this driver does what Linux's remoteproc-mss + rmtfs + pd-mapper + tqftpserv + icnss2 +
 * wlfwd did together on postmarketOS (see ~/.claude/skills/jm21-fiio/references/wifi.md):
 *
 *   1  the WCN3990 supplies voted on through RPM (L8A, L16A, L17A, L23A; hakios.jm21_a610.c owns the FIFO)
 *   2  modem.mdt + modem.bNN from fw/ of the RAM disk, TrustZone PAS 4, relocated to 0x4ab00000
 *   3  SMP2P item 428 (ours) in the SMEM partition 0<->1, doorbells APCS IPC bit 14 (SMP2P), bit 12 (GLINK)
 *   4  RMTFS buffer (CDSP reserved region, hyp-assigned to MSS+NAV), modemst1/modemst2/fsg/fsc read from
 *      /dev/rmmc0 (served from memory; nothing written to the eMMC)
 *   5  GLINK over SMEM (items 478/479/480), channel IPCRTR, QRTR with our name-service part
 *   6  servers: RMTFS (14), TQFTP (4096), service-registry locator (64)
 *   7  WLFW client (service 0x45): ind_register, MSA info + hyp assign + MSA ready, CAP, BDF, CAL_REPORT, MAC,
 *      then waits for FW_READY
 *
 * The SMEM / SMP2P / GLINK / QRTR / RMTFS / TFTP / servreg code is condensed from drivers/modem/hakios.mpss.c
 * (the POCO X5 Pro's modem), with khaje's numbers from the vendor DT.
 *
 * Opt-in: boot-args jm21_ios=1 jm21_wifi=1 (JM21_WIFI=1 in fiio-xnu-poc-display). jm21_wifi_mac=XX:..:XX.
 * sysctl kern.jm21_wifi_phase / kern.jm21_wifi_state / kern.jm21_wifi_services.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>
#include <vm/vm_kern.h>
#include <sys/hakios.qrtr.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.sta.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void IOSleep(unsigned int milliseconds);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern bool hakios_jm21_rpm_request(uint32_t type, uint32_t id, const uint32_t *kvp, uint32_t words);

#define TAG "jm21-wifi: "

static LCK_GRP_DECLARE(jw_grp, "jm21-wifi");

static int jw_phase;
static char jw_state[160] = "off";
static char jw_services[256] = "";
SYSCTL_INT(_kern, OID_AUTO, jm21_wifi_phase, CTLFLAG_RD | CTLFLAG_LOCKED, &jw_phase, 0, "JM21 Wi-Fi bring-up phase (negative: failed there)");
SYSCTL_STRING(_kern, OID_AUTO, jm21_wifi_state, CTLFLAG_RD | CTLFLAG_LOCKED, jw_state, sizeof(jw_state), "JM21 Wi-Fi state");
SYSCTL_STRING(_kern, OID_AUTO, jm21_wifi_services, CTLFLAG_RD | CTLFLAG_LOCKED, jw_services, sizeof(jw_services), "QRTR services seen");

static void
state(int phase, const char *fmt, ...) __printflike(2, 3);
static void
state(int phase, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(jw_state, sizeof(jw_state), fmt, ap);
	va_end(ap);
	jw_phase = phase;
	printf(TAG "[%d] %s\n", phase, jw_state);
}

static inline uint32_t rd(vm_offset_t a) { return *(volatile uint32_t *)a; }
static inline void wr(vm_offset_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile ("dsb sy" ::: "memory"); }
static inline void settle(void) { __asm__ volatile ("dsb sy\n isb" ::: "memory"); }
static void sleep_ms(uint32_t ms) { IOSleep(ms); }
static uint64_t
now_us(void)
{
	uint64_t abs = mach_absolute_time(), ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000;
}

/* ---- khaje numbers (vendor DT) --------------------------------------------------------------------- */
#define SMEM_PHYS               0x46000000u
#define SMEM_SIZE               0x00200000u
#define TCSR_MUTEX_LOCK3        0x00343000u     /* tcsr_mutex syscon 0x340000, stride 0x1000, smem lock 3 */
#define APCS_IPC                0x0f111008u     /* apcs_glb + 8 (qcom,bengal-apcs-hmss-global) */
#define DOORBELL_GLINK          (1u << 12)      /* mboxes <apcs 12> of glink-edge modem */
#define DOORBELL_SMP2P          (1u << 14)      /* mboxes <apcs 14> of qcom,smp2p-modem */
#define MSS_REGION              0x4ab00000u     /* reserved-memory modem_region */
#define MSS_REGION_SIZE         0x06900000u
#define PAS_MSS                 4
#define MSA_REGION              0x51900000u     /* reserved-memory wlan_msa_region */
#define MSA_REGION_SIZE         0x00100000u
#define RMTFS_BUFFER_PA         0x51a00000u     /* first 2 MB of reserved cdsp_regions (CDSP never started here) */
#define RMTFS_BUFFER_SIZE       0x00200000u     /* dt: qcom,rmtfs_sharedmem@0 reg size */
#define SMEM_US                 0
#define SMEM_MPSS               1
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5
#define ITEM_SMP2P_OURS         428
#define ITEM_SMP2P_THEIRS       435
#define ITEM_CRASH_REASON       421
#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480

/* ---- SMEM ------------------------------------------------------------------------------------------ */
struct smem_table_entry { uint32_t offset, size, flags; uint16_t host0, host1; uint32_t cacheline, reserved[7]; };
struct smem_table { uint8_t magic[4]; uint32_t version, count, reserved[5]; };
struct smem_partition { uint8_t magic[4]; uint16_t host0, host1; uint32_t size, free_uncached, free_cached, reserved[3]; };
struct smem_entry { uint16_t canary, item; uint32_t size; uint16_t padding_data, padding_header; uint32_t reserved; };
struct smem_item { uint32_t item, offset, size; };

static volatile uint8_t *smem;
static vm_offset_t apcs, mutex_lock;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}
static void smem_poke(uint32_t offset, uint32_t value) { wr((vm_offset_t)smem + offset, value); }
static uint32_t smem_word(uint32_t offset) { return rd((vm_offset_t)smem + offset); }

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_MPSS }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

/* The global (uncached-from-bottom) allocator of Linux's qcom_smem_alloc_private(), under TCSR mutex 3. */
static bool
smem_alloc(uint32_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t cacheline = 0, psize = 0, partition = smem_partition(SMEM_US, SMEM_MPSS, &cacheline, &psize);
	const uint32_t item_size = (bytes + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	if (partition == 0) {
		return false;
	}
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(mutex_lock, 1);
		owner = rd(mutex_lock);
		if (owner == 1) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != 1) {
		printf(TAG "SMEM lock held by %u\n", owner);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached || at + need_bytes > header.free_cached) {
		printf(TAG "SMEM alloc %u: marks %#x/%#x/%#x refused\n", item, at, header.free_uncached, header.free_cached);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - bytes);
	smem_poke(partition + at + 12, 0);
	for (uint32_t z = 0; z < item_size; z += 4) {
		smem_poke(partition + at + (uint32_t)sizeof(entry) + z, 0);
	}
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(mutex_lock, 0);
	return made;
}

/* ---- SMP2P ------------------------------------------------------------------------------------------ */
struct smp2p {
	uint32_t magic, version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct { char name[16]; uint32_t value; } entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)

static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static void ring(uint32_t bit) { wr(apcs, bit); }

static bool
dsp_words(struct smp2p *words)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_THEIRS, &size);

	return at != 0 && size >= sizeof(*words) && smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static uint32_t
dsp_state(const struct smp2p *w)
{
	for (unsigned i = 0; i < w->valid && i < 16; i++) {
		if (strncmp(w->entry[i].name, "slave-kernel", 16) == 0) {
			return w->entry[i].value;
		}
	}
	return 0;
}

static void
show_words(const char *prefix, const struct smp2p *w)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < w->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s=%#x", w->entry[i].name, w->entry[i].value);
	}
	printf(TAG "%s: ver %#x flags %#x%s\n", prefix, w->version, w->flags, line);
}

static void
show_crash_reason(void)
{
	char text[200];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf(TAG "no crash reason item\n");
		return;
	}
	size = size < sizeof(text) - 1 ? size : sizeof(text) - 1;
	smem_peek(at, text, size);
	text[size] = 0;
	for (uint32_t i = 0; i < size; i++) {
		if (text[i] != 0 && (text[i] < 32 || text[i] > 126)) {
			text[i] = '.';
		}
	}
	printf(TAG "MODEM CRASH REASON: \"%s\"\n", text);
	snprintf(jw_state, sizeof(jw_state), "modem crash: %.120s", text);
}

static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		ring(DOORBELL_SMP2P);
	}
}

static bool
our_item_setup(void)
{
	uint32_t size = 0, name[4];

	our_item = smem_item(ITEM_SMP2P_OURS, &size);
	if (our_item == 0) {
		if (!smem_alloc(ITEM_SMP2P_OURS, sizeof(struct smp2p))) {
			return false;
		}
		our_item = smem_item(ITEM_SMP2P_OURS, &size);
	}
	if (our_item == 0 || size < sizeof(struct smp2p)) {
		return false;
	}
	for (uint32_t at = 0; at < sizeof(struct smp2p); at += 4) {
		smem_poke(our_item + at, 0);
	}
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_MPSS << 16);
	bzero(name, sizeof(name));
	strlcpy((char *)name, "master-kernel", sizeof(name));
	for (uint32_t w = 0; w < 4; w++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * w, name[w]);
	}
	/* "ipa" (dt qcom,smp2p-ipa-1-out): answer valid, clock off -- there is no IPA driver here */
	bzero(name, sizeof(name));
	strlcpy((char *)name, "ipa", sizeof(name));
	for (uint32_t w = 0; w < 4; w++) {
		smem_poke(our_item + SMP2P_AT_NAME(1) + 4 * w, name[w]);
	}
	{
		uint32_t ipa = 1;

		PE_parse_boot_argn("jm21_wifi_ipa", &ipa, sizeof(ipa));
		smem_poke(our_item + SMP2P_AT_VALUE(1), ipa);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 2u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	ring(DOORBELL_SMP2P);
	printf(TAG "SMP2P item %u at %#x\n", ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	return true;
}

/* ---- TrustZone -------------------------------------------------------------------------------------- */
#define SCM_CALL(svc, cmd)      (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_MP_ASSIGN           SCM_CALL(0xc, 0x16)
#define SCM_ASSIGN_ARGUMENTS    0x1117
#define VMID_HLOS               3
#define VMID_MSS_MSA            15
#define VMID_WLAN               0x18
#define VMID_WLAN_CE            0x19
#define VMID_NAV                0x2b
#define VM_PERM_RW              6

static int32_t
scm(uint64_t function, uint64_t arguments, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t *answer)
{
	uint64_t session = 0;

	for (int i = 0; i < 1000; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : 1;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = a;
		register uint64_t x3 __asm__("x3") = b;
		register uint64_t x4 __asm__("x4") = c;
		register uint64_t x5 __asm__("x5") = d;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != 1) {
			*answer = x1;
			return (int32_t)x0;
		}
	}
	return -1;
}

struct kbuf { vm_offset_t cached; uint64_t pa, size; };

static bool
kbuf_alloc(struct kbuf *b, uint64_t size)
{
	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &b->cached, size, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	bzero((void *)b->cached, size);
	b->pa = kvtophys(b->cached);
	b->size = size;
	return (b->pa >> 32) == 0;
}
static void kbuf_free(struct kbuf *b) { if (b->cached) kmem_free(kernel_map, b->cached, b->size); b->cached = 0; }
static void kbuf_flush(struct kbuf *b) { FlushPoC_DcacheRegion(b->cached, b->size); settle(); }

/* qcom_scm_assign_mem(): one region, from `src` to `dst` (all RW). */
static bool
assign(uint64_t pa, uint64_t size, const uint32_t *src, unsigned nsrc, const uint32_t *dst, unsigned ndst, const char *what)
{
	struct page {
		uint64_t address, size;
		uint64_t pad0[6];
		uint32_t sources[4];
		uint32_t pad1[12];
		struct { uint32_t vmid, permission; uint64_t context; uint32_t context_size, unused; } dest[4];
		uint8_t pad2[16];
		uint64_t more[7];
	} *p;
	struct kbuf k = {};
	uint64_t answer = ~0ull;
	int32_t r;

	_Static_assert(__offsetof(struct page, sources) == 64, "sources");
	_Static_assert(__offsetof(struct page, dest) == 128, "dest");
	_Static_assert(__offsetof(struct page, more) == 240, "more");
	if (!kbuf_alloc(&k, PAGE_SIZE)) {
		return false;
	}
	p = (struct page *)k.cached;
	p->address = pa;
	p->size = size;
	for (unsigned i = 0; i < nsrc; i++) {
		p->sources[i] = src[i];
	}
	for (unsigned i = 0; i < ndst; i++) {
		p->dest[i].vmid = dst[i];
		p->dest[i].permission = VM_PERM_RW;
	}
	p->more[0] = nsrc * 4;
	p->more[1] = k.pa + 128;
	p->more[2] = ndst * 24;
	p->more[3] = 0;
	kbuf_flush(&k);
	r = scm(SCM_MP_ASSIGN, SCM_ASSIGN_ARGUMENTS, k.pa, 16, k.pa + 64, k.pa + 240, &answer);
	kbuf_free(&k);
	printf(TAG "hyp assign %s %#llx+%#llx: %d/%#llx\n", what, pa, size, r, answer);
	return r == 0 && answer == 0;
}

/* ---- firmware --------------------------------------------------------------------------------------- */
static const uint8_t *
fw(const char *name, uint64_t *size)
{
	const uint8_t *d = hakios_image_file("fw", name, size, NULL);

	if (d == NULL) {
		printf(TAG "fw/%s missing\n", name);
	}
	return d;
}

static bool mss_started;

static bool
mss_boot(void)
{
	uint64_t mdt_size = 0, answer = ~0ull;
	const uint8_t *mdt = fw("modem.mdt", &mdt_size);
	uint32_t phoff, min_a = ~0u, max_a = 0;
	uint16_t phnum;
	struct kbuf meta = {};
	volatile uint8_t *mem;
	int32_t r;

	if (mdt == NULL || mdt_size < 52 || memcmp(mdt, "\177ELF", 4) != 0) {
		return false;
	}
	memcpy(&phoff, mdt + 28, 4);
	memcpy(&phnum, mdt + 44, 2);
	if (phnum > 64 || phoff + 32ull * phnum > mdt_size) {
		return false;
	}
	for (unsigned i = 0; i < phnum; i++) {
		const uint8_t *ph = mdt + phoff + 32 * i;
		uint32_t type, paddr, memsz, flags;

		memcpy(&type, ph, 4); memcpy(&paddr, ph + 12, 4); memcpy(&memsz, ph + 20, 4); memcpy(&flags, ph + 24, 4);
		if (type != 1 || memsz == 0 || ((flags >> 24) & 7) == 2) {
			continue;
		}
		min_a = paddr < min_a ? paddr : min_a;
		max_a = paddr + memsz > max_a ? paddr + memsz : max_a;
	}
	max_a = (max_a + 0xfff) & ~0xfffu;
	if (min_a != MSS_REGION || max_a - min_a > MSS_REGION_SIZE) {
		printf(TAG "modem layout unexpected %#x..%#x\n", min_a, max_a);
		return false;
	}
	/* a modem left running by an earlier kernel (warm restart) is shut down first */
	{
		struct smp2p w;

		if (dsp_words(&w)) {
			show_words("modem item already present", &w);
			r = scm(SCM_PAS_SHUTDOWN, 1, PAS_MSS, 0, 0, 0, &answer);
			printf(TAG "PAS shutdown before start: %d/%#llx\n", r, answer);
		}
	}
	if (!kbuf_alloc(&meta, mdt_size)) {
		return false;
	}
	memcpy((void *)meta.cached, mdt, mdt_size);
	kbuf_flush(&meta);
	r = scm(SCM_PAS_INIT_IMAGE, 2 | (2 << 6), PAS_MSS, meta.pa, 0, 0, &answer);
	printf(TAG "PAS init_image(4, %llu bytes): %d/%#llx\n", mdt_size, r, answer);
	if (r != 0 || answer != 0) {
		kbuf_free(&meta);
		return false;
	}
	r = scm(SCM_PAS_MEM_SETUP, 3, PAS_MSS, min_a, max_a - min_a, 0, &answer);
	printf(TAG "PAS mem_setup(%#x, %#x): %d/%#llx\n", min_a, max_a - min_a, r, answer);
	if (r != 0 || answer != 0) {
		kbuf_free(&meta);
		return false;
	}
	mem = (volatile uint8_t *)ml_io_map_unmappable(min_a, max_a - min_a, VM_WIMG_WCOMB);
	if (mem == NULL) {
		kbuf_free(&meta);
		return false;
	}
	for (unsigned i = 0; i < phnum; i++) {
		const uint8_t *ph = mdt + phoff + 32 * i, *data = NULL;
		uint32_t type, offset, paddr, filesz, memsz, flags, at, n = 0;
		uint64_t size = 0;
		char name[24];

		memcpy(&type, ph, 4); memcpy(&offset, ph + 4, 4); memcpy(&paddr, ph + 12, 4); memcpy(&filesz, ph + 16, 4);
		memcpy(&memsz, ph + 20, 4); memcpy(&flags, ph + 24, 4);
		if (type != 1 || memsz == 0 || ((flags >> 24) & 7) == 2) {
			continue;
		}
		at = paddr - min_a;
		if (filesz != 0) {
			snprintf(name, sizeof(name), "modem.b%02u", i);
			data = fw(name, &size);
			if (data == NULL || size < filesz) {
				printf(TAG "segment %u: %s short (%llu < %u)\n", i, name, size, filesz);
				ml_io_unmap((vm_offset_t)mem, max_a - min_a);
				kbuf_free(&meta);
				return false;
			}
		}
		if ((at & 7) == 0) {
			for (; n + 8 <= memsz; n += 8) {
				uint64_t v = 0;

				if (n + 8 <= filesz) {
					memcpy(&v, data + n, 8);
				} else {
					for (unsigned b = 0; b < 8 && n + b < filesz; b++) {
						v |= (uint64_t)data[n + b] << (8 * b);
					}
				}
				*(volatile uint64_t *)(mem + at + n) = v;
			}
		}
		for (; n < memsz; n++) {
			mem[at + n] = n < filesz ? data[n] : 0;
		}
	}
	settle();
	ml_io_unmap((vm_offset_t)mem, max_a - min_a);
	printf(TAG "modem image written: %#x bytes at %#x\n", max_a - min_a, min_a);
	r = scm(SCM_PAS_AUTH_AND_RESET, 1, PAS_MSS, 0, 0, 0, &answer);
	printf(TAG "PAS auth_and_reset(4): %d/%#llx\n", r, answer);
	kbuf_free(&meta);
	if (r != 0 || answer != 0) {
		return false;
	}
	mss_started = true;
	return true;
}

/* ---- RMTFS storage: GPT partitions of /dev/rmmc0, read once into memory -------------------------------- */
static struct {
	const char *path, *partition;
	uint8_t *data;
	uint64_t size;
	bool open;
} rmtfs_files[4] = {
	{ "/boot/modem_fs1", "modemst1" }, { "/boot/modem_fs2", "modemst2" }, { "/boot/modem_fsc", "fsc" },
	{ "/boot/modem_fsg", "fsg" },
};

static int
disk_read(vnode_t vp, uint64_t offset, void *buf, uint64_t len)
{
	int resid = 0;
	vfs_context_t ctx = vfs_context_kernel();
	int e = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)len, (off_t)offset, UIO_SYSSPACE, IO_NOAUTH,
	    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));

	return e != 0 ? e : (resid != 0 ? EIO : 0);
}

static unsigned
rmtfs_load(void)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	uint8_t *hdr = NULL, *ents = NULL;
	unsigned loaded = 0;
	uint64_t entries_lba;
	uint32_t count, esize;

	if (vnode_open("/dev/rmmc0", FREAD, 0, 0, &vp, ctx) != 0) {
		printf(TAG "cannot open /dev/rmmc0\n");
		return 0;
	}
	hdr = kalloc_data(512, Z_WAITOK | Z_ZERO);
	if (hdr == NULL || disk_read(vp, 512, hdr, 512) != 0 || memcmp(hdr, "EFI PART", 8) != 0) {
		printf(TAG "no GPT on /dev/rmmc0\n");
		goto out;
	}
	memcpy(&entries_lba, hdr + 72, 8);
	memcpy(&count, hdr + 80, 4);
	memcpy(&esize, hdr + 84, 4);
	if (count == 0 || count > 256 || esize != 128) {
		goto out;
	}
	ents = kalloc_data(count * esize, Z_WAITOK | Z_ZERO);
	if (ents == NULL || disk_read(vp, entries_lba * 512, ents, ((count * esize) + 511) & ~511u) != 0) {
		goto out;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = ents + i * esize;
		char name[37];
		uint64_t first, last;

		for (unsigned c = 0; c < 36; c++) {
			name[c] = (char)e[56 + 2 * c];
		}
		name[36] = 0;
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		for (unsigned f = 0; f < 4; f++) {
			uint64_t size = (last - first + 1) * 512;

			if (strcmp(name, rmtfs_files[f].partition) != 0 || rmtfs_files[f].data != NULL || size > (16u << 20)) {
				continue;
			}
			rmtfs_files[f].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
			if (rmtfs_files[f].data == NULL) {
				continue;
			}
			if (disk_read(vp, first * 512, rmtfs_files[f].data, size) == 0) {
				rmtfs_files[f].size = size;
				loaded++;
				printf(TAG "rmtfs: %s %llu bytes (LBA %llu)\n", name, size, first);
			} else {
				kfree_data(rmtfs_files[f].data, size);
				rmtfs_files[f].data = NULL;
			}
		}
	}
out:
	if (ents) {
		kfree_data(ents, count * esize);
	}
	if (hdr) {
		kfree_data(hdr, 512);
	}
	vnode_close(vp, FREAD, ctx);
	return loaded;
}

static volatile uint8_t *rmtfs_window;
static bool rmtfs_shared;

static bool
rmtfs_buffer_setup(void)
{
	static const uint32_t src[] = { VMID_HLOS }, dst[] = { VMID_HLOS, VMID_MSS_MSA, VMID_NAV };

	rmtfs_window = (volatile uint8_t *)ml_io_map_unmappable(RMTFS_BUFFER_PA, RMTFS_BUFFER_SIZE, VM_WIMG_WCOMB);
	if (rmtfs_window == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < RMTFS_BUFFER_SIZE; i += 8) {
		*(volatile uint64_t *)(rmtfs_window + i) = 0;
	}
	settle();
	rmtfs_shared = assign(RMTFS_BUFFER_PA, RMTFS_BUFFER_SIZE, src, 1, dst, 3, "rmtfs");
	return rmtfs_shared;
}

/* ---- GLINK over SMEM -------------------------------------------------------------------------------- */
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16
enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_INTENT_REUSE      1u

struct glink_message { uint16_t command, param1; uint32_t param2; };
struct glink_packet { struct glink_packet *next; uint32_t length; uint8_t data[]; };
struct glink_channel {
	const char *name;
	uint32_t ours;
	struct { uint32_t size, count; } intents[3];
	uint32_t theirs;
	bool asked, acknowledged, open;
	bool request_answered, request_granted;
	struct { uint32_t id, size; bool there, in_use; } their_intents[64];
	struct { uint32_t size, fill; uint8_t *data; } our_intents[24];
	uint32_t our_intent_count;
	struct glink_packet *first, *last;
};

static LCK_MTX_DECLARE(glink_lock, &jw_grp);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken, agreed;
	uint32_t features, said, heard, trace;
	struct glink_channel qrtr;
	char others[4][33];
} glink = {
	.qrtr = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
};

static bool
glink_find(bool may_alloc)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 && may_alloc && smem_alloc(ITEM_GLINK_DESCRIPTOR, 32)) {
		printf(TAG "glink: descriptor allocated by us\n");
		glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	}
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	if (glink.ours == 0 && may_alloc && smem_alloc(ITEM_GLINK_OUR_QUEUE, 16384)) {
		printf(TAG "glink: tx fifo allocated by us\n");
		glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	}
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	ring(DOORBELL_GLINK);
	glink.said++;
	return true;
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message m = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&m, sizeof(m), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf(TAG "glink > %u %u %#x%s\n", command, param1, param2, sent ? "" : " NOT SENT");
	}
}

static bool
glink_offer(struct glink_channel *c, uint32_t size)
{
	uint32_t words[2] = { size, c->our_intent_count + 1 }, i = c->our_intent_count;

	if (i >= sizeof(c->our_intents) / sizeof(c->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (c->our_intents[i].data == NULL || c->our_intents[i].size != size) {
		c->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (c->our_intents[i].data == NULL) {
		return false;
	}
	c->our_intents[i].size = size;
	c->our_intents[i].fill = 0;
	c->our_intent_count++;
	glink_say(GLINK_INTENT, c->ours, 1, words, sizeof(words));
	return true;
}

static void
glink_pump(void)
{
	struct glink_channel *c = &glink.qrtr;
	struct glink_message m;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(m);

		if (waiting < sizeof(m)) {
			return;
		}
		glink_peek(&m, 0, sizeof(m));
		if (m.command == GLINK_OPEN) {
			length += m.param2;
		} else if (m.command == GLINK_INTENT) {
			length += 8 * m.param2;
		} else if (m.command == GLINK_DATA || m.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf(TAG "glink: message of %u bytes (cmd %u): broken\n", length, m.command);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && m.command != GLINK_DATA && m.command != GLINK_DATA_MORE && m.command != GLINK_INTENT &&
		    m.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf(TAG "glink < %u %u %#x\n", m.command, m.param1, m.param2);
		}
		bool ours = c->theirs != 0 && m.param1 == c->theirs;
		switch (m.command) {
		case GLINK_VERSION:
			if (m.param1 == 1) {
				glink.features &= m.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, 1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (m.param1 == 1 && m.param2 == glink.features) {
				glink.agreed = true;
			} else if (m.param1 != 0) {
				glink.features &= m.param2;
				glink_say(GLINK_VERSION, 1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(m), m.param2 < sizeof(name) - 1 ? m.param2 : sizeof(name) - 1);
			glink_advance(length);
			if (strcmp(name, c->name) != 0) {
				printf(TAG "glink: modem opens \"%s\" (%u): unanswered\n", name, m.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			c->theirs = m.param1;
			glink_say(GLINK_OPEN_ACK, c->theirs, 0, NULL, 0);
			if (!c->asked) {
				uint32_t size = (uint32_t)strlen(c->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, c->name, sizeof(name));
				glink_say(GLINK_OPEN, c->ours, size, name, (size + 7) & ~7u);
				c->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			if (m.param1 == c->ours) {
				c->acknowledged = true;
			}
			break;
		case GLINK_INTENT:
			for (uint32_t i = 0; ours && i < m.param2; i++) {
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < 64; n++) {
					if (!c->their_intents[n].there || c->their_intents[n].id == words[1]) {
						c->their_intents[n].there = true;
						c->their_intents[n].in_use = false;
						c->their_intents[n].size = words[0];
						c->their_intents[n].id = words[1];
						break;
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			for (unsigned n = 0; ours && n < 64; n++) {
				if (c->their_intents[n].there && c->their_intents[n].id == m.param2) {
					c->their_intents[n].in_use = false;
					c->their_intents[n].there = m.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if (ours) {
				bool granted = glink_offer(c, m.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, c->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			if (ours || m.param1 == c->ours) {
				c->request_answered = true;
				c->request_granted = m.param2 != 0;
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = m.param2 - 1;

			if (!ours || intent >= c->our_intent_count || words[0] > c->our_intents[intent].size - c->our_intents[intent].fill) {
				printf(TAG "glink: data %u bytes ch %u intent %u dropped\n", words[0], m.param1, m.param2);
				glink_advance(length);
				break;
			}
			glink_peek(c->our_intents[intent].data + c->our_intents[intent].fill, 16, words[0]);
			c->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *p = kalloc_data(sizeof(*p) + c->our_intents[intent].fill, Z_NOWAIT);

				if (p != NULL) {
					p->next = NULL;
					p->length = c->our_intents[intent].fill;
					memcpy(p->data, c->our_intents[intent].data, p->length);
					if (c->last != NULL) {
						c->last->next = p;
					} else {
						c->first = p;
					}
					c->last = p;
				}
				c->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, c->ours, m.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			ring(DOORBELL_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, m.param1, 0, NULL, 0);
			if (ours) {
				printf(TAG "glink: modem closes IPCRTR\n");
				c->open = false;
				c->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf(TAG "glink: unknown command %u %u %#x: broken\n", m.command, m.param1, m.param2);
			glink.broken = true;
			return;
		}
		if (c->theirs != 0 && c->acknowledged && !c->open) {
			c->open = true;
			printf(TAG "glink: IPCRTR open (ours %u, theirs %u)\n", c->ours, c->theirs);
			for (unsigned i = 0; i < 3; i++) {
				for (unsigned n = 0; n < c->intents[i].count; n++) {
					glink_offer(c, c->intents[i].size);
				}
			}
		}
	}
}

static int
glink_send(const void *data, uint32_t length)
{
	struct glink_channel *c = &glink.qrtr;
	struct { struct glink_message m; uint32_t size, left; } header = { { GLINK_DATA, (uint16_t)c->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !c->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < 64; n++) {
			if (!c->their_intents[n].there || c->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!c->their_intents[n].in_use && (found < 0 || c->their_intents[n].size < c->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			c->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, c->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && c->request_answered && !c->request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	c->their_intents[found].in_use = true;
	header.m.param2 = c->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR -------------------------------------------------------------------------------------------- */
#define QRTR_MAX                8192
#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };
struct qrtr_header { uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port; };
struct qrtr_control { uint32_t command; uint32_t words[4]; };

typedef void (*receive_t)(void *context, uint32_t node, uint32_t port, const void *data, size_t length);
struct qport {
	uint32_t number; receive_t receive; void *context;
	hakios_qrtr_server_t server;                    /* the QMI helpers' ports (hakios_qrtr_ops below) */
	struct { uint32_t service, instance; bool asked, fresh; } lookups[4];
};

static LCK_MTX_DECLARE(qrtr_lock, &jw_grp);
static struct qport qports[16];
static struct { uint32_t service, instance, node, port; bool there; } qservers[64];
static uint32_t qrtr_peer = ~0u, qrtr_next_port = 0x4000, qrtr_in, qrtr_out, qrtr_hellos;
static struct { uint32_t service, instance, port; } our_servers[4];

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *p;
	int error;

	if (length == 0 || length > QRTR_MAX) {
		return EMSGSIZE;
	}
	p = kalloc_data(sizeof(*p) + padded, Z_WAITOK | Z_ZERO);
	if (p == NULL) {
		return ENOMEM;
	}
	p->version = 1;
	p->type = type;
	p->from_node = QRTR_US;
	p->from_port = from_port;
	p->size = length;
	p->to_node = to_node;
	p->to_port = to_port;
	memcpy(p + 1, data, length);
	error = glink_send(p, (uint32_t)sizeof(*p) + padded);
	kfree_data(p, sizeof(*p) + padded);
	qrtr_out += error == 0;
	return error;
}

static void
qrtr_announce(uint32_t node, int server)
{
	struct qrtr_control p = { .command = QRTR_NEW_SERVER };

	if (server >= 0) {
		p.words[0] = our_servers[server].service;
		p.words[1] = our_servers[server].instance;
		p.words[2] = QRTR_US;
		p.words[3] = our_servers[server].port;
	}
	qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, node, QRTR_CONTROL, &p, sizeof(p));
}

static void
services_string(void)
{
	int at = 0;

	jw_services[0] = 0;
	for (unsigned i = 0; i < 64 && at < (int)sizeof(jw_services) - 16; i++) {
		if (qservers[i].there) {
			at += snprintf(jw_services + at, sizeof(jw_services) - (size_t)at, "%s%u:%u", at ? " " : "", qservers[i].service,
			    qservers[i].instance >> 8);
		}
	}
}

static void
qrtr_tell(uint32_t service, uint32_t instance, uint32_t node, uint32_t port, bool arrived)
{
	for (unsigned p = 0; p < 16; p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server = NULL;
			void *ctx = NULL;

			lck_mtx_lock(&qrtr_lock);
			if (qports[p].number != 0 && qports[p].server != NULL && qports[p].lookups[l].asked && !qports[p].lookups[l].fresh &&
			    qports[p].lookups[l].service == service &&
			    (qports[p].lookups[l].instance == 0 || qports[p].lookups[l].instance == instance)) {
				server = qports[p].server;
				ctx = qports[p].context;
			}
			lck_mtx_unlock(&qrtr_lock);
			if (server != NULL) {
				server(ctx, arrived, service, instance & 0xff, instance >> 8, node, port);
			}
		}
	}
}

/* lookups made since the last pass are told of the servers already there; from the link thread */
static void
qrtr_fresh(void)
{
	for (unsigned p = 0; p < 16; p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server;
			uint32_t service, instance;
			void *ctx;

			lck_mtx_lock(&qrtr_lock);
			if (qports[p].number == 0 || qports[p].server == NULL || !qports[p].lookups[l].fresh) {
				lck_mtx_unlock(&qrtr_lock);
				continue;
			}
			qports[p].lookups[l].fresh = false;
			server = qports[p].server;
			service = qports[p].lookups[l].service;
			instance = qports[p].lookups[l].instance;
			ctx = qports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			printf(TAG "qrtr: a QMI client looks up service %#x inst %#x\n", service, instance);
			for (unsigned i = 0; i < 64; i++) {
				if (qservers[i].there && qservers[i].service == service && (instance == 0 || qservers[i].instance == instance)) {
					printf(TAG "qrtr: ... told of %u:%u\n", qservers[i].node, qservers[i].port);
					server(ctx, true, service, qservers[i].instance & 0xff, qservers[i].instance >> 8, qservers[i].node,
					    qservers[i].port);
				}
			}
		}
	}
}

static void
qrtr_control(const struct qrtr_header *h, const struct qrtr_control *c)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1;

	switch (c->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, h->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf(TAG "qrtr: HELLO from node %u\n", h->from_node);
		for (int i = 0; i < 4; i++) {
			if (our_servers[i].port != 0) {
				qrtr_announce(h->from_node, i);
			}
		}
		break;
	case QRTR_NEW_SERVER:
		if (c->words[0] == 0 && c->words[3] == 0) {
			break;
		}
		lck_mtx_lock(&qrtr_lock);
		for (unsigned i = 0; i < 64; i++) {
			if (qservers[i].there && qservers[i].node == c->words[2] && qservers[i].port == c->words[3]) {
				found = (int)i;
				break;
			}
			if (!qservers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qservers[found].there = true;
			qservers[found].service = c->words[0];
			qservers[found].instance = c->words[1];
			qservers[found].node = c->words[2];
			qservers[found].port = c->words[3];
		}
		services_string();
		lck_mtx_unlock(&qrtr_lock);
		printf(TAG "qrtr: service %u (%#x) v%u inst %u at %u:%u\n", c->words[0], c->words[0], c->words[1] & 0xff,
		    c->words[1] >> 8, c->words[2], c->words[3]);
		qrtr_tell(c->words[0], c->words[1], c->words[2], c->words[3], true);
		break;
	case QRTR_DEL_SERVER:
	case QRTR_BYE:
		lck_mtx_lock(&qrtr_lock);
		for (unsigned i = 0; i < 64; i++) {
			if (qservers[i].there && (c->command == QRTR_BYE ? qservers[i].node == h->from_node :
			    qservers[i].node == c->words[2] && qservers[i].port == c->words[3])) {
				qservers[i].there = false;
				printf(TAG "qrtr: service %u gone\n", qservers[i].service);
				lck_mtx_unlock(&qrtr_lock);
				qrtr_tell(qservers[i].service, qservers[i].instance, qservers[i].node, qservers[i].port, false);
				lck_mtx_lock(&qrtr_lock);
			}
		}
		services_string();
		lck_mtx_unlock(&qrtr_lock);
		break;
	case QRTR_NEW_LOOKUP: {
		unsigned matches = 0;

		for (int i = 0; i < 4; i++) {
			if (our_servers[i].port != 0 && (c->words[0] == 0 || our_servers[i].service == c->words[0]) &&
			    (c->words[1] == 0 || our_servers[i].instance == c->words[1])) {
				qrtr_announce(h->from_node, i);
				matches++;
			}
		}
		qrtr_announce(h->from_node, -1);
		printf(TAG "qrtr: modem looks up %u inst %#x: %u\n", c->words[0], c->words[1], matches);
		break;
	}
	default:
		break;
	}
}

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header h;
	receive_t receive = NULL;
	void *context = NULL;
	uint32_t at;

	if (length >= sizeof(h) && data[0] == 1) {
		memcpy(&h, data, sizeof(h));
		at = sizeof(h);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&h, sizeof(h));
		h.type = data[1];
		h.confirm = data[2] & 1;
		memcpy(&h.size, data + 4, 4);
		h.from_node = (uint32_t)(data[8] | data[9] << 8);
		h.from_port = (uint32_t)(data[10] | data[11] << 8);
		h.to_node = (uint32_t)(data[12] | data[13] << 8);
		h.to_port = (uint32_t)(data[14] | data[15] << 8);
		h.from_port = h.from_port == 0xfffe ? QRTR_CONTROL : h.from_port;
		h.to_port = h.to_port == 0xfffe ? QRTR_CONTROL : h.to_port;
		at = 16 + data[3];
	} else {
		return;
	}
	if (h.size == 0 || at + h.size > length) {
		return;
	}
	qrtr_in++;
	if (qrtr_peer == ~0u) {
		qrtr_peer = h.from_node;
	}
	if (h.type == QRTR_RESUME_TX) {
		return;
	}
	if (h.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { h.to_node, h.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, h.to_port, h.from_node, h.from_port, &resume, sizeof(resume));
	}
	if (h.to_port == QRTR_CONTROL || h.type != QRTR_DATA) {
		struct qrtr_control c = { 0 };

		memcpy(&c, data + at, h.size < sizeof(c) ? h.size : sizeof(c));
		qrtr_control(&h, &c);
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < 16; p++) {
		if (qports[p].number == h.to_port) {
			receive = qports[p].receive;
			context = qports[p].context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (receive != NULL) {
		static unsigned shown;

		if (shown++ < 40 && h.size >= 7) {
			const uint8_t *m = data + at;

			printf(TAG "qmi< %u:%u -> port %u type %u txn %u msg %#x len %u\n", h.from_node, h.from_port, h.to_port, m[0],
			    m[1] | m[2] << 8, m[3] | m[4] << 8, h.size);
		}
		receive(context, h.from_node, h.from_port, data + at, h.size);
	} else {
		printf(TAG "qrtr: %u bytes to unbound port %u\n", h.size, h.to_port);
	}
}

static struct qport *
qrtr_open(receive_t receive, void *context)
{
	struct qport *port = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < 16 && port == NULL; p++) {
		if (qports[p].number == 0) {
			port = &qports[p];
			bzero(port, sizeof(*port));
			port->number = qrtr_next_port++;
			port->receive = receive;
			port->context = context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return port;
}

static int
qrtr_send(struct qport *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	if (port == NULL || port->number == 0 || length > QRTR_MAX) {
		return EINVAL;
	}
	return qrtr_out_packet(QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}

static void
qrtr_close(struct qport *port)
{
	struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, port ? port->number : 0 } };

	if (port == NULL || port->number == 0) {
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	port->number = 0;
	lck_mtx_unlock(&qrtr_lock);
	if (qrtr_peer != ~0u && glink.qrtr.open) {
		qrtr_out_packet(QRTR_DEL_CLIENT, gone.words[1], qrtr_peer, QRTR_CONTROL, &gone, sizeof(gone));
	}
}

static bool
qrtr_find(uint32_t service, uint32_t *node, uint32_t *port)
{
	bool found = false;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned i = 0; i < 64 && !found; i++) {
		if (qservers[i].there && qservers[i].service == service) {
			*node = qservers[i].node;
			*port = qservers[i].port;
			found = true;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return found;
}

/* The QMI helpers' transport (sys/hakios.qrtr.h): ath10k's qmi.c speaks WLFW through these (jm21_wifi=2). */
struct hakios_qrtr_port { struct qport q; };     /* (never instantiated: the pointer is a struct qport's) */
static struct hakios_qrtr_port *
ops_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct qport *p = qrtr_open(receive, context);

	if (p != NULL) {
		p->server = server;
	}
	return (struct hakios_qrtr_port *)(void *)p;
}
static int
ops_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	static unsigned shown;
	int e = qrtr_send((struct qport *)(void *)port, node, to, data, length);
	const uint8_t *m = data;

	if (shown++ < 24 && length >= 7) {
		printf(TAG "qmi> port %u -> %u:%u type %u txn %u msg %#x len %zu: %d\n", ((struct qport *)(void *)port)->number, node, to,
		    m[0], m[1] | m[2] << 8, m[3] | m[4] << 8, length, e);
	}
	return e;
}
static int
ops_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	struct qport *p = (struct qport *)(void *)port;
	int error = ENOSPC;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned l = 0; p != NULL && l < 4 && error != 0; l++) {
		if (!p->lookups[l].asked) {
			p->lookups[l].service = service;
			p->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			p->lookups[l].asked = p->lookups[l].fresh = true;
			error = 0;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return error;
}
static void ops_close(struct hakios_qrtr_port *port) { qrtr_close((struct qport *)(void *)port); }
static const struct hakios_qrtr_ops jm21_qrtr_ops = { .open = ops_open, .send = ops_send, .lookup = ops_lookup, .close = ops_close };

/* ---- QMI helpers ------------------------------------------------------------------------------------- */
#define QMI_REQUEST             0
#define QMI_RESPONSE            2
#define QMI_INDICATION          4

static const uint8_t *
qmi_tlv(const uint8_t *m, size_t length, uint8_t type, uint16_t *size)
{
	for (size_t at = 7; at + 3 <= length;) {
		uint16_t n = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (at + 3 + n > length) {
			return NULL;
		}
		if (m[at] == type) {
			*size = n;
			return m + at + 3;
		}
		at += 3 + n;
	}
	return NULL;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static unsigned
put_tlv(uint8_t *at, uint8_t type, const void *value, uint16_t length)
{
	at[0] = type;
	at[1] = (uint8_t)length;
	at[2] = (uint8_t)(length >> 8);
	memcpy(at + 3, value, length);
	return 3u + length;
}
static void
qmi_result_answer(struct qport *from, uint32_t node, uint32_t port, const uint8_t *req, uint16_t result, uint16_t error,
    const uint8_t *extra, unsigned extra_len)
{
	uint8_t a[64];
	unsigned n = 7;
	uint8_t r[4] = { (uint8_t)result, (uint8_t)(result >> 8), (uint8_t)error, (uint8_t)(error >> 8) };

	a[0] = QMI_RESPONSE;
	a[1] = req[1];
	a[2] = req[2];
	a[3] = req[3];
	a[4] = req[4];
	n += put_tlv(a + n, 2, r, 4);
	if (extra_len && n + extra_len <= sizeof(a)) {
		memcpy(a + n, extra, extra_len);
		n += extra_len;
	}
	a[5] = (uint8_t)(n - 7);
	a[6] = (uint8_t)((n - 7) >> 8);
	qrtr_send(from, node, port, a, n);
}

/* ---- RMTFS server (service 14) ----------------------------------------------------------------------- */
static struct qport *rmtfs_port, *tftp_port, *servreg_port;
static uint32_t rmtfs_requests, rmtfs_reads, rmtfs_writes, tftp_requests, tftp_served, servreg_requests;

static void
rmtfs_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *v;
	uint8_t extra[16];
	uint16_t message, size = 0;
	unsigned file = 4;

	if (length < 7 || m[0] != QMI_REQUEST) {
		return;
	}
	message = (uint16_t)(m[3] | m[4] << 8);
	rmtfs_requests++;
	switch (message) {
	case 1:         /* open */
		v = qmi_tlv(m, length, 1, &size);
		for (unsigned f = 0; v != NULL && f < 4; f++) {
			if (size == strlen(rmtfs_files[f].path) && memcmp(v, rmtfs_files[f].path, size) == 0) {
				file = f;
			}
		}
		if (file == 4 || rmtfs_files[file].data == NULL) {
			printf(TAG "rmtfs: open \"%.*s\": not here\n", v ? (int)(size < 64 ? size : 64) : 0, v ? (const char *)v : "");
			qmi_result_answer(rmtfs_port, node, port, m, 1, 1, NULL, 0);
			return;
		}
		rmtfs_files[file].open = true;
		printf(TAG "rmtfs: open %s\n", rmtfs_files[file].path);
		{
			uint32_t id = file + 1;
			unsigned n = put_tlv(extra, 0x10, &id, 4);

			qmi_result_answer(rmtfs_port, node, port, m, 0, 0, extra, n);
		}
		return;
	case 2:         /* close */
		v = qmi_tlv(m, length, 1, &size);
		if (v != NULL && size == 4 && le32(v) >= 1 && le32(v) <= 4) {
			rmtfs_files[le32(v) - 1].open = false;
		}
		qmi_result_answer(rmtfs_port, node, port, m, 0, 0, NULL, 0);
		return;
	case 4: {       /* alloc buff */
		uint64_t pa = RMTFS_BUFFER_PA;

		v = qmi_tlv(m, length, 2, &size);
		if (v == NULL || size != 4 || le32(v) > RMTFS_BUFFER_SIZE || !rmtfs_shared) {
			printf(TAG "rmtfs: alloc %u refused\n", v && size == 4 ? le32(v) : 0);
			qmi_result_answer(rmtfs_port, node, port, m, 1, 1, NULL, 0);
			return;
		}
		printf(TAG "rmtfs: alloc %u -> %#llx\n", le32(v), pa);
		qmi_result_answer(rmtfs_port, node, port, m, 0, 0, extra, put_tlv(extra, 0x10, &pa, 8));
		return;
	}
	case 3: {       /* rw iovec */
		const uint8_t *dir, *ent;
		uint16_t dsize = 0, esize = 0;
		bool write;

		v = qmi_tlv(m, length, 1, &size);
		dir = qmi_tlv(m, length, 2, &dsize);
		ent = qmi_tlv(m, length, 3, &esize);
		file = v != NULL && size == 4 && le32(v) >= 1 && le32(v) <= 4 ? le32(v) - 1 : 4;
		if (file == 4 || !rmtfs_files[file].open || dir == NULL || dsize != 1 || ent == NULL || esize < 1 ||
		    esize != 1 + 12 * ent[0] || rmtfs_window == NULL) {
			qmi_result_answer(rmtfs_port, node, port, m, 1, 2, NULL, 0);
			return;
		}
		write = dir[0] != 0;
		for (unsigned e = 0; e < ent[0]; e++) {
			uint64_t sector = le32(ent + 1 + 12 * e), phys = le32(ent + 5 + 12 * e), sectors = le32(ent + 9 + 12 * e);
			uint64_t bytes = sectors * 512;

			if (phys < RMTFS_BUFFER_PA || phys - RMTFS_BUFFER_PA + bytes > RMTFS_BUFFER_SIZE ||
			    sector * 512 + bytes > rmtfs_files[file].size) {
				qmi_result_answer(rmtfs_port, node, port, m, 1, 2, NULL, 0);
				return;
			}
			for (uint64_t i = 0; i < bytes; i++) {
				if (write) {
					rmtfs_files[file].data[sector * 512 + i] = rmtfs_window[phys - RMTFS_BUFFER_PA + i];
				} else {
					rmtfs_window[phys - RMTFS_BUFFER_PA + i] = rmtfs_files[file].data[sector * 512 + i];
				}
			}
		}
		settle();
		rmtfs_reads += !write;
		rmtfs_writes += write;
		if (rmtfs_reads + rmtfs_writes <= 8) {
			printf(TAG "rmtfs: %s %s (%u pieces)\n", write ? "write (memory only)" : "read", rmtfs_files[file].partition, ent[0]);
		}
		qmi_result_answer(rmtfs_port, node, port, m, 0, 0, NULL, 0);
		return;
	}
	case 5: {       /* get dev error */
		uint8_t none = 0;

		qmi_result_answer(rmtfs_port, node, port, m, 0, 0, extra, put_tlv(extra, 0x10, &none, 1));
		return;
	}
	default:
		qmi_result_answer(rmtfs_port, node, port, m, 1, 2, NULL, 0);
		return;
	}
}

/* ---- TFTP over QRTR (tqftpserv, service 4096) --------------------------------------------------------- */
#define TFTP_RRQ 1
#define TFTP_WRQ 2
#define TFTP_DATA 3
#define TFTP_ACK 4
#define TFTP_ERROR 5
#define TFTP_OACK 6
#define TFTP_MAX_BLOCK 8000
#define TFTP_MAX_WINDOW 4
#define TFTP_WRITE_MAX (1u << 20)

struct tftp_transfer {
	struct qport *port;
	uint32_t node, peer;
	const uint8_t *data;
	uint64_t size;
	uint32_t blksize, wsize, rsize, seek, sent, expected, written_size;
	bool done, writing, owned;
	uint8_t *written;
	char name[96];
};
static struct tftp_transfer transfers[4];
static struct { char name[96]; uint8_t *data; uint32_t size; } tftp_files[8];

static void
tftp_error(struct qport *from, uint32_t node, uint32_t port, uint16_t code, const char *text)
{
	uint8_t p[64];
	size_t n = strlen(text) + 1;

	n = n > sizeof(p) - 4 ? sizeof(p) - 4 : n;
	p[0] = 0;
	p[1] = TFTP_ERROR;
	p[2] = (uint8_t)(code >> 8);
	p[3] = (uint8_t)code;
	memcpy(p + 4, text, n);
	p[4 + n - 1] = 0;
	qrtr_send(from, node, port, p, 4 + n);
}

static void
tftp_end(struct tftp_transfer *t)
{
	if (t->written != NULL) {
		kfree_data(t->written, TFTP_WRITE_MAX);
		t->written = NULL;
	}
	if (t->port != NULL) {
		qrtr_close(t->port);
		t->port = NULL;
	}
	t->data = NULL;
	t->done = true;
}

static void
tftp_send_window(struct tftp_transfer *t, uint32_t from)
{
	uint8_t *p = kalloc_data(t->blksize + 4, Z_WAITOK);

	if (p == NULL) {
		return;
	}
	for (uint32_t w = 0; w < t->wsize; w++) {
		uint32_t block = from + 1 + w;
		uint64_t offset = (uint64_t)t->seek + (uint64_t)(block - 1) * t->blksize;
		uint32_t n = offset >= t->size ? 0 : (uint32_t)(t->size - offset < t->blksize ? t->size - offset : t->blksize);

		if (t->rsize != 0 && (uint64_t)(block - 1) * t->blksize >= t->rsize) {
			break;
		}
		if (t->rsize != 0 && (uint64_t)block * t->blksize > t->rsize) {
			uint32_t left = (uint32_t)(t->rsize - (uint64_t)(block - 1) * t->blksize);

			n = left < n ? left : n;
		}
		p[0] = 0;
		p[1] = TFTP_DATA;
		p[2] = (uint8_t)(block >> 8);
		p[3] = (uint8_t)block;
		if (n != 0) {
			memcpy(p + 4, t->data + offset, n);
		}
		if (qrtr_send(t->port, t->node, t->peer, p, 4 + n) != 0) {
			break;
		}
		t->sent = block;
		if (n < t->blksize) {
			break;
		}
	}
	kfree_data(p, t->blksize + 4);
}

static void
tftp_transfer_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct tftp_transfer *t = context;
	const uint8_t *m = data;
	uint16_t opcode, block;

	if (t->done || node != t->node || port != t->peer || length < 4) {
		return;
	}
	opcode = (uint16_t)(m[0] << 8 | m[1]);
	block = (uint16_t)(m[2] << 8 | m[3]);
	if (t->writing && opcode == TFTP_DATA) {
		uint32_t n = (uint32_t)(length - 4);
		uint8_t ack[4] = { 0, TFTP_ACK, (uint8_t)(block >> 8), (uint8_t)block };

		if (block != t->expected || t->written_size + n > TFTP_WRITE_MAX) {
			tftp_error(t->port, node, port, 4, "bad");
			tftp_end(t);
			return;
		}
		memcpy(t->written + t->written_size, m + 4, n);
		t->written_size += n;
		t->expected++;
		if (block % t->wsize == 0 || n < t->blksize) {
			qrtr_send(t->port, node, port, ack, sizeof(ack));
		}
		if (n < t->blksize) {
			unsigned f, free_slot = 8;

			for (f = 0; f < 8; f++) {
				if (tftp_files[f].data != NULL && strcmp(tftp_files[f].name, t->name) == 0) {
					break;
				}
				if (tftp_files[f].data == NULL && free_slot == 8) {
					free_slot = f;
				}
			}
			f = f == 8 ? free_slot : f;
			if (f < 8) {
				if (tftp_files[f].data != NULL) {
					kfree_data(tftp_files[f].data, tftp_files[f].size);
				}
				tftp_files[f].data = kalloc_data(t->written_size ? t->written_size : 1, Z_WAITOK);
				if (tftp_files[f].data != NULL) {
					memcpy(tftp_files[f].data, t->written, t->written_size);
					tftp_files[f].size = t->written_size;
					strlcpy(tftp_files[f].name, t->name, sizeof(tftp_files[f].name));
				}
			}
			printf(TAG "tftp: modem wrote \"%s\" (%u bytes, memory only)\n", t->name, t->written_size);
			tftp_served++;
			tftp_end(t);
		}
		return;
	}
	if (opcode == TFTP_ERROR) {
		if (block != 9) {
			printf(TAG "tftp: %s: error %u\n", t->name, block);
		}
		tftp_end(t);
		return;
	}
	if (opcode != TFTP_ACK) {
		tftp_end(t);
		return;
	}
	if ((uint64_t)t->seek + (uint64_t)block * t->blksize >= t->size || (t->rsize != 0 && (uint64_t)block * t->blksize >= t->rsize)) {
		tftp_served++;
		tftp_end(t);
		return;
	}
	tftp_send_window(t, block);
}

/* /readonly/firmware/image/NAME (and the vendor spellings) -> fw/NAME of the RAM disk */
static const char *
tftp_file_of(const char *path)
{
	static const char *const prefixes[] = { "/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/",
		"/readonly/vendor/firmware/", "/readonly/firmware/" };

	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t n = strlen(prefixes[i]);

		if (strncmp(path, prefixes[i], n) == 0 && strchr(path + n, '/') == NULL && strnstr(path + n, "..", 96) == NULL) {
			return path + n;
		}
	}
	return NULL;
}

static void
tftp_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	const char *end = (const char *)m + length, *name, *mode, *p;
	struct tftp_transfer *t = NULL;
	uint32_t blksize = 512, wsize = 1, rsize = 0, seek = 0;
	bool tsize = false, options = false, writing;
	uint64_t size = 0;
	const uint8_t *file = NULL;
	uint8_t oack[160];
	unsigned at;

	if (length < 4) {
		return;
	}
	tftp_requests++;
	writing = (m[0] << 8 | m[1]) == TFTP_WRQ;
	if ((m[0] << 8 | m[1]) != TFTP_RRQ && !writing) {
		tftp_error(tftp_port, node, port, 4, "bad op");
		return;
	}
	name = (const char *)m + 2;
	for (p = name; p < end && *p; p++) {
	}
	if (p >= end) {
		return;
	}
	mode = p + 1;
	for (p = mode; p < end && *p; p++) {
	}
	if (p >= end) {
		return;
	}
	p++;
	while (p < end) {
		const char *opt = p, *val;

		while (p < end && *p) {
			p++;
		}
		if (p >= end) {
			break;
		}
		val = ++p;
		while (p < end && *p) {
			p++;
		}
		if (p >= end) {
			break;
		}
		p++;
		options = true;
		if (strcmp(opt, "blksize") == 0) {
			blksize = (uint32_t)strtoul(val, NULL, 10);
		} else if (strcmp(opt, "wsize") == 0) {
			wsize = (uint32_t)strtoul(val, NULL, 10);
		} else if (strcmp(opt, "rsize") == 0) {
			rsize = (uint32_t)strtoul(val, NULL, 10);
		} else if (strcmp(opt, "seek") == 0) {
			seek = (uint32_t)strtoul(val, NULL, 10);
		} else if (strcmp(opt, "tsize") == 0) {
			tsize = true;
		}
	}
	blksize = blksize < 8 ? 8 : blksize > TFTP_MAX_BLOCK ? TFTP_MAX_BLOCK : blksize;
	wsize = wsize < 1 ? 1 : wsize > TFTP_MAX_WINDOW ? TFTP_MAX_WINDOW : wsize;
	for (unsigned i = 0; i < 4 && t == NULL; i++) {
		if (transfers[i].port == NULL) {
			t = &transfers[i];
		}
	}
	if (t == NULL) {
		tftp_error(tftp_port, node, port, 1, "busy");
		return;
	}
	bzero(t, sizeof(*t));
	strlcpy(t->name, name, sizeof(t->name));
	if (writing) {
		t->written = kalloc_data(TFTP_WRITE_MAX, Z_WAITOK);
		t->port = t->written == NULL ? NULL : qrtr_open(tftp_transfer_receive, t);
		if (t->port == NULL) {
			tftp_end(t);
			tftp_error(tftp_port, node, port, 1, "no port");
			return;
		}
		t->node = node;
		t->peer = port;
		t->writing = true;
		t->expected = 1;
		t->blksize = blksize;
		t->wsize = wsize;
		printf(TAG "tftp: modem writes \"%s\"\n", name);
		if (!options) {
			uint8_t ack[4] = { 0, TFTP_ACK, 0, 0 };

			qrtr_send(t->port, node, port, ack, sizeof(ack));
			return;
		}
		goto oack;
	}
	for (unsigned f = 0; f < 8 && file == NULL; f++) {
		if (tftp_files[f].data != NULL && strcmp(tftp_files[f].name, name) == 0) {
			file = tftp_files[f].data;
			size = tftp_files[f].size;
		}
	}
	if (file == NULL && tftp_file_of(name) != NULL) {
		file = hakios_image_file("fw", tftp_file_of(name), &size, NULL);
	}
	if (file == NULL) {
		printf(TAG "tftp: modem reads \"%s\": not here\n", name);
		tftp_error(tftp_port, node, port, 1, "file not found");
		return;
	}
	t->port = qrtr_open(tftp_transfer_receive, t);
	if (t->port == NULL) {
		tftp_error(tftp_port, node, port, 1, "no port");
		return;
	}
	t->node = node;
	t->peer = port;
	t->data = file;
	t->size = size;
	t->blksize = blksize;
	t->wsize = wsize;
	t->rsize = rsize;
	t->seek = seek;
	printf(TAG "tftp: modem reads \"%s\" (%llu bytes)\n", name, size);
	if (!options) {
		tftp_send_window(t, 0);
		return;
	}
oack:
	oack[0] = 0;
	oack[1] = TFTP_OACK;
	at = 2;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "blksize") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", blksize) + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "timeoutms") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "1000") + 1;
	if (tsize && !t->writing) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "tsize") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%llu", size) + 1;
	}
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "wsize") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", wsize) + 1;
	if (rsize != 0) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "rsize") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", rsize) + 1;
	}
	if (seek != 0) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "seek") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", seek) + 1;
	}
	qrtr_send(t->port, node, port, oack, at);
}

/* ---- service-registry locator (pd-mapper, service 64) -------------------------------------------------- */
static struct { char service[48]; char domain[48]; uint32_t instance; } smap[32];
static unsigned smaps;

static const char *
jsn_string(const char *from, const char *end, const char *key, char *out, size_t out_size)
{
	char quoted[48];
	const char *at;

	snprintf(quoted, sizeof(quoted), "\"%s\"", key);
	at = strnstr(from, quoted, (size_t)(end - from));
	if (at == NULL) {
		return NULL;
	}
	at += strlen(quoted);
	while (at < end && (*at == ':' || *at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')) {
		at++;
	}
	if (at >= end) {
		return NULL;
	}
	if (*at == '"') {
		const char *close = at + 1;

		while (close < end && *close != '"') {
			close++;
		}
		if (close >= end) {
			return NULL;
		}
		snprintf(out, out_size, "%.*s", (int)(close - at - 1), at + 1);
		return close + 1;
	}
	{
		const char *close = at;

		while (close < end && ((*close >= '0' && *close <= '9') || *close == '-')) {
			close++;
		}
		snprintf(out, out_size, "%.*s", (int)(close - at), at);
		return close;
	}
}

static void
servreg_load(void)
{
	static const char *const files[] = { "modemr.jsn", "modemuw.jsn", "adspr.jsn", "adsps.jsn", "adspua.jsn", "cdspr.jsn" };

	smaps = 0;
	for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
		char soc[24], domain[24], sub[24], inst[16], provider[24], service[24], full[48];
		uint64_t size = 0;
		const char *text = (const char *)hakios_image_file("fw", files[f], &size, NULL), *end, *at;

		if (text == NULL) {
			continue;
		}
		end = text + size;
		if (jsn_string(text, end, "soc", soc, sizeof(soc)) == NULL || jsn_string(text, end, "domain", domain, sizeof(domain)) == NULL ||
		    jsn_string(text, end, "subdomain", sub, sizeof(sub)) == NULL ||
		    jsn_string(text, end, "qmi_instance_id", inst, sizeof(inst)) == NULL) {
			continue;
		}
		snprintf(full, sizeof(full), "%s/%s/%s", soc, domain, sub);
		at = text;
		while (smaps < 32 && (at = jsn_string(at, end, "provider", provider, sizeof(provider))) != NULL &&
		    (at = jsn_string(at, end, "service", service, sizeof(service))) != NULL) {
			snprintf(smap[smaps].service, sizeof(smap[0].service), "%s/%s", provider, service);
			strlcpy(smap[smaps].domain, full, sizeof(smap[0].domain));
			smap[smaps].instance = (uint32_t)strtoul(inst, NULL, 10);
			smaps++;
		}
	}
	printf(TAG "servreg: %u services\n", smaps);
}

static void
servreg_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *v;
	uint8_t a[512];
	uint16_t message, size = 0;
	unsigned at = 14, found = 0;
	char wanted[64] = "";

	if (length < 7 || m[0] != QMI_REQUEST) {
		return;
	}
	message = (uint16_t)(m[3] | m[4] << 8);
	servreg_requests++;
	bzero(a, sizeof(a));
	a[0] = QMI_RESPONSE;
	a[1] = m[1];
	a[2] = m[2];
	a[3] = m[3];
	a[4] = m[4];
	a[7] = 2;
	a[8] = 4;
	if (message == 0x21) {
		v = qmi_tlv(m, length, 1, &size);
		if (v != NULL) {
			snprintf(wanted, sizeof(wanted), "%.*s", (int)(size < 63 ? size : 63), (const char *)v);
		}
		for (unsigned i = 0; i < smaps; i++) {
			found += strcmp(smap[i].service, wanted) == 0;
		}
		a[at++] = 0x10; a[at++] = 2; a[at++] = 0; a[at++] = (uint8_t)found; a[at++] = 0;
		a[at++] = 0x11; a[at++] = 2; a[at++] = 0; a[at++] = 1; a[at++] = 0;
		if (found != 0) {
			unsigned list = at;

			a[at++] = 0x12;
			at += 2;
			a[at++] = (uint8_t)found;
			for (unsigned i = 0; i < smaps && at + 64 < sizeof(a); i++) {
				if (strcmp(smap[i].service, wanted) != 0) {
					continue;
				}
				a[at++] = (uint8_t)strlen(smap[i].domain);
				memcpy(a + at, smap[i].domain, strlen(smap[i].domain));
				at += (unsigned)strlen(smap[i].domain);
				for (unsigned b = 0; b < 4; b++) {
					a[at++] = (uint8_t)(smap[i].instance >> (8 * b));
				}
				a[at++] = 0;
				at += 4;
			}
			a[list + 1] = (uint8_t)(at - list - 3);
			a[list + 2] = (uint8_t)((at - list - 3) >> 8);
		}
		printf(TAG "servreg: where is \"%s\": %u\n", wanted, found);
	} else {
		printf(TAG "servreg: request %#x\n", message);
		if (message != 0x24) {
			a[10] = 1;
			a[12] = 2;
		}
	}
	a[5] = (uint8_t)(at - 7);
	a[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(servreg_port, node, port, a, at);
}

/* ---- WLFW client (service 0x45) ------------------------------------------------------------------------ */
#define WLFW_SERVICE            0x45
static struct qport *wlfw_port;
static uint32_t wlfw_node, wlfw_srv;
static LCK_MTX_DECLARE(wlfw_lock, &jw_grp);
static uint8_t *wlfw_resp;
static size_t wlfw_resp_len;
static uint16_t wlfw_txn = 1, wlfw_wait_txn;
static volatile bool wlfw_got;
static volatile uint32_t wlfw_inds, ind_fw_ready, ind_msa_ready, ind_mem_ready, ind_init_done, ind_pin, ind_cal_done;

static void
wlfw_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t id;

	if (length < 7) {
		return;
	}
	id = (uint16_t)(m[3] | m[4] << 8);
	if (m[0] == QMI_INDICATION) {
		wlfw_inds++;
		printf(TAG "wlfw: indication %#x (%zu bytes)\n", id, length);
		switch (id) {
		case 0x21: ind_fw_ready++; break;
		case 0x2b: ind_msa_ready++; break;
		case 0x37: ind_mem_ready++; break;
		case 0x38: ind_init_done++; break;
		case 0x3e: ind_cal_done++; break;
		case 0x2c: {
			uint16_t s = 0;
			const uint8_t *v = qmi_tlv(m, length, 0x10, &s);

			ind_pin++;
			printf(TAG "wlfw: pin connect result pwr %#x phy %#x rf %#x\n", v && s == 4 ? le32(v) : 0,
			    (v = qmi_tlv(m, length, 0x11, &s)) && s == 4 ? le32(v) : 0, (v = qmi_tlv(m, length, 0x12, &s)) && s == 4 ? le32(v) : 0);
			break;
		}
		default: break;
		}
		return;
	}
	if (m[0] == QMI_RESPONSE) {
		lck_mtx_lock(&wlfw_lock);
		if ((uint16_t)(m[1] | m[2] << 8) == wlfw_wait_txn && wlfw_resp != NULL) {
			wlfw_resp_len = length < 2048 ? length : 2048;
			memcpy(wlfw_resp, m, wlfw_resp_len);
			wlfw_got = true;
		}
		lck_mtx_unlock(&wlfw_lock);
	}
}

/* A request, its answer (into wlfw_resp); returns the QMI result (0 ok), -1 on transport failure. */
static int
wlfw_call(uint16_t id, const uint8_t *tlvs, size_t tlv_len, const char *what)
{
	uint8_t *msg = kalloc_data(7 + tlv_len, Z_WAITOK);
	const uint8_t *r;
	uint16_t s = 0;
	int result = -1;

	if (msg == NULL) {
		return -1;
	}
	lck_mtx_lock(&wlfw_lock);
	wlfw_wait_txn = wlfw_txn++;
	wlfw_got = false;
	lck_mtx_unlock(&wlfw_lock);
	msg[0] = QMI_REQUEST;
	msg[1] = (uint8_t)wlfw_wait_txn;
	msg[2] = (uint8_t)(wlfw_wait_txn >> 8);
	msg[3] = (uint8_t)id;
	msg[4] = (uint8_t)(id >> 8);
	msg[5] = (uint8_t)tlv_len;
	msg[6] = (uint8_t)(tlv_len >> 8);
	if (tlv_len) {
		memcpy(msg + 7, tlvs, tlv_len);
	}
	if (qrtr_send(wlfw_port, wlfw_node, wlfw_srv, msg, 7 + tlv_len) != 0) {
		printf(TAG "wlfw: %s: not sent\n", what);
		kfree_data(msg, 7 + tlv_len);
		return -1;
	}
	kfree_data(msg, 7 + tlv_len);
	for (unsigned ms = 0; ms < 5000 && !wlfw_got; ms++) {
		sleep_ms(1);
	}
	if (!wlfw_got) {
		printf(TAG "wlfw: %s: no answer\n", what);
		return -1;
	}
	r = qmi_tlv(wlfw_resp, wlfw_resp_len, 2, &s);
	if (r != NULL && s == 4) {
		result = r[0] | r[1] << 8;
		printf(TAG "wlfw: %s: result %u error %u\n", what, result, r[2] | r[3] << 8);
		if (result != 0) {
			result = 0x10000 | (r[2] | r[3] << 8);
		}
	}
	return result;
}

static bool
wlfw_bring_up(void)
{
	uint8_t t[96];
	unsigned n = 0;
	uint8_t one = 1;
	uint32_t client = 0x4b4e454c;   /* "KNEL", icnss's own id */
	uint16_t s = 0;
	const uint8_t *v;
	uint32_t chip = 0, family = 0, board = 0xff, soc = 0;
	char foundry = 0;
	int r;

	wlfw_resp = kalloc_data(2048, Z_WAITOK | Z_ZERO);
	wlfw_port = qrtr_open(wlfw_receive, NULL);
	if (wlfw_resp == NULL || wlfw_port == NULL) {
		return false;
	}
	state(40, "wlfw service at %u:%u", wlfw_node, wlfw_srv);
	for (uint8_t tlv = 0x10; tlv <= 0x1a; tlv++) {
		if (tlv == 0x15) {
			n += put_tlv(t + n, tlv, &client, 4);
		} else if (tlv != 0x16 && tlv != 0x19) {        /* not request_mem, not rejuvenate */
			n += put_tlv(t + n, tlv, &one, 1);
		}
	}
	if ((r = wlfw_call(0x20, t, n, "ind_register")) != 0) {
		state(-41, "wlfw ind_register failed (%#x)", r);
		return false;
	}
	/* MSA: the fixed region; the firmware answers with its regions, which are assigned to MSS + WLAN (icnss2) */
	{
		uint64_t addr = MSA_REGION;
		uint32_t size = MSA_REGION_SIZE;

		n = 0;
		n += put_tlv(t + n, 1, &addr, 8);
		n += put_tlv(t + n, 2, &size, 4);
		if ((r = wlfw_call(0x2d, t, n, "msa_info")) != 0) {
			state(-42, "wlfw msa_info failed (%#x)", r);
			return false;
		}
		v = qmi_tlv(wlfw_resp, wlfw_resp_len, 3, &s);
		if (v != NULL) {
			char hex[128];
			int h = 0;

			for (unsigned i = 0; i < s && i < 40; i++) {
				h += snprintf(hex + h, sizeof(hex) - (size_t)h, "%02x", v[i]);
			}
			printf(TAG "wlfw: msa_info regions TLV (%u bytes): %s\n", s, hex);
		}
		for (unsigned i = 0; v != NULL && s >= 1 && i < v[0] && 1 + 13 * (i + 1) <= s; i++) {
			uint64_t ra;
			uint32_t rs;
			uint8_t secure = v[1 + 13 * i + 12];
			static const uint32_t src[] = { VMID_HLOS };
			static const uint32_t d3[] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE }, d2[] = { VMID_MSS_MSA, VMID_WLAN };

			memcpy(&ra, v + 1 + 13 * i, 8);
			memcpy(&rs, v + 1 + 13 * i + 8, 4);
			printf(TAG "wlfw: MSA region %u: %#llx+%#x secure %u\n", i, ra, rs, secure);
			if (ra < MSA_REGION || ra + rs > MSA_REGION + MSA_REGION_SIZE) {
				state(-43, "MSA region outside reserved memory");
				return false;
			}
			if (!assign(ra, rs, src, 1, secure ? d2 : d3, secure ? 2 : 3, "msa") &&
			    !assign(ra, rs, src, 1, d3, 3, "msa (3 vmids)")) {
				printf(TAG "wlfw: MSA region %u not assigned (already owned elsewhere?); continuing\n", i);
			}
		}
	}
	if ((r = wlfw_call(0x2e, NULL, 0, "msa_ready")) != 0) {
		state(-44, "wlfw msa_ready failed (%#x)", r);
		return false;
	}
	for (unsigned tries = 0;; tries++) {
		r = wlfw_call(0x24, NULL, 0, "cap");
		if (r == 0 || tries >= 10) {
			break;
		}
		sleep_ms(1000);
	}
	if (r != 0) {
		state(-45, "wlfw cap failed (%#x)", r);
		return false;
	}
	if ((v = qmi_tlv(wlfw_resp, wlfw_resp_len, 0x10, &s)) != NULL && s >= 8) {
		chip = le32(v);
		family = le32(v + 4);
	}
	if ((v = qmi_tlv(wlfw_resp, wlfw_resp_len, 0x11, &s)) != NULL && s >= 4) {
		board = le32(v);
	}
	if ((v = qmi_tlv(wlfw_resp, wlfw_resp_len, 0x12, &s)) != NULL && s >= 4) {
		soc = le32(v);
	}
	if ((v = qmi_tlv(wlfw_resp, wlfw_resp_len, 0x1d, &s)) != NULL && s >= 1) {
		foundry = (char)v[0];
	}
	{
		char fwver[40] = "";

		if ((v = qmi_tlv(wlfw_resp, wlfw_resp_len, 0x13, &s)) != NULL && s > 4) {
			snprintf(fwver, sizeof(fwver), "%.*s", (int)(s - 4 < 32 ? s - 4 : 32), (const char *)v + 4);
		}
		state(46, "cap chip %#x family %#x board %#x soc %#x foundry %c fw %s", chip, family, board, soc, foundry ? foundry : '?',
		    fwver);
	}
	/* BDF: the foundry-specific calibrated file if present in fw/, else bdwlan.bin / bdwlans.bin */
	{
		char name[32];
		uint64_t size = 0;
		const uint8_t *bdf = NULL;

		if (foundry) {
			snprintf(name, sizeof(name), "bdwlan%c.bin", foundry);
			bdf = hakios_image_file("fw", name, &size, NULL);
		}
		if (bdf == NULL) {
			strlcpy(name, "bdwlan.bin", sizeof(name));
			bdf = hakios_image_file("fw", name, &size, NULL);
		}
		if (bdf == NULL || size == 0 || size > (1u << 20)) {
			state(-47, "no BDF in fw/");
			return false;
		}
		uint8_t *seg = kalloc_data(6144 + 64, Z_WAITOK);
		uint32_t total = (uint32_t)size, sent = 0, segment = 0, file_id = 0;

		if (seg == NULL) {
			return false;
		}
		while (sent < total) {
			uint32_t chunk = total - sent > 6144 ? 6144 : total - sent;
			uint8_t end = sent + chunk == total, type = 0;
			uint16_t clen = (uint16_t)chunk;

			n = 0;
			n += put_tlv(seg + n, 0x01, &one, 1);
			n += put_tlv(seg + n, 0x10, &file_id, 4);
			n += put_tlv(seg + n, 0x11, &total, 4);
			n += put_tlv(seg + n, 0x12, &segment, 4);
			seg[n] = 0x13;
			seg[n + 1] = (uint8_t)(chunk + 2);
			seg[n + 2] = (uint8_t)((chunk + 2) >> 8);
			memcpy(seg + n + 3, &clen, 2);
			memcpy(seg + n + 5, bdf + sent, chunk);
			n += 5 + chunk;
			n += put_tlv(seg + n, 0x14, &end, 1);
			n += put_tlv(seg + n, 0x15, &type, 1);
			if ((r = wlfw_call(0x25, seg, n, segment == 0 ? "bdf (first)" : "bdf")) != 0) {
				kfree_data(seg, 6144 + 64);
				state(-47, "BDF %s segment %u failed (%#x)", name, segment, r);
				return false;
			}
			sent += chunk;
			segment++;
		}
		kfree_data(seg, 6144 + 64);
		printf(TAG "wlfw: BDF %s: %u bytes in %u segments\n", name, total, segment);
	}
	{
		uint8_t none = 0;

		n = put_tlv(t, 0x01, &none, 1);
		if ((r = wlfw_call(0x26, t, n, "cal_report")) != 0) {
			printf(TAG "wlfw: cal_report %#x (continuing)\n", r);
		}
	}
	{
		uint8_t mac[6] = { 0x02, 0x4a, 0x4d, 0x32, 0x31, 0x01 };
		char arg[32];

		if (PE_parse_boot_arg_str("jm21_wifi_mac", arg, sizeof(arg))) {
			unsigned b = 0;
			const char *p = arg;

			while (b < 6 && *p) {
				mac[b++] = (uint8_t)strtoul(p, NULL, 16);
				p = strchr(p, ':');
				if (p == NULL) {
					break;
				}
				p++;
			}
		}
		n = put_tlv(t, 0x10, mac, 6);
		if ((r = wlfw_call(0x33, t, n, "mac")) != 0) {
			printf(TAG "wlfw: mac %#x (continuing)\n", r);
		}
	}
	state(48, "BDF sent; waiting for FW_READY");
	for (unsigned ms = 0; ms < 20000 && ind_fw_ready == 0; ms++) {
		sleep_ms(1);
	}
	if (ind_fw_ready == 0) {
		state(-49, "no FW_READY in 20 s (inds %u msa %u mem %u init %u pin %u)", wlfw_inds, ind_msa_ready, ind_mem_ready,
		    ind_init_done, ind_pin);
		return false;
	}
	state(50, "WLAN FIRMWARE READY (chip %#x board %#x foundry %c)", chip, board, foundry ? foundry : '?');
	return true;
}

/* ---- jm21_wifi=2: Linux's ath10k (tools/mk-ath10k.sh) on top, through hakios.lx.c ---------------------- */
struct hakios_wifi_backend {
	bool (*radio_map)(uint64_t physical, uint64_t size, unsigned long long *address);
	bool (*dsp_is_up)(void);
	const uint8_t *(*driver_file)(const char *name, uint64_t *size);
	bool (*radio_address)(uint8_t *address);
	bool (*network)(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);
};
extern const struct hakios_wifi_backend *hakios_wifi_backend;
extern int hakios_lx_start(void);
extern void hakios_lx_net_early(void);
extern int hakios_lx_prepare(void);
static uint32_t jw_mode;

/* the loader bypasses SMMU SIDs 0x1a0/0x1a1 (src/usb.c wlan_dma_bypass): the radio sees physical addresses */
static bool be_radio_map(uint64_t physical, __unused uint64_t size, unsigned long long *address) { *address = physical; return true; }
static bool be_dsp_is_up(void) { return glink.qrtr.open; }
static const uint8_t *
be_driver_file(const char *name, uint64_t *size)
{
	const char *last = strrchr(name, '/');
	const uint8_t *d = NULL;

	last = last == NULL ? name : last + 1;
	if (strcmp(last, "firmware-5.bin") == 0) {
		d = hakios_image_file("fw", "ath10k-firmware-5.bin", size, NULL);
	} else if (strcmp(last, "board.bin") == 0) {
		d = hakios_image_file("fw", "bdwlans.bin", size, NULL);
	}
	printf(TAG "ath10k asks for %s: %s\n", name, d ? "given" : "not here");
	return d;
}
static bool
be_radio_address(uint8_t *mac)
{
	uint8_t m[6] = { 0x02, 0x4a, 0x4d, 0x32, 0x31, 0x01 };
	char arg[32];

	if (PE_parse_boot_arg_str("jm21_wifi_mac", arg, sizeof(arg))) {
		const char *p = arg;

		for (unsigned b = 0; b < 6 && p != NULL && *p; b++) {
			m[b] = (uint8_t)strtoul(p, NULL, 16);
			p = strchr(p, ':');
			p = p ? p + 1 : NULL;
		}
	}
	memcpy(mac, m, 6);
	return true;
}
/* fw/network of the RAM disk: the name on the first line, the passphrase on the second (never logged) */
static bool
be_network(uint8_t *name, unsigned *name_length, uint8_t *pass, unsigned *pass_length)
{
	uint64_t size = 0;
	const uint8_t *f = hakios_image_file("fw", "network", &size, NULL), *end, *line;
	unsigned sizes[2] = { *name_length, *pass_length }, lengths[2] = { 0, 0 };
	uint8_t *to[2] = { name, pass };

	if (f == NULL) {
		return false;
	}
	end = f + size;
	line = f;
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(to[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	*name_length = lengths[0];
	*pass_length = lengths[1];
	printf(TAG "network given: name %u bytes, passphrase %u bytes\n", lengths[0], lengths[1]);
	return lengths[0] != 0;
}
static const struct hakios_wifi_backend jm21_backend = {
	.radio_map = be_radio_map, .dsp_is_up = be_dsp_is_up, .driver_file = be_driver_file,
	.radio_address = be_radio_address, .network = be_network,
};

/* ---- the link thread and the bring-up -------------------------------------------------------------------- */
static volatile bool link_wanted;

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	while (link_wanted) {
		IOSleep(1);
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				show_words("MODEM FATAL", &words);
				show_crash_reason();
				jw_phase = -99;
			}
		}
		for (;;) {
			struct glink_packet *p;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			p = glink.qrtr.first;
			if (p != NULL) {
				glink.qrtr.first = p->next;
				glink.qrtr.last = glink.qrtr.first == NULL ? NULL : glink.qrtr.last;
			}
			lck_mtx_unlock(&glink_lock);
			if (p == NULL) {
				break;
			}
			qrtr_input(p->data, p->length);
			kfree_data(p, sizeof(*p) + p->length);
		}
		qrtr_fresh();
	}
	thread_terminate(current_thread());
}

static bool
supplies_on(void)
{
	/* rpm-smd "ldoa": swen=1, uv=; the WCN3990's: cx-mx L8 640 mV, xo L16 1.8 V, rfa L17 1.304 V, ch0 L23 3.312 V */
	static const struct { uint32_t id, uv; } ldo[] = { { 8, 640000 }, { 16, 1800000 }, { 17, 1304000 }, { 23, 3312000 } };
	bool all = true;

	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t kvp[] = { 0x6e657773, 4, 1, 0x7675, 4, ldo[i].uv };     /* "swen", "uv" */
		bool ok = hakios_jm21_rpm_request(0x616f646c, ldo[i].id, kvp, 6); /* "ldoa" */

		printf(TAG "RPM ldoa%u %u uV: %s\n", ldo[i].id, ldo[i].uv, ok ? "ACK" : "FAILED");
		all = all && ok;
		IOSleep(2);
	}
	return all;
}

static void
wifi_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t delay_s = 12, fixed = 0;
	struct smp2p w;
	thread_t t;
	uint64_t began;
	unsigned loaded;

	PE_parse_boot_argn("jm21_wifi_delay", &delay_s, sizeof(delay_s));
	state(1, "waiting %u s and for /dev/rmmc0", delay_s);
	IOSleep(delay_s * 1000);
	for (unsigned i = 0; i < 120; i++) {
		vnode_t vp = NULLVP;

		if (rootvnode != NULLVP && vnode_lookup("/dev/rmmc0", 0, &vp, vfs_context_kernel()) == 0) {
			vnode_put(vp);
			break;
		}
		IOSleep(500);
	}
	smem = (volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	apcs = ml_io_map(APCS_IPC & ~0xfffu, 0x1000) + (APCS_IPC & 0xfffu);
	mutex_lock = ml_io_map(TCSR_MUTEX_LOCK3, 0x1000);
	if (smem == NULL || apcs == 0 || mutex_lock == 0) {
		state(-2, "map failed");
		return;
	}
	{
		uint32_t cl = 0, size = 0, p = smem_partition(SMEM_US, SMEM_MPSS, &cl, &size);

		if (p == 0) {
			state(-2, "no SMEM partition 0<->1");
			return;
		}
		printf(TAG "SMEM partition 0<->1 at %#x size %#x cacheline %u\n", SMEM_PHYS + p, size, cl);
	}
	state(3, "WCN3990 supplies through RPM");
	if (!supplies_on()) {
		printf(TAG "some supply votes failed; continuing\n");
	}
	loaded = rmtfs_load();
	servreg_load();
	state(4, "rmtfs partitions %u/4, servreg %u", loaded, smaps);
	if (!our_item_setup()) {
		state(-5, "SMP2P item");
		return;
	}
	if (!rmtfs_buffer_setup()) {
		printf(TAG "rmtfs buffer not shared; modem will get no buffer\n");
	}
	/* ports and servers before the modem starts (wifi.md: services 14/64/4096 must be ready) */
	rmtfs_port = qrtr_open(rmtfs_receive, NULL);
	tftp_port = qrtr_open(tftp_receive, NULL);
	servreg_port = qrtr_open(servreg_receive, NULL);
	our_servers[0].service = 14; our_servers[0].instance = 1; our_servers[0].port = rmtfs_port->number;
	our_servers[1].service = 4096; our_servers[1].instance = 1; our_servers[1].port = tftp_port->number;
	our_servers[2].service = 64; our_servers[2].instance = 257; our_servers[2].port = servreg_port->number;
	state(6, "loading modem (PAS 4)");
	if (!mss_boot()) {
		state(-6, "modem PAS boot failed");
		return;
	}
	began = now_us();
	state(7, "modem out of reset; watching SMP2P");
	bool ready = false;
	for (unsigned ms = 0; ms < 15000; ms++) {
		if (dsp_words(&w)) {
			uint32_t s = dsp_state(&w);

			dsp_listen(&w);
			if (s & DSP_FATAL) {
				show_words("modem", &w);
				show_crash_reason();
				state(-7, "modem fatal at %llu ms", (now_us() - began) / 1000);
				return;
			}
			if (s & DSP_READY) {
				show_words("modem ready", &w);
				ready = true;
				break;
			}
		}
		IOSleep(1);
	}
	if (!ready) {
		show_crash_reason();
		state(-7, "modem did not say ready in 15 s");
		return;
	}
	state(8, "modem ready after %llu ms; GLINK", (now_us() - began) / 1000);
	for (unsigned ms = 0; ms < 5000 && !glink_find(ms >= 1000); ms += 10) {
		IOSleep(10);
	}
	if (glink.descriptor == 0 || glink.ours == 0 || glink.theirs == 0) {
		state(-8, "GLINK items missing (desc %#x ours %#x theirs %#x)", glink.descriptor, glink.ours, glink.theirs);
		return;
	}
	printf(TAG "glink: desc %#x tx %#x/%u rx %#x/%u\n", glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.trace = 40;
	glink.up = true;
	glink_say(GLINK_VERSION, 1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = true;
	if (kernel_thread_start(link_thread, NULL, &t) != KERN_SUCCESS) {
		state(-8, "no link thread");
		return;
	}
	thread_deallocate(t);
	for (unsigned ms = 0; ms < 10000 && !glink.qrtr.open && !glink.broken; ms++) {
		IOSleep(1);
	}
	if (!glink.qrtr.open) {
		state(-9, "IPCRTR not open (broken %u heard %u)", glink.broken, glink.heard);
		return;
	}
	state(10, "IPCRTR open; waiting for WLFW (0x45)");
	PE_parse_boot_argn("jm21_wifi_fixed", &fixed, sizeof(fixed));
	for (unsigned ms = 0; ms < 60000; ms += 20) {
		if (qrtr_find(WLFW_SERVICE, &wlfw_node, &wlfw_srv)) {
			break;
		}
		if (jw_phase == -99) {
			return;
		}
		IOSleep(20);
	}
	printf(TAG "after link: rmtfs %u req (%u r/%u w), tftp %u req %u served, servreg %u, qrtr in %u out %u; services: %s\n",
	    rmtfs_requests, rmtfs_reads, rmtfs_writes, tftp_requests, tftp_served, servreg_requests, qrtr_in, qrtr_out, jw_services);
	if (!qrtr_find(WLFW_SERVICE, &wlfw_node, &wlfw_srv)) {
		state(-10, "no WLFW service in 60 s; services %s", jw_services);
		return;
	}
	if (jw_mode >= 2) {
		int e;

		state(60, "WLFW at %u:%u; starting ath10k (it speaks WLFW itself)", wlfw_node, wlfw_srv);
		e = hakios_lx_start();
		state(e == 0 ? 70 : -60, "ath10k start returned %d", e);
		if (e == 0 && PE_parse_boot_argn("wifi_station", &fixed, sizeof(fixed)) && fixed) {
			/* the station is offered to Apple80211 (wifid): join fw/network as the standing network, as redwood's join_given() */
			static struct hakios_sta_join what;
			unsigned nl = sizeof(what.ssid), pl = sizeof(what.passphrase);

			bzero(&what, sizeof(what));
			if (be_network(what.ssid, &nl, what.passphrase, &pl)) {
				what.ssid_length = (uint8_t)nl;
				what.passphrase_length = (uint8_t)pl;
				what.security = HAKIOS_STA_WPA2_PSK;
				e = hakios_sta_ask_join_standing(&what);
				bzero(&what, sizeof(what));
				for (unsigned w = 0; e == 0 && hakios_sta_busy() && w < 300; w++) {
					IOSleep(100);
				}
				state(e == 0 ? 80 : -80, "standing join asked: %d; busy %d", e, hakios_sta_busy());
			}
		}
		return;
	}
	if (!wlfw_bring_up()) {
		return;
	}
	printf(TAG "phase 1 complete; data path (ath10k snoc CE @0xc800000, SMMU SID 0x1a0) not implemented yet\n");
}

void hakios_jm21_wifi_init(void);
void
hakios_jm21_wifi_init(void)
{
	uint32_t on = 0, ios = 0;
	thread_t t;

	if (!PE_parse_boot_argn("jm21_ios", &ios, sizeof(ios)) || !ios ||
	    !PE_parse_boot_argn("jm21_wifi", &on, sizeof(on)) || !on) {
		return;
	}
	jw_mode = on;
	if (jw_mode >= 2) {
		hakios_wifi_backend = &jm21_backend;
		hakios_qmi_set_transport(&jm21_qrtr_ops);
		printf(TAG "radio memory reserved early: %d\n", hakios_lx_prepare());
		hakios_lx_net_early();          /* en0 + the IOKit entries before wifid looks (hakios.lx.c) */
	}
	if (kernel_thread_start(wifi_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}
