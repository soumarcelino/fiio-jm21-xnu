/* FiiO JM21 battery: CellWise CW221X fuel gauge (I2C 0x64) and SG Micro SGM41513
 * charger (I2C 0x1a) on QUP0 SE0 (TLMM GPIO0 SDA, GPIO1 SCL), from the Android
 * dtbo (fragment@28 qupv3_se0_i2c; fiio_charge_manager fcm-fuel-gauge=cw221X-bat,
 * fcm-chargers=sgm41513-charger). The PMIC (PM6125) is not involved: no SPMI.
 * Opt-in: jm21_ios=1 jm21_battery=1. Poll period jm21_battery_s (default 20 s).
 *
 * READ ONLY on the gauge. On the charger the only write, with jm21_charge=1, is
 * REG01 bit4 CHG_CONFIG, exactly Android's sgm4154x_enable_charger/disable
 * (update_bits(0x01, 0x10, on)): Android's sgm4154x_charger_shutdown() clears it
 * at every shutdown/reboot and nothing sets it again before XNU, so without this
 * the JM21 never charges under XNU. Currents, voltages, timers, termination and
 * the CE pin (GPIO66, only read) keep Android's last values. Software JEITA is
 * replaced by a narrower gate: enable only at 20.0..43.0 C, disable below 18.0,
 * above 45.0 C, or after 3 polls without a gauge reading. The only write is to
 * the TLMM: GPIO0/1 are switched from function qup0 to plain GPIO, open drain
 * emulated (never driven high; the bus pull-ups do that), like the GT967 bus.
 *
 * Gauge scaling from Android cw221X_fuel_gauge.ko (disassembly, see
 * fiio-xnu-poc-display/docs/jm21-battery.md):
 *   0x02/0x03 VCELL BE16, mV = raw*5/16;   0x04 SOC %, 0x05 SOC /256
 *   0x06 TEMP, 0.1 C = raw*5 - 400 (0 = no NTC); 0x0e/0x0f CURRENT s16 BE,
 *   uA = raw*800 if fw_version(0xab)&0xc0, else raw*3815/2 (0 if fw 0);
 *   0xa4/0xa5 CYCLE BE16 /16; 0xa6 SOH %; 0x00 chip id.  Words are read twice
 *   until equal, as the Android driver does.
 * Charger (BQ2560x-like map, sgm4154x_charger.ko get_state): 0x08 status
 *   VBUS_STAT 7:5, CHRG_STAT 4:3 (0 none,1 pre,2 fast,3 done), PG bit2;
 *   0x0a bit7 VBUS_GD; 0x0b part number. 0x09 (fault, clear-on-read) is not read. */
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

#define SDA 0
#define SCL 1
#define FG 0x64
#define CHG 0x1a

static int phase, soc = -1, soc_frac, voltage_mv = -1, current_ma, temp_dc = -4000, cycles, soh;
static int chip_id = -1, fw_version = -1, chg_part = -1, chg_status = -1, chg_ctrl = -1, chg_vbus_gd = -1;
static int charger_present, charging, polls, i2c_failures, current_raw;
static int charge_enabled = -1, charge_writes, ce_level = -1, charge_ctl;
static int chg_regs[0x0c] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_phase, CTLFLAG_RD | CTLFLAG_LOCKED, &phase, 0,
    "JM21 battery: 1 map, 2 pins, 3 gauge id, 4 charger id, 5 polling, negative failed");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED, &soc, 0, "CW221X SOC percent");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_soc_256, CTLFLAG_RD | CTLFLAG_LOCKED, &soc_frac, 0, "CW221X SOC fraction /256");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_mv, CTLFLAG_RD | CTLFLAG_LOCKED, &voltage_mv, 0, "CW221X cell mV");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_ma, CTLFLAG_RD | CTLFLAG_LOCKED, &current_ma, 0, "CW221X current mA (Android sign)");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_current_raw, CTLFLAG_RD | CTLFLAG_LOCKED, &current_raw, 0, "CW221X raw current register");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_temp_dc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_dc, 0, "CW221X temperature, 0.1 C");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED, &cycles, 0, "CW221X cycle count");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_soh, CTLFLAG_RD | CTLFLAG_LOCKED, &soh, 0, "CW221X state of health percent");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chip_id, CTLFLAG_RD | CTLFLAG_LOCKED, &chip_id, 0, "CW221X chip id");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_fw, CTLFLAG_RD | CTLFLAG_LOCKED, &fw_version, 0, "CW221X fw version");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_part, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_part, 0, "SGM41513 REG0B");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_status, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_status, 0, "SGM41513 REG08");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_ctrl, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_ctrl, 0, "SGM41513 REG01");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_reg00, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_regs[0], 0, "SGM41513 REG00 (HIZ, IINDPM)");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_reg02, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_regs[2], 0, "SGM41513 REG02 (ICHG)");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_reg04, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_regs[4], 0, "SGM41513 REG04 (VREG)");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_reg05, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_regs[5], 0, "SGM41513 REG05 (WDT, EN_TERM)");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_enabled, CTLFLAG_RD | CTLFLAG_LOCKED, &charge_enabled, 0, "SGM41513 REG01 CHG_CONFIG as last read");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_chg_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &charge_writes, 0, "CHG_CONFIG writes done");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_ce_gpio66, CTLFLAG_RD | CTLFLAG_LOCKED, &ce_level, 0, "charger CE pin level (0 = enabled)");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_vbus_gd, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_vbus_gd, 0, "SGM41513 REG0A bit7");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_external, CTLFLAG_RD | CTLFLAG_LOCKED, &charger_present, 0, "input power present");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED, &charging, 0, "charger in pre/fast charge");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_polls, CTLFLAG_RD | CTLFLAG_LOCKED, &polls, 0, "successful polls");
SYSCTL_INT(_kern, OID_AUTO, jm21_battery_i2c_failures, CTLFLAG_RD | CTLFLAG_LOCKED, &i2c_failures, 0, "failed I2C transfers");

static vm_offset_t tlmm;
static bool bus_error;
#define CFG(n) (tlmm + 0x1000UL * (n))
#define IO(n) (CFG(n) + 4)
static uint32_t rd(vm_offset_t p) { return *(volatile uint32_t *)p; }
static void wr(vm_offset_t p, uint32_t v) { *(volatile uint32_t *)p = v; __asm__ volatile("dsb sy" ::: "memory"); }
static void us(unsigned n) {
	uint64_t end;
	nanoseconds_to_absolutetime((uint64_t)n * NSEC_PER_USEC, &end);
	end += mach_absolute_time();
	while (mach_absolute_time() < end) {}
}
/* open drain: high = released (input, pull-up enabled), low = output 0 */
static void line(unsigned n, bool high) { wr(IO(n), 0); wr(CFG(n), high ? 3 : 3 | (1u << 9)); }
static bool level(unsigned n) { return rd(IO(n)) & 1; }
static void clock_high(void) {
	line(SCL, true);
	for (unsigned n = 0; n < 200 && !level(SCL); n++) us(5);     /* clock stretching */
	if (!level(SCL)) bus_error = true;
	us(5);
}
static void start(void) { line(SDA, true); clock_high(); line(SDA, false); us(5); line(SCL, false); us(5); }
static void stop(void) { line(SDA, false); us(5); clock_high(); line(SDA, true); us(5); }
static bool bit(bool v) { line(SDA, v); us(5); clock_high(); bool r = level(SDA); line(SCL, false); us(5); return r; }
static bool put(uint8_t v) { for (int n = 7; n >= 0; n--) bit((v >> n) & 1); return !bit(true) && !bus_error; }
static uint8_t get(bool more) { uint8_t v = 0; for (unsigned n = 0; n < 8; n++) v = (uint8_t)((v << 1) | bit(true)); bit(!more); return v; }
static void recover(void) {
	/* a slave left holding SDA low: clock it out (9 pulses), then STOP */
	line(SDA, true);
	for (unsigned n = 0; n < 9 && !level(SDA); n++) { line(SCL, false); us(5); line(SCL, true); us(5); }
	bus_error = false; stop();
}
static bool xfer(uint8_t addr, uint8_t reg, uint8_t *p, unsigned count) {
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		bus_error = false;
		start();
		bool ok = put(addr << 1) && put(reg);
		if (ok) { start(); ok = put((addr << 1) | 1); }
		if (ok) for (unsigned n = 0; n < count; n++) p[n] = get(n + 1 < count);
		stop();
		if (ok && !bus_error) return true;
		i2c_failures++;
		recover();
		us(200);
	}
	return false;
}
static bool byte(uint8_t addr, uint8_t reg, int *out) { uint8_t v; if (!xfer(addr, reg, &v, 1)) return false; *out = v; return true; }
static bool put_byte(uint8_t addr, uint8_t reg, uint8_t v) {
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		bus_error = false; start();
		bool ok = put(addr << 1) && put(reg) && put(v);
		stop();
		if (ok && !bus_error) return true;
		i2c_failures++; recover(); us(200);
	}
	return false;
}
/* REG01 bit4 only, read back; other bits as read (WD_RST/OTG are 0 in them). */
static void set_charge(bool on, const char *why) {
	int r1;
	if (!byte(CHG, 0x01, &r1)) return;
	charge_enabled = (r1 >> 4) & 1;
	if (charge_enabled == (int)on) return;
	uint8_t v = (uint8_t)((r1 & ~0x70) | (on ? 0x10 : 0));
	if (put_byte(CHG, 0x01, v) && byte(CHG, 0x01, &r1)) {
		charge_writes++;
		charge_enabled = (r1 >> 4) & 1;
		chg_ctrl = chg_regs[1] = r1;
	}
	printf("jm21-battery: charging %s (%s): REG01 now %#x\n", on ? "ENABLED" : "disabled", why, r1);
}
static int missed;
static void charge_policy(bool fresh) {
	if (!charge_ctl) return;
	missed = fresh ? 0 : missed + 1;
	if (missed >= 3) { set_charge(false, "no gauge reading"); return; }
	if (!fresh || temp_dc == -4000) return;
	if (temp_dc < 180 || temp_dc > 450) set_charge(false, "battery temperature out of 18..45 C");
	else if (temp_dc >= 200 && temp_dc <= 430) set_charge(true, "battery 20..43 C");
}
/* the Android driver reads a word up to three times until two reads agree */
static bool word(uint8_t reg, uint16_t *out) {
	uint8_t a[2], b[2];
	if (!xfer(FG, reg, a, 2)) return false;
	for (unsigned n = 0; n < 2; n++) {
		delay_for_interval(4, NSEC_PER_MSEC);
		if (!xfer(FG, reg, b, 2)) return false;
		if (a[0] == b[0] && a[1] == b[1]) break;
		a[0] = b[0]; a[1] = b[1];
	}
	*out = (uint16_t)((b[0] << 8) | b[1]);
	return true;
}

static void report(void) {
	struct hakios_battery_state b = { .size = sizeof(b) };
	b.percent_hundredths = soc * 100 + soc_frac * 100 / 256;
	if (b.percent_hundredths > 10000) b.percent_hundredths = 10000;
	b.voltage_uv = voltage_mv * 1000;
	/* hakios_battery_state: current > 0 while the battery takes charge. */
	b.current_ua = current_ma * 1000;
	b.temperature_hundredths = temp_dc * 10;
	b.present = 1;
	b.external = charger_present;
	/* POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	b.status = !charger_present ? 2 : charging ? 1 : ((chg_status >> 3) & 3) == 3 ? 4 : 3;
	b.full_uah = 2400 * 1000;       /* DT cw,capacity = 0x960 mAh (cw,capacity2 0xc1c for battery id 2) */
	b.design_uah = 2400 * 1000;
	b.charge_uah = b.full_uah / 10000 * b.percent_hundredths;
	b.cycles = cycles;
	b.usb_type = !charger_present ? 0 : (((chg_status >> 5) & 7) == 1 ? 1 : ((chg_status >> 5) & 7) == 2 ? 3 :
	    ((chg_status >> 5) & 7) == 3 ? 2 : 0);
	b.usb_voltage_uv = charger_present ? 5000000 : 0;
	b.usb_current_ua = charger_present ? 500000 : 0;
	hakios_battery_report(&b);
}

static bool poll(void) {
	uint16_t w; int v, st, gd;
	bool ok = true;
	if (word(0x02, &w)) voltage_mv = w * 5 / 16; else ok = false;
	uint8_t s[2];
	if (xfer(FG, 0x04, s, 2)) { soc = s[0] > 100 ? 100 : s[0]; soc_frac = s[0] >= 100 ? 0 : s[1]; } else ok = false;
	if (byte(FG, 0x06, &v)) { if (v) temp_dc = v * 5 - 400; } else ok = false;
	if (word(0x0e, &w)) {
		int raw = (int16_t)w;
		current_raw = raw;
		long ua = (fw_version & 0xc0) ? (long)raw * 800 : fw_version ? (long)raw * 3815 / 2 : 0;
		current_ma = (int)(ua / 1000);
	} else ok = false;
	if (word(0xa4, &w)) cycles = w / 16;
	if (byte(FG, 0xa6, &v)) soh = v;
	if (byte(CHG, 0x08, &st)) chg_status = st; else ok = false;
	if (byte(CHG, 0x0a, &gd)) chg_vbus_gd = (gd >> 7) & 1;
	if (chg_status >= 0) {
		unsigned vbus = (chg_status >> 5) & 7, chrg = (chg_status >> 3) & 3;
		charger_present = (vbus != 0 && vbus != 7) || (chg_status & 4) || chg_vbus_gd == 1;
		charging = charger_present && (chrg == 1 || chrg == 2);
	}
	return ok && soc >= 0 && voltage_mv > 0;
}

static void battery_thread(__unused void *p, __unused wait_result_t r) {
	uint32_t period = 20;
	PE_parse_boot_argn("jm21_battery_s", &period, sizeof(period));
	if (period < 5) period = 5;
	if (period > 600) period = 600;
	phase = 1;
	tlmm = ml_io_map(0x00500000UL, 0x100000UL);
	if (!tlmm) goto failed;
	phase = 2;
	printf("jm21-battery: gpio0 cfg=%#x io=%#x, gpio1 cfg=%#x io=%#x (were qup0)\n",
	    rd(CFG(SDA)), rd(IO(SDA)), rd(CFG(SCL)), rd(IO(SCL)));
	line(SDA, true); line(SCL, true); us(50);
	if (!level(SCL)) { printf("jm21-battery: SCL stuck low\n"); goto failed; }
	if (!level(SDA)) { printf("jm21-battery: SDA low, recovering bus\n"); recover(); }
	phase = 3;
	if (!byte(FG, 0x00, &chip_id)) { printf("jm21-battery: no CW221X at 0x64\n"); goto failed; }
	byte(FG, 0xab, &fw_version);
	printf("jm21-battery: CW221X chip_id=%#x fw=%#x\n", chip_id, fw_version);
	phase = 4;
	if (byte(CHG, 0x0b, &chg_part)) {
		for (unsigned i = 0; i < 0x0c; i++) if (i != 9) { chg_regs[i] = -1; byte(CHG, (uint8_t)i, &chg_regs[i]); }
		chg_ctrl = chg_regs[1];
		if (chg_ctrl >= 0) charge_enabled = (chg_ctrl >> 4) & 1;
		printf("jm21-battery: SGM41513 REG0B=%#x regs 00..0b (09 skipped): %x %x %x %x %x %x %x %x %x - %x %x\n",
		    chg_part, chg_regs[0], chg_regs[1], chg_regs[2], chg_regs[3], chg_regs[4], chg_regs[5],
		    chg_regs[6], chg_regs[7], chg_regs[8], chg_regs[10], chg_regs[11]);
	} else printf("jm21-battery: no SGM41513 at 0x1a (charger state unknown)\n");
	PE_parse_boot_argn("jm21_charge", &charge_ctl, sizeof(charge_ctl));
	ce_level = rd(IO(66)) & 1;
	printf("jm21-battery: CE gpio66 cfg=%#x level=%d; CHG_CONFIG=%d; charge control %s\n", rd(CFG(66)), ce_level,
	    chg_regs[1] >= 0 ? (chg_regs[1] >> 4) & 1 : -1, charge_ctl ? "on (REG01 bit4 only)" : "off (read only)");
	if (chg_part < 0) charge_ctl = 0;
	phase = 5;
	printf("jm21-battery: polling every %u s\n", period);
	for (unsigned tick = 0;; tick++) {
		bool fresh = poll();
		if (fresh) polls++;
		ce_level = rd(IO(66)) & 1;
		charge_policy(fresh);
		if (fresh) report();
		if (tick < 3 || tick % 15 == 0)
			printf("jm21-battery: soc=%d.%02d%% V=%d mV I=%d mA (raw %d) T=%d.%d C cyc=%d soh=%d chg08=%#x gd=%d ext=%d charging=%d chg_en=%d fails=%d\n",
			    soc, soc_frac * 100 / 256, voltage_mv, current_ma, current_raw, temp_dc / 10, (temp_dc < 0 ? -temp_dc : temp_dc) % 10,
			    cycles, soh, chg_status, chg_vbus_gd, charger_present, charging, charge_enabled, i2c_failures);
		delay_for_interval(period * 1000, NSEC_PER_MSEC);
	}
failed:
	phase = -phase;
	printf("jm21-battery: disabled at phase %d\n", phase);
	thread_terminate(current_thread());
}

void hakios_jm21_battery_init(void) {
	unsigned on = 0, jm21 = 0; thread_t thread;
	if (!PE_parse_boot_argn("jm21_ios", &jm21, sizeof(jm21)) || !jm21 ||
	    !PE_parse_boot_argn("jm21_battery", &on, sizeof(on)) || !on) return;
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
