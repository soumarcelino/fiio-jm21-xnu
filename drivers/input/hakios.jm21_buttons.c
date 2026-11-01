/* FiiO JM21 physical keys -> /dev/hakios-touch button records (same queue as
 * rwtouch "button"), consumed by libhakios in backboardd as HID consumer page
 * 0x0C: Power 0x30, Volume Up 0xE9, Volume Down 0xEA.
 * Opt-in: jm21_ios=1 jm21_buttons=1.
 *
 * Wiring, from the original Android DT (vendor_boot dtb + dtbo overlays 12/22/31/40):
 *   Power        PM6125 SID0 PON @0x800 (qcom,qpnp-power-on, pon_1 type0 KEY_POWER),
 *                gen2 PON, RT_STS 0x810 bit0 = KPDPWR_N held.
 *   Volume Up    TLMM GPIO96 (WEST), gpio-keys active low, pull-up, debounce 15 ms.
 *   Volume Down  TLMM GPIO97 (WEST), gpio-keys active low, pull-up, debounce 15 ms.
 * RESIN is NOT volume down on this device.
 *
 * Normal input only reads the PMIC (observer channel, EE0). Final platform
 * poweroff also selects PS_HOLD shutdown on the verified EE0 write channel. The TLMM pins are configured as
 * input+pull-up only if the boot firmware left them otherwise (logged). Polling
 * every 10 ms; a change is accepted after two equal looks (>=10-20 ms, like the
 * 15 ms DT debounce). A key held at start is ignored until seen released. */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

static int phase, held_mask, presses[3], pon_subtype, spmi_failures;
static int spmi_timeouts, spmi_max_wait_us, spmi_max_hold_us;
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_phase, CTLFLAG_RD | CTLFLAG_LOCKED, &phase, 0,
    "JM21 keys: 1 map, 2 PON channel, 3 PON id, 4 TLMM pins, 5 polling, negative failed");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_held, CTLFLAG_RD | CTLFLAG_LOCKED, &held_mask, 0,
    "JM21 keys held now: bit0 power, bit1 volume up, bit2 volume down");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_power, CTLFLAG_RD | CTLFLAG_LOCKED, &presses[0], 0, "JM21 power presses");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_volup, CTLFLAG_RD | CTLFLAG_LOCKED, &presses[1], 0, "JM21 volume up presses");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_voldown, CTLFLAG_RD | CTLFLAG_LOCKED, &presses[2], 0, "JM21 volume down presses");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_pon_subtype, CTLFLAG_RD | CTLFLAG_LOCKED, &pon_subtype, 0, "PM6125 PON subtype");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_spmi_failures, CTLFLAG_RD | CTLFLAG_LOCKED, &spmi_failures, 0, "failed PON reads");

/* Measure the actual critical section before changing its timeout or locking. */
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_spmi_timeouts, CTLFLAG_RD | CTLFLAG_LOCKED, &spmi_timeouts, 0, "PON reads without completion before deadline");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_spmi_max_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &spmi_max_wait_us, 0, "maximum PON SPMI lock wait in microseconds");
SYSCTL_INT(_kern, OID_AUTO, jm21_buttons_spmi_max_hold_us, CTLFLAG_RD | CTLFLAG_LOCKED, &spmi_max_hold_us, 0, "maximum PON SPMI lock hold in microseconds");

static uint32_t rd(vm_offset_t p) { return *(volatile uint32_t *)p; }
static void wr(vm_offset_t p, uint32_t v) { *(volatile uint32_t *)p = v; __asm__ volatile("dsb sy" ::: "memory"); }

static vm_offset_t tlmm, pon_obs, pon_write;
#define CFG(n) (tlmm + 0x1000UL * (n))
#define IO(n) (CFG(n) + 4)

/* SPMI v5 observer read of one PON register (EE0). */
static bool pon_read(unsigned reg, uint8_t *value) {
	uint64_t end, now, waiting, acquired, released, ns;
	uint32_t status = 0;
	nanoseconds_to_absolutetime(2000 * NSEC_PER_USEC, &end);
	waiting = mach_absolute_time();
	hakios_spmi_lock();
	acquired = mach_absolute_time();
	/* Contention must not consume the hardware transfer timeout. */
	end += acquired;
	wr(pon_obs, (1u << 27) | (reg << 4));
	do { status = rd(pon_obs + 8); now = mach_absolute_time(); } while (!(status & 1) && now < end);
	uint32_t data = rd(pon_obs + 0x18);
	released = mach_absolute_time();
	hakios_spmi_unlock();
	/* Conversion and statistics stay outside the non-preemptible section. */
	absolutetime_to_nanoseconds(acquired - waiting, &ns);
	if (ns / NSEC_PER_USEC > (uint64_t)spmi_max_wait_us)
		spmi_max_wait_us = (int)(ns / NSEC_PER_USEC);
	absolutetime_to_nanoseconds(released - acquired, &ns);
	if (ns / NSEC_PER_USEC > (uint64_t)spmi_max_hold_us)
		spmi_max_hold_us = (int)(ns / NSEC_PER_USEC);
	if (!(status & 1)) spmi_timeouts++;
	if ((status & 15) != 1) return false;
	*value = (uint8_t)data;
	return true;
}

/* PM6125 gen2 PS_HOLD sequence, matching Linux pm8941_reboot_notify:
 * disable, wait >=3 sleep-clock cycles, select shutdown (4), re-enable.
 * Called only after filesystem shutdown, never from normal input polling. */
static bool pon_set(unsigned reg, uint8_t value) {
 uint32_t status = 0;
 hakios_spmi_lock();
 wr(pon_write + 0x10, value);
 wr(pon_write, reg << 4);
 for (unsigned i = 0; i < 200; i++) {
  status = rd(pon_write + 8);
  if (status & 1) break;
  delay(5);
 }
 hakios_spmi_unlock();
 return (status & 15) == 1;
}

bool hakios_jm21_prepare_poweroff(void) {
 uint8_t rev = 0, ctl = 0, enable = 0, check = 0;
 if (!pon_obs || !pon_write || (pon_subtype != 4 && pon_subtype != 5) ||
     !pon_read(1, &rev) || !pon_read(0x5a, &ctl)) return false;
 unsigned enable_reg = rev == 0 ? 0x5a : 0x5b;
 if (!pon_read(enable_reg, &enable)) return false;
 printf("JM21: PON shutdown rev=%u ctl=%#x enable=%#x\n", rev, ctl, enable);
 if (!pon_set(enable_reg, enable & ~0x80u)) return false;
 delay(100);
 uint8_t shutdown_ctl = (ctl & ~0x0fu) | 4;
 if (enable_reg == 0x5a) shutdown_ctl &= ~0x80u;
 bool ok = pon_set(0x5a, shutdown_ctl);
 uint8_t armed = enable_reg == 0x5a ? shutdown_ctl | 0x80u : enable | 0x80u;
 /* Always attempt to re-enable PS_HOLD even if the type write failed. */
 bool enabled = pon_set(enable_reg, armed);
 ok = ok && enabled && pon_read(0x5a, &check) && (check & 0xf) == 4;
 ok = ok && pon_read(enable_reg, &check) && (check & 0x80);
 if (!ok) {
  pon_set(0x5a, ctl);
  pon_set(enable_reg, enable);
 }
 return ok;
}

static struct key {
	const char *name; uint32_t record;
	bool usable, live, last, down;
} keys[3] = {
	{ "power", HAKIOS_BUTTON_LOCK },
	{ "volume up", HAKIOS_BUTTON_VOLUME_UP },
	{ "volume down", HAKIOS_BUTTON_VOLUME_DOWN },
};

/* Power as Home (default; boot-arg jm21_power_home=0 restores Lock): the iPhone 8 screen class
 * (artwork-device-subtype 1334) leaves apps by the Home button, and this phone has none. A short
 * press is Home at release; held 500 ms it becomes Lock, held as long as the key is. */
static uint32_t power_home = 1;
static uint64_t power_since;
static bool power_lock_sent;

static uint64_t ms_since(uint64_t t) { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t, &ns); return ns / 1000000; }

static void look(unsigned i, bool pressed) {
	struct key *k = &keys[i];
	bool steady = pressed == k->last;
	k->last = pressed;
	if (!k->usable || !steady) return;
	if (!k->live) {
		if (!pressed) { k->live = true; printf("jm21-buttons: %s released, believed from now on\n", k->name); }
		return;
	}
	if (i == 0 && power_home && k->down && pressed && !power_lock_sent && ms_since(power_since) >= 500) {
		power_lock_sent = true;
		hakios_touch_post(HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_LOCK, 0, 0);
	}
	if (pressed == k->down) return;
	if (i == 0 && power_home) {
		k->down = pressed;
		if (pressed) { presses[0]++; held_mask |= 1; power_since = mach_absolute_time(); power_lock_sent = false; return; }
		held_mask &= ~1;
		if (power_lock_sent) {
			hakios_touch_post(HAKIOS_BUTTON_UP, HAKIOS_BUTTON_LOCK, 0, 0);
		} else {
			hakios_touch_post(HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_HOME, 0, 0);
			hakios_touch_post(HAKIOS_BUTTON_UP, HAKIOS_BUTTON_HOME, 0, 0);
		}
		printf("jm21-buttons: power released: %s\n", power_lock_sent ? "lock" : "home");
		return;
	}
	k->down = pressed;
	if (pressed) { presses[i]++; held_mask |= 1 << i; } else held_mask &= ~(1 << i);
	printf("jm21-buttons: %s %s (presses=%d)\n", k->name, pressed ? "PRESSED" : "released", presses[i]);
	hakios_touch_post(pressed ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, k->record, 0, 0);
}

static bool pin_input(unsigned n) {
	uint32_t cfg = rd(CFG(n)), io = rd(IO(n));
	printf("jm21-buttons: gpio%u cfg=%#x io=%#x\n", n, cfg, io);
	/* want function 0 (bits5:2), OE=0 (bit9), pull-up (bits1:0 == 3) */
	if ((cfg & 0x23c) != 0 || (cfg & 3) != 3) {
		wr(CFG(n), 3);
		printf("jm21-buttons: gpio%u set to gpio input pull-up, cfg now %#x\n", n, rd(CFG(n)));
		if ((rd(CFG(n)) & 0x23f) != 3) return false;
	}
	return true;
}

static void buttons_thread(__unused void *p, __unused wait_result_t r) {
	PE_parse_boot_argn("jm21_power_home", &power_home, sizeof(power_home));
	unsigned failures_in_row = 0;
	phase = 1;
	vm_offset_t core = ml_io_map(0x01c40000, 0x2000), owners = ml_io_map(0x01c0a000, 0x1000);
	tlmm = ml_io_map(0x00500000UL, 0x100000UL);
	if (!core || !owners || !tlmm) goto failed;
	uint32_t version = rd(core);
	printf("jm21-buttons: SPMI arbiter version %#x\n", version);
	phase = 2;
	unsigned apid = 512;
	if (version >= 0x50000000 && version < 0x70000000) {
		for (unsigned i = 0; i < 512; i++) {
			uint32_t map = rd(core + 0x900 + 4 * i);
			if (map && ((map >> 8) & 0xfff) == 0x008 && (rd(owners + 0x700 + 4 * i) & 7) == 0) { apid = i; break; }
		}
	}
	phase = 3;
	if (apid < 512) {
		uint8_t type = 0, sub = 0, st = 0;
		pon_obs = ml_io_map(0x03e00000UL + 0x80UL * apid, 0x80);
		if (pon_obs && pon_read(4, &type) && pon_read(5, &sub) && pon_read(0x10, &st)) {
			pon_subtype = sub;
			if (type == 0x01 && (sub == 4 || sub == 5))
				pon_write = ml_io_map(0x01e00000UL + 0x10000UL * apid, 0x1000);
			printf("jm21-buttons: PM6125 PON APID=%u type=%#x subtype=%#x rt_status=%#x\n", apid, type, sub, st);
			if (type == 0x01 && (sub == 4 || sub == 5)) {
				keys[0].usable = true;
				keys[0].last = st & 1;
			} else printf("jm21-buttons: unknown PON layout, power key left alone\n");
		} else printf("jm21-buttons: PON read failed, power key left alone\n");
	} else printf("jm21-buttons: no EE0 channel for PON (SID0 0x08), power key left alone\n");
	phase = 4;
	keys[1].usable = pin_input(96);
	keys[2].usable = pin_input(97);
	if (keys[1].usable) keys[1].last = !(rd(IO(96)) & 1);
	if (keys[2].usable) keys[2].last = !(rd(IO(97)) & 1);
	if (!keys[0].usable && !keys[1].usable && !keys[2].usable) goto failed;
	for (unsigned i = 0; i < 3; i++) {
		keys[i].live = keys[i].usable && !keys[i].last;
		if (keys[i].usable && keys[i].last) printf("jm21-buttons: %s reads held at start; ignored until released\n", keys[i].name);
	}
	phase = 5;
	printf("jm21-buttons: polling every 10 ms:%s%s%s\n", keys[0].usable ? " power" : "",
	    keys[1].usable ? " volume-up" : "", keys[2].usable ? " volume-down" : "");
	for (;;) {
		if (keys[0].usable) {
			uint8_t st;
			if (pon_read(0x10, &st)) { failures_in_row = 0; look(0, st & 1); }
			else {
				spmi_failures++;
				if (++failures_in_row >= 500) {
					printf("jm21-buttons: PON reads keep failing, power key disabled\n");
					if (keys[0].down) hakios_touch_post(HAKIOS_BUTTON_UP, HAKIOS_BUTTON_LOCK, 0, 0);
					keys[0].usable = false; held_mask &= ~1;
				}
			}
		}
		if (keys[1].usable) look(1, !(rd(IO(96)) & 1));
		if (keys[2].usable) look(2, !(rd(IO(97)) & 1));
		delay_for_interval(10, NSEC_PER_MSEC);
	}
failed:
	phase = -phase;
	printf("jm21-buttons: disabled at phase %d\n", phase);
	thread_terminate(current_thread());
}

void hakios_jm21_buttons_init(void) {
	unsigned on = 0, jm21 = 0; thread_t thread;
	if (!PE_parse_boot_argn("jm21_ios", &jm21, sizeof(jm21)) || !jm21 ||
	    !PE_parse_boot_argn("jm21_buttons", &on, sizeof(on)) || !on) return;
	if (kernel_thread_start(buttons_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
