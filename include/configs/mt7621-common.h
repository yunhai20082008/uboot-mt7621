/* SPDX-License-Identifier:	GPL-2.0+ */
/*
 * Copyright (C) 2018 MediaTek Incorporation. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef __CONFIG_MT7621_COMMON_H
#define __CONFIG_MT7621_COMMON_H

#define CONFIG_SYS_HZ			1000
#define CONFIG_SYS_MIPS_TIMER_FREQ	880000000

#define CONFIG_SYS_MONITOR_BASE		CONFIG_SYS_TEXT_BASE

#define CONFIG_SYS_MALLOC_LEN		0x100000
#define CONFIG_SYS_BOOTPARAMS_LEN	0x20000

#define CONFIG_SYS_SDRAM_BASE		0x80000000

#if defined(CONFIG_MT7621_DRAM_DDR2_512M_W9751G6KB_A02_1066MHZ_LEGACY) || \
	defined(CONFIG_MT7621_DRAM_DDR2_512M_W9751G6KB_A02_1066MHZ) || \
	defined(CONFIG_MT7621_DRAM_DDR2_512M_LEGACY) ||\
	defined(CONFIG_MT7621_DRAM_DDR2_512M)
#define CONFIG_SYS_LOAD_ADDR	0x82000000
#else
#define CONFIG_SYS_LOAD_ADDR	0x84000000
#endif

#define CONFIG_VERY_BIG_RAM
#define CONFIG_MAX_MEM_MAPPED		0x1c000000

#define CONFIG_SYS_MIPS_CACHE_MODE	CONF_CM_CACHABLE_COW

#define CONFIG_SYS_BOOTM_LEN		0x2000000

/* SPL */
#define CONFIG_SPL_BSS_START_ADDR	0xbe108000
#define CONFIG_SPL_BSS_MAX_SIZE		0x2000

/* Serial Port */
#if defined(CONFIG_SPL_BUILD) && defined(CONFIG_SPL_SERIAL_SUPPORT)
#define CONFIG_SYS_NS16550_REG_SIZE	-4
#define CONFIG_CONS_INDEX		1
#define CONFIG_SYS_NS16550_CLK		50000000
#define CONFIG_SYS_NS16550_MEM32
#endif
#define CONFIG_SYS_BAUDRATE_TABLE	{ 9600, 19200, 38400, 57600, 115200, \
					  230400, 460800, 921600 }

/* Custom version string, printed right after the SPL banner (first serial
 * output on every MT7621 board) and appended to the full U-Boot banner via
 * CONFIG_IDENT_STRING.
 */
#define MT7621_UBOOT_VERSION		"Fitten-UB-XYZ-VER1"

/* Network
 * Shorten ARP retry (default: 5s x 5 = 25s per attempt) so the
 * net-recovery probe in mtkautoboot fails fast (~2s per ping)
 * when the recovery host is not present. TFTP still uses its own
 * 5s packet timeout, so normal transfers are not affected.
 */
#define CONFIG_ARP_TIMEOUT		1000
#define CONFIG_NET_RETRY_COUNT		2

/* PCIe/WiFi reset pins (E8820V2, active LOW)
 * Pulse low-then-high in last_stage_init so both WiFi chips go
 * through a full reset cycle before the kernel takes over.
 */
#define MT7621_WIFI_RESET_2G		19
#define MT7621_WIFI_RESET_5G		26

#endif  /* __CONFIG_MT7621_COMMON_H */
