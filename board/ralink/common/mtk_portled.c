/*
 * Copyright (C) 2026
 *
 * MT7621 ethernet port LED mode control.
 *
 * The port LEDs of the E8820V2 are driven by the internal PHYs of the
 * embedded MT7530 switch (GSW LED_EN 0x7d00=1, LED_IO_MODE 0x7d04=1
 * means "PHY mode").  Each internal PHY (address 0-4) exposes two LED
 * control registers in a vendor MMD space:
 *
 *   0x24 LED0 ON_CTRL   bit15 enable, bit14 polarity, bit6 force_on,
 *                       bits 5:0 "on" event bits (link/fdx/hdx)
 *   0x25 LED0 BLINK_CTRL
 *   0x26 LED1 ON_CTRL
 *   0x27 LED1 BLINK_CTRL
 *
 * Recovery modes blink all five port LEDs in software by toggling the
 * force_on bit from the polling loops (net_loop and bootmenu), with a
 * mode-specific period:
 *
 *   - web failsafe (httpd):  fast blink
 *   - serial interruption:   slow blink
 *   - normal boot:           untouched (default link/activity)
 *
 * The MT7530 is soft-reset by every eth_start(), which also restores
 * the PHY LED registers to power-on defaults.  The current mode is
 * remembered and re-applied at the end of mt7530_setup() via
 * mt7621_portled_reapply().
 */

#include <common.h>
#include <command.h>
#include <dm.h>
#include <dm/uclass-id.h>
#include <miiphy.h>
#include <watchdog.h>
#include <linux/mdio.h>

#include <mt7621_portled.h>

/* Number of RJ45 ports (internal PHY addresses 0-4): 4 LAN + 1 WAN */
#define PORTLED_NUM_PORTS	5

/* Vendor MMD holding the LED registers.  The MT7621 driver comment
 * uses devad 0x1f; the kernel uses MDIO_MMD_VEND2 (0x1e).  Switchable
 * at runtime with `portled mmd <n>`.
 */
#define PORTLED_MMD_DEFAULT	0x1f

#define PHY_LED0_ON_CTRL	0x24
#define PHY_LED0_BLINK_CTRL	0x25
#define PHY_LED1_ON_CTRL	0x26
#define PHY_LED1_BLINK_CTRL	0x27

/* ON_CTRL bits (layout per kernel drivers/net/phy/mediatek/mtk.h) */
#define LED_ON_ENABLE		BIT(15)
#define LED_ON_POLARITY		BIT(14)
#define LED_ON_FORCE_ON		BIT(6)
#define LED_ON_EVENT_MASK	0x7f	/* force_on + link/duplex events */

/* BLINK_CTRL bits */
#define LED_BLINK_FORCE		BIT(9)

/*
 * LED0 ON_CTRL values calibrated on the E8820V2 with `portled phy`:
 * 0xC040 (enable | polarity | force_on) lights all port LEDs solid,
 * 0x4000 (polarity, block disabled) turns them off.  Writing either
 * PHY address affects all five port LEDs, so the register acts
 * globally for the internal PHY LED block.
 */
#define PORTLED_ON_VAL		0xc040
#define PORTLED_OFF_VAL		0x4000

/* GSW-wide LED configuration (page-addressed switch registers) */
#define GSW_LED_EN		0x7d00	/* 1: LED block enabled (default) */
#define GSW_LED_IO_MODE		0x7d04	/* 0: GPIO mode, 1: PHY mode */

/* Blink toggle periods in ms */
#define PORTLED_T_FAST		100	/* httpd: 5 Hz */
#define PORTLED_T_SLOW		400	/* console: 1.25 Hz */

static enum mt7621_portled_mode cur_mode = PORTLED_MODE_NONE;
static int defaults_captured;
static int mmd_dev = PORTLED_MMD_DEFAULT;

static u16 def_on[PORTLED_NUM_PORTS];
static u16 def_blink[PORTLED_NUM_PORTS];
static u16 def_on1[PORTLED_NUM_PORTS];
static u16 def_blink1[PORTLED_NUM_PORTS];

static u16 last_written[PORTLED_NUM_PORTS];
static int last_written_valid;

static ulong last_tick;
static int blink_phase;

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

static int portled_phy_read(struct mii_dev *bus, int port, u32 reg)
{
	return bus->read(bus, port, mmd_dev, reg);
}

static int portled_phy_write(struct mii_dev *bus, int port, u32 reg, u16 val)
{
	return bus->write(bus, port, mmd_dev, reg, val);
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
	int p;

	if (defaults_captured)
		return;

	for (p = 0; p < PORTLED_NUM_PORTS; p++) {
		int v;

		v = portled_phy_read(bus, p, PHY_LED0_ON_CTRL);
		if (v < 0)
			return;
		def_on[p] = (u16)v;

		v = portled_phy_read(bus, p, PHY_LED0_BLINK_CTRL);
		if (v < 0)
			return;
		def_blink[p] = (u16)v;

		v = portled_phy_read(bus, p, PHY_LED1_ON_CTRL);
		if (v < 0)
			return;
		def_on1[p] = (u16)v;

		v = portled_phy_read(bus, p, PHY_LED1_BLINK_CTRL);
		if (v < 0)
			return;
		def_blink1[p] = (u16)v;
	}

	defaults_captured = 1;
}

/* Blink: write the calibrated ON/OFF pattern to all port LEDs */
static void portled_apply_blink(struct mii_dev *bus, int on)
{
	u16 val = on ? PORTLED_ON_VAL : PORTLED_OFF_VAL;
	int p;

	for (p = 0; p < PORTLED_NUM_PORTS; p++) {
		if (last_written_valid && last_written[p] == val)
			continue;

		if (!portled_phy_write(bus, p, PHY_LED0_ON_CTRL, val))
			last_written[p] = val;
	}

	last_written_valid = 1;
}

static void portled_restore(struct mii_dev *bus)
{
	int p;

	if (!defaults_captured)
		return;

	for (p = 0; p < PORTLED_NUM_PORTS; p++) {
		portled_phy_write(bus, p, PHY_LED0_ON_CTRL, def_on[p]);
		portled_phy_write(bus, p, PHY_LED0_BLINK_CTRL, def_blink[p]);
		portled_phy_write(bus, p, PHY_LED1_ON_CTRL, def_on1[p]);
		portled_phy_write(bus, p, PHY_LED1_BLINK_CTRL, def_blink1[p]);
	}

	last_written_valid = 0;
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

	portled_capture_defaults(bus);
	if (!defaults_captured)
		return -EIO;

	switch (mode) {
	case PORTLED_MODE_HTTPD:
	case PORTLED_MODE_CONSOLE:
		/* Start dark, mt7621_portled_tick() toggles from now on */
		blink_phase = 0;
		last_tick = get_timer(0);
		portled_apply_blink(bus, 0);
		break;
	default:
		portled_restore(bus);
		break;
	}

	if (ret)
		debug("%s: apply failed (%d)\n", __func__, ret);

	return ret;
}

/*
 * Toggle the force_on bit on all port LEDs with the mode-specific
 * period.  Called from the busy-wait loops of net_loop and bootmenu;
 * cheap no-op unless a recovery mode is active.
 */
void mt7621_portled_tick(void)
{
	struct mii_dev *bus;
	ulong period;
	ulong now;

	if (cur_mode == PORTLED_MODE_NONE || !defaults_captured)
		return;

	now = get_timer(0);
	period = (cur_mode == PORTLED_MODE_HTTPD) ? PORTLED_T_FAST :
						    PORTLED_T_SLOW;
	if (now - last_tick < period)
		return;
	last_tick = now;

	bus = portled_get_bus();
	if (!bus)
		return;

	blink_phase ^= 1;
	portled_apply_blink(bus, blink_phase);
}

void mt7621_portled_reapply(struct mii_dev *bus)
{
	if (!bus || cur_mode == PORTLED_MODE_NONE || !defaults_captured)
		return;

	/* Registers were reset to defaults by the switch soft-reset */
	blink_phase = 0;
	last_tick = get_timer(0);
	last_written_valid = 0;
	portled_apply_blink(bus, 0);
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
	u32 val;
	int p;

	bus = portled_get_bus();
	if (!bus) {
		printf("No ethernet/MDIO bus available\n");
		return 1;
	}

	if (argc == 1) {
		printf("Current mode: %d (0=none 1=httpd 2=console), devad 0x%x\n",
			cur_mode, mmd_dev);

		portled_gsw_read(bus, GSW_LED_EN, &val);
		printf("GSW LED_EN   (0x%04x): 0x%08x\n", GSW_LED_EN, val);
		portled_gsw_read(bus, GSW_LED_IO_MODE, &val);
		printf("GSW LED_IO_MODE (0x%04x): 0x%08x (bit0: 1=PHY mode)\n",
			GSW_LED_IO_MODE, val);

		for (p = 0; p < PORTLED_NUM_PORTS; p++) {
			printf("PHY%d: ON0 0x%04x BLINK0 0x%04x ON1 0x%04x BLINK1 0x%04x\n",
				p,
				portled_phy_read(bus, p, PHY_LED0_ON_CTRL) & 0xffff,
				portled_phy_read(bus, p, PHY_LED0_BLINK_CTRL) & 0xffff,
				portled_phy_read(bus, p, PHY_LED1_ON_CTRL) & 0xffff,
				portled_phy_read(bus, p, PHY_LED1_BLINK_CTRL) & 0xffff);
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
	} else if (!strcmp(argv[1], "psw") && argc == 3) {
		mt7621_portled_set(PORTLED_MODE_CONSOLE);
		if (!strcmp(argv[2], "on")) {
			portled_apply_blink(bus, 1);
			printf("All LEDs forced on (0x%04x)\n", PORTLED_ON_VAL);
		} else if (!strcmp(argv[2], "off")) {
			portled_apply_blink(bus, 0);
			printf("All LEDs forced off (0x%04x)\n",
				PORTLED_OFF_VAL);
		} else if (!strcmp(argv[2], "blink")) {
			for (p = 0; p < PORTLED_NUM_PORTS; p++) {
				portled_phy_write(bus, p, PHY_LED0_ON_CTRL,
					PORTLED_OFF_VAL);
				portled_phy_write(bus, p, PHY_LED0_BLINK_CTRL,
					LED_BLINK_FORCE);
			}
			last_written_valid = 0;
			printf("All LEDs hardware force-blink\n");
		} else {
			return CMD_RET_USAGE;
		}
	} else if (!strcmp(argv[1], "mmd") && argc == 3) {
		mmd_dev = simple_strtoul(argv[2], NULL, 0);
		printf("PHY LED devad set to 0x%x\n", mmd_dev);
	} else if (!strcmp(argv[1], "phy") && argc == 5) {
		u32 port = simple_strtoul(argv[2], NULL, 0);
		u32 reg = simple_strtoul(argv[3], NULL, 16);
		u32 v = simple_strtoul(argv[4], NULL, 16);
		int rv;

		if (port >= PORTLED_NUM_PORTS) {
			printf("PHY port must be 0-%d\n", PORTLED_NUM_PORTS - 1);
			return 1;
		}

		rv = portled_phy_write(bus, port, reg, (u16)v);
		if (rv) {
			printf("write failed (%d)\n", rv);
			return 1;
		}
		last_written_valid = 0;
		printf("PHY%u devad 0x%x reg 0x%02x = 0x%04x (read back 0x%04x)\n",
			port, mmd_dev, reg, (u16)v,
			portled_phy_read(bus, port, reg) & 0xffff);
	} else {
		return CMD_RET_USAGE;
	}

	return 0;
}

U_BOOT_CMD(portled, 5, 0, do_portled,
	"MT7621 port LED control (recovery indication)",
	"- dump LED mode, GSW LED config and per-port PHY LED registers\n"
	"portled httpd       - all port LEDs fast blink (web recovery)\n"
	"portled console     - all port LEDs slow blink (serial interrupt)\n"
	"portled restore     - restore default LED behavior\n"
	"portled psw on|off|blink - force pattern on all port LEDs\n"
	"portled mmd <n>     - set PHY MMD devad (default 0x1f, try 0x1e)\n"
	"portled phy <p> <reg> <val> - raw write PHY LED register (hex)"
);
