/*
 * Copyright (C) 2026
 *
 * MT7621 ethernet port LED mode control.
 *
 * The port LEDs of the E8820V2 are driven by the embedded MT7530
 * switch.  GSW LED behavior registers (0x3000-0x301C, one per LED
 * pin, 4 bits per switch port) select the function that drives each
 * pin, including hardware blink modes.  By rewriting them we make all
 * port LEDs blink with a mode-specific rate:
 *
 *   - web failsafe (httpd):  fast blink
 *   - serial interruption:   slow blink
 *   - normal boot:           untouched (default link/activity)
 *
 * The MT7530 is soft-reset by every eth_start(), which also resets the
 * LED registers.  To survive that, the current mode is remembered and
 * re-applied at the end of mt7530_setup() via mt7621_portled_reapply().
 */

#include <common.h>
#include <command.h>
#include <dm.h>
#include <dm/uclass-id.h>
#include <miiphy.h>
#include <watchdog.h>
#include <linux/mdio.h>

#include <mt7621_portled.h>

/* Number of RJ45 ports (LED pins 0-4): 4 LAN + 1 WAN */
#define PORTLED_NUM_LEDS	5

/* GSW LED behavior register for LED pin n */
#define MT7530_LED_REG(n)	(0x3000 + (n) * 4)
#define MT7530_NUM_PORTS	8

/*
 * Per-port PHY LED control register (MMD device 0x1f, reg 0x24).
 * 0x4000 = force off (as noted by the MediaTek driver comment).
 */
#define MT7530_PHY_LED_REG	0x24

/*
 * Function codes written to the LED behavior registers (4 bits per
 * port).  The exact blink-rate mapping of these codes is not covered
 * by public documentation: calibrate on the real board with
 * `portled sweep`, then adjust the two values below.
 */
#define PORTLED_FN_FAST		0x9
#define PORTLED_FN_SLOW		0xd

static enum mt7621_portled_mode cur_mode = PORTLED_MODE_NONE;
static u32 led_default[PORTLED_NUM_LEDS];
static int defaults_captured;

static struct mii_dev *portled_get_bus(void)
{
	struct udevice *dev;

	/* Probing the ethernet device also registers its MDIO bus */
	if (uclass_get_device(UCLASS_ETH, 0, &dev)) {
		debug("%s: no ethernet device\n", __func__);
		return NULL;
	}

	return mdio_get_current_dev();
}

/* MT7530 page-addressed register access over MDIO (phy addr 0x1f) */
static int portled_gsw_write(struct mii_dev *bus, u32 reg, u32 val)
{
	int ret;
	u16 page = (reg >> 6) & 0x3ff;
	u16 idx = (reg >> 2) & 0xf;

	ret = bus->write(bus, 0x1f, MDIO_DEVAD_NONE, 0x1f, page);
	if (ret)
		return ret;

	ret = bus->write(bus, 0x1f, MDIO_DEVAD_NONE, idx, val & 0xffff);
	if (ret)
		return ret;

	return bus->write(bus, 0x1f, MDIO_DEVAD_NONE, 0x10 | idx,
		val >> 16);
}

static int portled_gsw_read(struct mii_dev *bus, u32 reg, u32 *val)
{
	int lo, hi, ret;
	u16 page = (reg >> 6) & 0x3ff;
	u16 idx = (reg >> 2) & 0xf;

	ret = bus->write(bus, 0x1f, MDIO_DEVAD_NONE, 0x1f, page);
	if (ret)
		return ret;

	lo = bus->read(bus, 0x1f, MDIO_DEVAD_NONE, idx);
	if (lo < 0)
		return lo;

	hi = bus->read(bus, 0x1f, MDIO_DEVAD_NONE, 0x10 | idx);
	if (hi < 0)
		return hi;

	*val = ((u32)hi << 16) | ((u32)lo & 0xffff);

	return 0;
}

static void portled_capture_defaults(struct mii_dev *bus)
{
	int i;

	if (defaults_captured)
		return;

	for (i = 0; i < PORTLED_NUM_LEDS; i++) {
		u32 v = 0;

		if (!portled_gsw_read(bus, MT7530_LED_REG(i), &v))
			led_default[i] = v;
	}

	defaults_captured = 1;
}

/* Write the same function code to every port nibble of LED0-4 */
static int portled_apply_fn(struct mii_dev *bus, u8 fn)
{
	int i, ret;
	u32 nv = 0;
	int b;

	portled_capture_defaults(bus);

	for (b = 0; b < MT7530_NUM_PORTS; b++)
		nv |= (u32)fn << (b * 4);

	for (i = 0; i < PORTLED_NUM_LEDS; i++) {
		ret = portled_gsw_write(bus, MT7530_LED_REG(i), nv);
		if (ret)
			return ret;
	}

	return 0;
}

static int portled_restore(struct mii_dev *bus)
{
	int i;

	if (!defaults_captured)
		return 0;

	for (i = 0; i < PORTLED_NUM_LEDS; i++)
		portled_gsw_write(bus, MT7530_LED_REG(i), led_default[i]);

	return 0;
}

int mt7621_portled_set(enum mt7621_portled_mode mode)
{
	struct mii_dev *bus;
	int ret = 0;

	/* Nothing to do on the normal boot path: avoid probing ethernet */
	if (mode == PORTLED_MODE_NONE && !defaults_captured &&
	    cur_mode == PORTLED_MODE_NONE)
		return 0;

	cur_mode = mode;

	bus = portled_get_bus();
	if (!bus)
		return -ENODEV;

	switch (mode) {
	case PORTLED_MODE_HTTPD:
		ret = portled_apply_fn(bus, PORTLED_FN_FAST);
		break;
	case PORTLED_MODE_CONSOLE:
		ret = portled_apply_fn(bus, PORTLED_FN_SLOW);
		break;
	default:
		ret = portled_restore(bus);
		break;
	}

	if (ret)
		debug("%s: apply failed (%d)\n", __func__, ret);

	return ret;
}

void mt7621_portled_reapply(struct mii_dev *bus)
{
	if (!bus || cur_mode == PORTLED_MODE_NONE)
		return;

	if (cur_mode == PORTLED_MODE_HTTPD)
		portled_apply_fn(bus, PORTLED_FN_FAST);
	else
		portled_apply_fn(bus, PORTLED_FN_SLOW);
}

/* Serial interruption: a key stopped the bootmenu countdown */
void board_bootmenu_interrupted(void)
{
	mt7621_portled_set(PORTLED_MODE_CONSOLE);
}

/* Debug / calibration command */
static int do_portled(cmd_tbl_t *cmdtp, int flag, int argc,
	char *const argv[])
{
	struct mii_dev *bus;
	int i;

	bus = portled_get_bus();
	if (!bus) {
		printf("No ethernet/MDIO bus available\n");
		return 1;
	}

	if (argc == 1) {
		printf("Current mode: %d (0=none 1=httpd 2=console)\n",
			cur_mode);
		for (i = 0; i < PORTLED_NUM_LEDS; i++) {
			u32 v = 0;

			portled_gsw_read(bus, MT7530_LED_REG(i), &v);
			printf("LED%d (0x%04x): 0x%08x\n", i,
				MT7530_LED_REG(i), v);
		}
		return 0;
	}

	if (!strcmp(argv[1], "httpd")) {
		mt7621_portled_set(PORTLED_MODE_HTTPD);
		printf("All port LEDs: fast blink (mode httpd)\n");
	} else if (!strcmp(argv[1], "console")) {
		mt7621_portled_set(PORTLED_MODE_CONSOLE);
		printf("All port LEDs: slow blink (mode console)\n");
	} else if (!strcmp(argv[1], "restore")) {
		mt7621_portled_set(PORTLED_MODE_NONE);
		printf("Port LEDs restored to default\n");
	} else if (!strcmp(argv[1], "sweep")) {
		int fn, n;

		printf("Cycling function codes 0x1-0xf on all port LEDs, "
			"2s each. Watch the LEDs...\n");
		for (fn = 1; fn <= 15; fn++) {
			printf("fn = 0x%x\n", fn);
			portled_apply_fn(bus, fn);
			for (n = 0; n < 20; n++) {
				WATCHDOG_RESET();
				mdelay(100);
			}
		}
		portled_restore(bus);
		printf("Sweep done, defaults restored\n");
	} else if (!strcmp(argv[1], "raw") && argc == 4) {
		u32 idx = simple_strtoul(argv[2], NULL, 0);
		u32 val = simple_strtoul(argv[3], NULL, 16);

		if (idx >= PORTLED_NUM_LEDS) {
			printf("LED index must be 0-%d\n", PORTLED_NUM_LEDS - 1);
			return 1;
		}
		portled_gsw_write(bus, MT7530_LED_REG(idx), val);
		printf("LED%u (0x%04x) = 0x%08x\n", idx,
			MT7530_LED_REG(idx), val);
	} else if (!strcmp(argv[1], "phy") && argc == 4) {
		u32 port = simple_strtoul(argv[2], NULL, 0);
		u32 val = simple_strtoul(argv[3], NULL, 16);

		if (port > 4) {
			printf("PHY port must be 0-4\n");
			return 1;
		}
		bus->write(bus, port, 0x1f, MT7530_PHY_LED_REG, val);
		printf("PHY%u LED reg = 0x%04x\n", port, val);
	} else {
		return CMD_RET_USAGE;
	}

	return 0;
}

U_BOOT_CMD(portled, 4, 0, do_portled,
	"MT7621 port LED control (recovery indication)",
	"- dump LED0-4 registers and current mode\n"
	"portled httpd        - all port LEDs fast blink (web recovery)\n"
	"portled console      - all port LEDs slow blink (serial interrupt)\n"
	"portled restore      - restore default LED behavior\n"
	"portled sweep        - cycle function codes 0x1-0xf for calibration\n"
	"portled raw <led> <val>   - write LED behavior register directly\n"
	"portled phy <port> <val>  - write per-port PHY LED reg (MMD 0x1f/0x24)"
);
