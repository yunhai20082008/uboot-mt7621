/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2026
 *
 * MT7621 ethernet port LED mode control.
 *
 * The RJ45 port LEDs are driven by the MT7530 switch.  During recovery
 * modes (web failsafe / serial interruption) the switch LED registers
 * are reprogrammed so all port LEDs blink, with a different blink rate
 * per mode, to give a visual indication of the active recovery mode.
 */

#ifndef __MT7621_PORTLED_H
#define __MT7621_PORTLED_H

struct mii_dev;

enum mt7621_portled_mode {
	PORTLED_MODE_NONE = 0,	/* normal boot, LEDs left untouched */
	PORTLED_MODE_HTTPD,	/* web failsafe recovery, fast blink */
	PORTLED_MODE_CONSOLE,	/* serial interruption, slow blink */
};

/*
 * Select the current LED mode.  PORTLED_MODE_NONE restores the default
 * LED behavior.  The mode is remembered and re-applied after every
 * MT7530 software reset (see mt7530_setup in drivers/net/mt7621_eth.c),
 * so it survives network restarts.
 */
int mt7621_portled_set(enum mt7621_portled_mode mode);

/*
 * Re-apply the current LED mode after an MT7530 reset.  Called at the
 * end of mt7530_setup(); the weak default in the driver is a no-op.
 */
void mt7621_portled_reapply(struct mii_dev *bus);

/*
 * Software-blink driver for recovery modes.  Called from the busy-wait
 * loops of net_loop() and bootmenu; toggles the PHY force_on bit with
 * a mode-specific period.  No-op unless a recovery mode is active.
 */
void mt7621_portled_tick(void);

/*
 * Called when a key press stops the autoboot countdown of the bootmenu
 * (weak default in cmd/bootmenu.c is a no-op).
 */
void board_bootmenu_interrupted(void);

#endif /* __MT7621_PORTLED_H */
