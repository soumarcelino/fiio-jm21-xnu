/*
 * The FiiO JM21's WCN3990 (Qualcomm khaje) for Linux's ath10k snoc: what ath10k_snoc_probe() takes from a
 * device tree and a platform bus, given here; everything else is snoc.c's own code (this file is
 * included at the end of lx10k_snoc.c, so it sees snoc.c's static functions).
 *
 * The firmware runs in the modem; the kernel's hakios.jm21_wifi.c starts it and is the QRTR transport of the
 * QMI helpers, so ath10k's qmi.c speaks WLFW itself. Copy engines: 0xc800000, physical addresses (the loader
 * bypasses SMMU SIDs 0x1a0/0x1a1). No interrupts: a thread runs the driver's NAPI poll every millisecond.
 */
#include <lx_os.h>
#include "lx.h"

static void crc32_tab_make(void);

void lx10k_unexpected(const char *what);
void
lx10k_unexpected(const char *what)
{
	lx_tell("lx10k: %s was called, which the JM21 bus does not provide\n", what);
}

/* ---- small things the ath11k port did not need ------------------------------------------------- */
const unsigned char rfc1042_header[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

void
DELAY(int us)
{
	unsigned long long until = lx_os_nanoseconds() + (unsigned long long)us * 1000ull;

	while (lx_os_nanoseconds() < until) {
	}
}

int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	unsigned long long ms = ((unsigned long long)sbt * 1000ull) >> 32;

	lx_pause_ms(ms == 0 ? 1 : (unsigned)ms);
	return (0);
}

void
nanouptime(struct timespec *ts)
{
	unsigned long long ns = lx_os_nanoseconds();

	ts->tv_sec = (time_t)(ns / 1000000000ull);
	ts->tv_nsec = (long)(ns % 1000000000ull);
}

/* Linux's printk extensions that XNU's vsnprintf lacks: %pV (a va_format), %pM (a MAC), %pa/%pad (an address). */
static int
lx10k_vformat(char *out, size_t size, const char *fmt, va_list ap)
{
	size_t at = 0;
	char spec[16];

	while (*fmt && at + 1 < size) {
		if (*fmt != '%') {
			out[at++] = *fmt++;
			continue;
		}
		if (fmt[1] == '%') {
			out[at++] = '%';
			fmt += 2;
			continue;
		}
		if (fmt[1] == 'p' && (fmt[2] == 'V' || fmt[2] == 'M' || fmt[2] == 'm' || fmt[2] == 'a' || fmt[2] == 'K' ||
		    fmt[2] == 'S' || fmt[2] == 's')) {
			const void *ptr = va_arg(ap, const void *);

			if (fmt[2] == 'V') {
				const struct va_format *v = ptr;
				va_list inner;

				va_copy(inner, *v->va);
				at += (size_t)lx10k_vformat(out + at, size - at, v->fmt, inner);
				va_end(inner);
				fmt += 3;
			} else if (fmt[2] == 'M' || fmt[2] == 'm') {
				const uint8_t *m = ptr;

				at += (size_t)snprintf(out + at, size - at, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
				fmt += 3;
			} else if (fmt[2] == 'a') {
				const uint64_t *a = ptr;

				at += (size_t)snprintf(out + at, size - at, "%#llx", (unsigned long long)*a);
				fmt += fmt[3] == 'd' || fmt[3] == 'p' ? 4 : 3;
			} else {
				at += (size_t)snprintf(out + at, size - at, "%p", ptr);
				fmt += 3;
				while (*fmt == 'R' || *fmt == 'F') fmt++;
			}
			continue;
		}
		/* one ordinary conversion: copy the spec, hand it to vsnprintf with the right argument */
		{
			size_t n = 0;
			const char *f = fmt;
			char conv;

			spec[n++] = *f++;
			while (*f && strchr("-+ #0123456789.*hlzjtL", *f) && n < sizeof(spec) - 2) {
				spec[n++] = *f++;
			}
			conv = *f;
			if (conv == 0) {
				break;
			}
			spec[n++] = *f++;
			spec[n] = 0;
			bool longlong = (strchr(spec, 'l') && strchr(strchr(spec, 'l') + 1, 'l')) || strchr(spec, 'z') || strchr(spec, 'j') || strchr(spec, 't') ||
			    (strchr(spec, 'l') && sizeof(long) == 8);
			int star = strchr(spec, '*') ? va_arg(ap, int) : -1;
			if (conv == 's') {
				const char *str = va_arg(ap, const char *);
				at += (size_t)(star >= 0 ? snprintf(out + at, size - at, spec, star, str ? str : "(null)") :
				    snprintf(out + at, size - at, spec, str ? str : "(null)"));
			} else if (conv == 'p') {
				at += (size_t)snprintf(out + at, size - at, "%p", va_arg(ap, void *));
			} else if (conv == 'c') {
				at += (size_t)snprintf(out + at, size - at, spec, va_arg(ap, int));
			} else if (longlong) {
				long long v = va_arg(ap, long long);
				char s2[20];
				size_t k = 0;
				/* normalise the length to ll for XNU */
				for (size_t i = 0; spec[i]; i++) {
					if (spec[i] != 'l' && spec[i] != 'z' && spec[i] != 'j' && spec[i] != 't') s2[k++] = spec[i];
					if (spec[i+1] == conv && k < sizeof(s2) - 4) { s2[k++] = 'l'; s2[k++] = 'l'; s2[k++] = conv; break; }
				}
				s2[k] = 0;
				at += (size_t)(star >= 0 ? snprintf(out + at, size - at, s2, star, v) : snprintf(out + at, size - at, s2, v));
			} else {
				int v = va_arg(ap, int);
				at += (size_t)(star >= 0 ? snprintf(out + at, size - at, spec, star, v) : snprintf(out + at, size - at, spec, v));
			}
			if (at >= size) at = size - 1;
			fmt = f;
		}
	}
	out[at < size ? at : size - 1] = 0;
	return (int)at;
}

int
device_printf(device_t dev, const char *fmt, ...)
{
	char text[400];
	va_list ap;

	va_start(ap, fmt);
	lx10k_vformat(text, sizeof(text), fmt, ap);
	va_end(ap);
	lx_tell("ath10k: %s", text);
	return (0);
}

struct sk_buff *
linuxkpi_skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
	struct sk_buff *n = dev_alloc_skb(skb->len + skb_headroom(skb));

	if (n == NULL) {
		return (NULL);
	}
	skb_reserve(n, skb_headroom(skb));
	memcpy(skb_put(n, skb->len), skb->data, skb->len);
	memcpy(n->cb, skb->cb, sizeof(n->cb));
	return (n);
}

/* ---- the device tree: one node, the properties ath10k asks about ---------------------------------- */
static const struct property jm21_props[] = {
	{ "qcom,msa-fixed-perm", 0, "" },
	{ "compatible", 18, "qcom,wcn3990-wifi" },
};

const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	for (int i = 0; np != NULL && i < np->nproperties; i++) {
		if (strcmp(np->properties[i].name, name) == 0) {
			if (lenp != NULL) {
				*lenp = np->properties[i].length;
			}
			return (&np->properties[i]);
		}
	}
	return (NULL);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	const struct property *p = of_find_property(np, name, lenp);

	return (p == NULL ? NULL : p->value);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	const struct property *p = of_find_property(np, name, NULL);

	if (p == NULL || p->length == 0) {
		return (-EINVAL);
	}
	*out = p->value;
	return (0);
}

bool
of_machine_is_compatible(const char *compatible)
{
	return (false);
}

int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	return (-ENODEV);	/* the MSA region is set up below, by hand */
}

int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
	return (lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT);
}

#undef qmi_send_request
int qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure);
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	int r;

	lx_tell("lx10k: qmi request %#x (largest %zu)\n", msg_id, largest);
	if (msg_id == QMI_WLFW_WLAN_MODE_REQ_V01) {
		lx_tell("lx10k: the firmware is asked for mode %u (0: mission, 4: off)\n",
		    ((const struct wlfw_wlan_mode_req_msg_v01 *)structure)->mode);
	}
	r = qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure);
	lx_tell("lx10k: qmi request %#x sent: %d\n", msg_id, r);
	return (r);
}

/* ---- interrupts: none; the NAPI instances are polled ---------------------------------------------- */
#define LX10K_NAPI 4
static struct napi_struct *napis[LX10K_NAPI];
static volatile bool napi_on[LX10K_NAPI];
static volatile bool poll_wanted, poll_runs;
static unsigned long long polls;

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_handler,
    unsigned long flags, const char *name, void *arg)
{
	return (0);
}
int lkpi_enable_irq(unsigned int irq) { return (0); }
void lkpi_disable_irq(unsigned int irq) { }
void lkpi_free_irq(unsigned int irq, void *device) { }
void lkpi_devm_free_irq(struct device *dev, unsigned int irq, void *device) { }

static int
napi_index(struct napi_struct *n)
{
	for (int i = 0; i < LX10K_NAPI; i++) {
		if (napis[i] == n) {
			return (i);
		}
	}
	return (-1);
}

void
linuxkpi_init_dummy_netdev(struct net_device *nd)
{
}

void
linuxkpi_netif_napi_add(struct net_device *nd, struct napi_struct *n, int (*poll)(struct napi_struct *, int))
{
	n->dev = nd;
	n->poll = poll;
	n->budget = 64;
	for (int i = 0; i < LX10K_NAPI; i++) {
		if (napis[i] == NULL) {
			napis[i] = n;
			break;
		}
	}
}
void
linuxkpi_netif_napi_del(struct napi_struct *n)
{
	int i = napi_index(n);

	if (i >= 0) {
		napi_on[i] = false;
		napis[i] = NULL;
	}
}
bool linuxkpi_napi_schedule_prep(struct napi_struct *n) { return (true); }
void linuxkpi___napi_schedule(struct napi_struct *n) { }
bool linuxkpi_napi_schedule(struct napi_struct *n) { return (true); }
void linuxkpi_napi_reschedule(struct napi_struct *n) { }
bool linuxkpi_napi_complete_done(struct napi_struct *n, int r) { return (true); }
bool linuxkpi_napi_complete(struct napi_struct *n) { return (true); }
void
linuxkpi_napi_enable(struct napi_struct *n)
{
	int i = napi_index(n);

	if (i >= 0) {
		napi_on[i] = true;
	}
}
void
linuxkpi_napi_disable(struct napi_struct *n)
{
	int i = napi_index(n);

	if (i >= 0) {
		napi_on[i] = false;
	}
	lx_pause_ms(3);
}
void linuxkpi_napi_synchronize(struct napi_struct *n) { lx_pause_ms(3); }

struct net_device *
linuxkpi_alloc_netdev(size_t len, const char *name, uint32_t flags, void (*setup)(struct net_device *))
{
	struct net_device *nd = kzalloc(sizeof(*nd) + len, GFP_KERNEL);

	return (nd);
}
void linuxkpi_free_netdev(struct net_device *nd) { kfree(nd); }

static struct ath10k *poll_ar;

static void
poll_thread(void *argument)
{
	unsigned long long served = 0;

	while (poll_wanted) {
		for (int i = 0; i < LX10K_NAPI; i++) {
			struct napi_struct *n = napis[i];

			if (n != NULL && napi_on[i] && n->poll != NULL) {
				if (poll_ar != NULL) {
					for (unsigned ce = 0; ce < poll_ar->hw_values->ce_count; ce++) {
						ath10k_ce_per_engine_service(poll_ar, ce);
					}
				}
				n->poll(n, n->budget);
				served++;
			}
		}
		polls++;
		if (polls % 30000 == 0 && polls <= 60000) {
			lx_tell("lx10k: poll %llu, served %llu, napi %p on %d\n", polls, served, napis[0], napi_on[0]);
		}
		lx_pause_ms(1);
	}
	poll_runs = false;
}

/* ---- mac80211 queueing: the station calls ops->tx directly, so no queues to schedule ----------------- */
uint8_t
linuxkpi_ieee80211_get_tid(struct ieee80211_hdr *hdr, bool nonqos_ok)
{
	if (!ieee80211_is_data_qos(hdr->frame_control)) {
		return (IEEE80211_NONQOS_TID);
	}
	return (*ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK);
}
struct sk_buff *linuxkpi_ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq) { return (NULL); }
void
linuxkpi_ieee80211_txq_get_depth(struct ieee80211_txq *txq, unsigned long *frames, unsigned long *bytes)
{
	if (frames) *frames = 0;
	if (bytes) *bytes = 0;
}
void linuxkpi_ieee80211_stop_queue(struct ieee80211_hw *hw, int q) { }
void linuxkpi_ieee80211_wake_queue(struct ieee80211_hw *hw, int q) { }
void linuxkpi_ieee80211_txq_schedule_start(struct ieee80211_hw *hw, uint8_t ac) { }
struct ieee80211_txq *linuxkpi_ieee80211_next_txq(struct ieee80211_hw *hw, uint8_t ac) { return (NULL); }
void linuxkpi_ieee80211_schedule_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq, bool withoutpkts) { }

uint32_t
linuxkpi_cfg80211_calculate_bitrate(struct rate_info *rate)
{
	return (rate == NULL ? 0 : rate->legacy);
}

/* ---- the probe -------------------------------------------------------------------------------------- */
#define JM21_CE_BASE    0x0c800000ull
#define JM21_CE_SIZE    0x00800000ul
#define JM21_MSA_BASE   0x51900000ull
#define JM21_MSA_SIZE   0x00100000ul

static struct ath10k *driver;

int
lx_start(void)
{
	static struct platform_device pdev;
	static struct device_node node;
	struct ath10k_snoc *ar_snoc;
	struct ath10k *ar;
	unsigned long long began;
	int ret;

	crc32_tab_make();
	if (lx_prepare() != 0) {
		lx_tell("lx10k: no thread, or no memory\n");
		return (ENOMEM);
	}
	ath10k_debug_mask = lx_os_option("wifi_debug");
	ath10k_frame_mode = ATH10K_HW_TXRX_ETHERNET;
	node.name = "wifi";
	node.full_name = "/soc/wifi@c800000";
	node.properties = jm21_props;
	node.nproperties = sizeof(jm21_props) / sizeof(jm21_props[0]);
	pdev.name = "ath10k_snoc";
	pdev.dev.of_node = &node;
	pdev.dev.kobj.name = "wifi";

	ar = ath10k_core_create(sizeof(*ar_snoc), &pdev.dev, ATH10K_BUS_SNOC, ATH10K_HW_WCN3990, &ath10k_snoc_hif_ops);
	if (ar == NULL) {
		return (ENOMEM);
	}
	ar_snoc = ath10k_snoc_priv(ar);
	poll_ar = ar;
	ar_snoc->dev = &pdev;
	platform_set_drvdata(&pdev, ar);
	ar_snoc->ar = ar;
	ar_snoc->ce.bus_ops = &ath10k_snoc_bus_ops;
	ar->ce_priv = &ar_snoc->ce;
	ath10k_snoc_quirks_init(ar);
	ar_snoc->mem_pa = JM21_CE_BASE;
	ar_snoc->mem = lx_os_map_registers(JM21_CE_BASE, JM21_CE_SIZE);
	if (ar_snoc->mem == NULL) {
		lx_tell("lx10k: the copy engines cannot be mapped\n");
		return (ENXIO);
	}
	for (int i = 0; i < CE_COUNT; i++) {
		ar_snoc->ce_irqs[i].irq_line = 1000 + i;
	}
	if ((ret = ath10k_snoc_setup_resource(ar)) != 0) {
		lx_tell("lx10k: setup_resource: %d\n", ret);
		return (-ret);
	}
	ar_snoc->num_vregs = 0;         /* the supplies: voted on by hakios.jm21_wifi.c through RPM */
	ar_snoc->num_clks = 0;
	ar->msa.paddr = JM21_MSA_BASE;
	ar->msa.mem_size = JM21_MSA_SIZE;
	ar->msa.vaddr = lx_os_map_registers(JM21_MSA_BASE, JM21_MSA_SIZE);
	ar_snoc->use_tz = true;
	poll_wanted = poll_runs = true;
	if (lx_os_thread("wifi-napi", poll_thread, NULL) != 0) {
		poll_wanted = poll_runs = false;
		return (ENOMEM);
	}
	if ((ret = ath10k_qmi_init(ar, JM21_MSA_SIZE)) != 0) {
		lx_tell("lx10k: qmi_init: %d\n", ret);
		return (-ret);
	}
	lx_tell("lx10k: ath10k snoc set up (WCN3990); waiting for the firmware and the driver's registration\n");
	began = lx_os_nanoseconds();
	while (!test_bit(ATH10K_SNOC_FLAG_REGISTERED, &ar_snoc->flags) && lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(20);
	}
	/* ath10k_core_register() runs on a work item: wait for mac80211 registration (the station's radio) */
	while (!test_bit(ATH10K_FLAG_CORE_REGISTERED, &ar->dev_flags) && lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(20);
	}
	lx_pause_ms(500);
	lx_tell("lx10k: %llu ms: registered %d, state %d, MAC %pM, fw %s\n", (lx_os_nanoseconds() - began) / 1000000ull,
	    test_bit(ATH10K_SNOC_FLAG_REGISTERED, &ar_snoc->flags) ? 1 : 0, ar->state, ar->mac_addr, ar->hw->wiphy->fw_version);
	if (!test_bit(ATH10K_SNOC_FLAG_REGISTERED, &ar_snoc->flags)) {
		return (ETIMEDOUT);
	}
	driver = ar;
	if (lx_os_option("wifi_station")) {
		if ((ret = lx_station_offer()) == 0) {
			return (0);
		}
	} else if (lx_os_option("wifi_join")) {
		if ((ret = lx_station_join()) == 0) {
			return (0);
		}
	} else {
		ret = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
	}
	return (ret);
}

int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

void
lx_stop(void)
{
	if (driver == NULL) {
		return;
	}
	lx_station_withdraw();
	lx_station_leave();
	driver = NULL;
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (driver == NULL ? ENXIO : lx_station_send(frame, length));
}

/* ---- what the link left as stubs, done ---------------------------------------------------------------- */
void *
lkpi_devres_alloc(void (*release)(struct device *, void *), size_t size, gfp_t gfp)
{
	return (kzalloc(size, gfp));            /* never given back: the driver lives as long as the kernel */
}
void lkpi_devres_add(struct device *dev, void *p) { }
void lkpi_devm_kmalloc_release(struct device *dev, void *p) { }

struct linuxkpi_ieee80211_regdomain *
lkpi_get_linuxkpi_ieee80211_regdomain(size_t n)
{
	return (kzalloc(sizeof(struct linuxkpi_ieee80211_regdomain) + n * sizeof(struct ieee80211_reg_rule), GFP_KERNEL));
}

int
eventratecheck(struct timeval *last, int *count, int max)
{
	return (1);
}

uint32_t crc32_tab[256];
static void
crc32_tab_make(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++) {
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		}
		crc32_tab[i] = c;
	}
}
