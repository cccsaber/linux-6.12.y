/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RFKILL_WLAN_H
#define _LINUX_RFKILL_WLAN_H

#include <linux/types.h>

#if IS_REACHABLE(CONFIG_RFKILL_RK)
int rfkill_get_wifi_power_state(int *power);
int rockchip_wifi_power(int on);
int rockchip_wifi_set_carddetect(int val);
int rockchip_wifi_get_oob_irq(void);
int rockchip_wifi_get_oob_irq_flag(void);
int rockchip_wifi_reset(int on);
int rockchip_wifi_mac_addr(unsigned char *buf);
void *rockchip_wifi_country_code(char *ccode);
#else
static inline int rfkill_get_wifi_power_state(int *power)
{
	return -ENODEV;
}

static inline int rockchip_wifi_power(int on)
{
	return -ENODEV;
}

static inline int rockchip_wifi_set_carddetect(int val)
{
	return -ENODEV;
}

static inline int rockchip_wifi_get_oob_irq(void)
{
	return -ENODEV;
}

static inline int rockchip_wifi_get_oob_irq_flag(void)
{
	return -ENODEV;
}

static inline int rockchip_wifi_reset(int on)
{
	return -ENODEV;
}

static inline int rockchip_wifi_mac_addr(unsigned char *buf)
{
	return -ENODEV;
}

static inline void *rockchip_wifi_country_code(char *ccode)
{
	return NULL;
}
#endif

#endif
