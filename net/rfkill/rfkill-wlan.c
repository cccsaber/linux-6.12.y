// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal Rockchip WLAN platform helper for vendor Wi-Fi drivers.
 *
 * The old vendor trees expose these helpers from net/rfkill. 6.12 does not
 * carry that glue layer, but the AIC8800 SDIO path still expects the same
 * exported symbols on Rockchip boards.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/rfkill-wlan.h>

#include <dt-bindings/gpio/gpio.h>

struct rk_wlan_gpio {
	int gpio;
	int active_value;
	bool requested;
};

struct rk_wlan_data {
	struct device *dev;
	struct clk *clk_wifi;
	struct mutex lock;
	struct rk_wlan_gpio power_gpio;
	struct rk_wlan_gpio vbat_gpio;
	struct rk_wlan_gpio reset_gpio;
	struct rk_wlan_gpio host_wake_gpio;
	char chip_type[64];
	u8 mac_addr[ETH_ALEN];
	bool mac_valid;
	int power_state;
};

static struct rk_wlan_data *g_rk_wlan;

static int rk_wlan_parse_gpio(struct device_node *np, const char *prop,
			      struct rk_wlan_gpio *gpio)
{
	u32 args[3];
	int ret;

	ret = of_get_named_gpio(np, prop, 0);
	if (!gpio_is_valid(ret)) {
		gpio->gpio = -1;
		gpio->active_value = 0;
		gpio->requested = false;
		return 0;
	}

	gpio->gpio = ret;
	gpio->active_value = 1;
	if (!of_property_read_u32_array(np, prop, args, ARRAY_SIZE(args)))
		gpio->active_value = (args[2] == GPIO_ACTIVE_LOW) ? 0 : 1;
	gpio->requested = false;
	return 0;
}

static int rk_wlan_request_gpio(struct rk_wlan_gpio *gpio, const char *name)
{
	int ret;

	if (!gpio_is_valid(gpio->gpio))
		return 0;

	ret = gpio_request(gpio->gpio, name);
	if (!ret) {
		gpio->requested = true;
		return 0;
	}

	/*
	 * The reset line may already be held by mmc-pwrseq-simple. The vendor
	 * driver only needs to toggle the line, so treat a busy line as
	 * shareable and keep going.
	 */
	if (ret == -EBUSY) {
		pr_info("rk-wlan: gpio %d (%s) already requested, continue\n",
			gpio->gpio, name);
		return 0;
	}

	pr_err("rk-wlan: failed to request gpio %d (%s): %d\n",
	       gpio->gpio, name, ret);
	return ret;
}

static void rk_wlan_free_gpio(struct rk_wlan_gpio *gpio)
{
	if (gpio->requested) {
		gpio_free(gpio->gpio);
		gpio->requested = false;
	}
}

static int rk_wlan_set_gpio(struct rk_wlan_gpio *gpio, bool active)
{
	int value;

	if (!gpio_is_valid(gpio->gpio))
		return 0;

	value = active ? gpio->active_value : !gpio->active_value;
	return gpio_direction_output(gpio->gpio, value);
}

static int rk_wlan_do_power_locked(struct rk_wlan_data *data, int on)
{
	int ret;

	if (on) {
		ret = rk_wlan_set_gpio(&data->vbat_gpio, true);
		if (ret)
			return ret;

		ret = rk_wlan_set_gpio(&data->power_gpio, true);
		if (ret)
			return ret;

		msleep(20);

		ret = rk_wlan_set_gpio(&data->reset_gpio, true);
		if (ret)
			return ret;

		msleep(80);
	} else {
		ret = rk_wlan_set_gpio(&data->reset_gpio, false);
		if (ret)
			return ret;

		msleep(20);

		ret = rk_wlan_set_gpio(&data->power_gpio, false);
		if (ret)
			return ret;

		ret = rk_wlan_set_gpio(&data->vbat_gpio, false);
		if (ret)
			return ret;

		msleep(50);
	}

	data->power_state = on ? 1 : 0;
	return 0;
}

int rfkill_get_wifi_power_state(int *power)
{
	if (!g_rk_wlan || !power)
		return -ENODEV;

	*power = g_rk_wlan->power_state;
	return 0;
}
EXPORT_SYMBOL_GPL(rfkill_get_wifi_power_state);

int rockchip_wifi_power(int on)
{
	int ret;

	if (!g_rk_wlan)
		return -ENODEV;

	mutex_lock(&g_rk_wlan->lock);
	ret = rk_wlan_do_power_locked(g_rk_wlan, on);
	mutex_unlock(&g_rk_wlan->lock);

	if (!ret)
		pr_info("rk-wlan: power %s\n", on ? "on" : "off");

	return ret;
}
EXPORT_SYMBOL_GPL(rockchip_wifi_power);

int rockchip_wifi_reset(int on)
{
	int ret;

	if (!g_rk_wlan)
		return -ENODEV;

	mutex_lock(&g_rk_wlan->lock);
	ret = rk_wlan_set_gpio(&g_rk_wlan->reset_gpio, on);
	if (!ret)
		msleep(on ? 50 : 20);
	mutex_unlock(&g_rk_wlan->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(rockchip_wifi_reset);

int rockchip_wifi_set_carddetect(int val)
{
	/*
	 * Current H20plus DTS keeps the SDIO device non-removable and already
	 * enumerated, so card-detect is only a compatibility hook.
	 */
	pr_info("rk-wlan: carddetect request %d ignored\n", val);
	return 0;
}
EXPORT_SYMBOL_GPL(rockchip_wifi_set_carddetect);

int rockchip_wifi_get_oob_irq(void)
{
	if (!g_rk_wlan || !gpio_is_valid(g_rk_wlan->host_wake_gpio.gpio))
		return -ENODEV;

	return gpio_to_irq(g_rk_wlan->host_wake_gpio.gpio);
}
EXPORT_SYMBOL_GPL(rockchip_wifi_get_oob_irq);

int rockchip_wifi_get_oob_irq_flag(void)
{
	if (!g_rk_wlan || !gpio_is_valid(g_rk_wlan->host_wake_gpio.gpio))
		return -ENODEV;

	return g_rk_wlan->host_wake_gpio.active_value ? 0 : 1;
}
EXPORT_SYMBOL_GPL(rockchip_wifi_get_oob_irq_flag);

int rockchip_wifi_mac_addr(unsigned char *buf)
{
	if (!g_rk_wlan || !buf)
		return -ENODEV;

	mutex_lock(&g_rk_wlan->lock);
	if (!g_rk_wlan->mac_valid) {
		eth_random_addr(g_rk_wlan->mac_addr);
		g_rk_wlan->mac_valid = true;
		pr_info("rk-wlan: generated random MAC %pM for %s\n",
			g_rk_wlan->mac_addr, g_rk_wlan->chip_type);
	}

	ether_addr_copy(buf, g_rk_wlan->mac_addr);
	mutex_unlock(&g_rk_wlan->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(rockchip_wifi_mac_addr);

void *rockchip_wifi_country_code(char *ccode)
{
	return NULL;
}
EXPORT_SYMBOL_GPL(rockchip_wifi_country_code);

static int rk_wlan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rk_wlan_data *data;
	const char *chip_type;
	int ret;

	if (!np)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	mutex_init(&data->lock);

	ret = rk_wlan_parse_gpio(np, "WIFI,poweren_gpio", &data->power_gpio);
	if (ret)
		return ret;
	ret = rk_wlan_parse_gpio(np, "WIFI,vbat_gpio", &data->vbat_gpio);
	if (ret)
		return ret;
	ret = rk_wlan_parse_gpio(np, "WIFI,reset_gpio", &data->reset_gpio);
	if (ret)
		return ret;
	ret = rk_wlan_parse_gpio(np, "WIFI,host_wake_irq", &data->host_wake_gpio);
	if (ret)
		return ret;

	ret = rk_wlan_request_gpio(&data->power_gpio, "rk-wlan-power");
	if (ret)
		return ret;
	ret = rk_wlan_request_gpio(&data->vbat_gpio, "rk-wlan-vbat");
	if (ret)
		goto err_free_power;
	ret = rk_wlan_request_gpio(&data->reset_gpio, "rk-wlan-reset");
	if (ret)
		goto err_free_vbat;
	ret = rk_wlan_request_gpio(&data->host_wake_gpio, "rk-wlan-hostwake");
	if (ret)
		goto err_free_reset;

	if (!of_property_read_string(np, "wifi_chip_type", &chip_type))
		strscpy(data->chip_type, chip_type, sizeof(data->chip_type));
	else
		strscpy(data->chip_type, "rkwifi", sizeof(data->chip_type));

	data->clk_wifi = devm_clk_get_optional(dev, "clk_wifi");
	if (IS_ERR(data->clk_wifi)) {
		ret = PTR_ERR(data->clk_wifi);
		goto err_free_hostwake;
	}

	if (data->clk_wifi) {
		ret = clk_prepare_enable(data->clk_wifi);
		if (ret)
			goto err_free_hostwake;
	}

	of_get_mac_address(np, data->mac_addr);
	data->mac_valid = is_valid_ether_addr(data->mac_addr);
	data->power_state = 1;

	platform_set_drvdata(pdev, data);
	g_rk_wlan = data;

	pr_info("rk-wlan: ready for %s reset=%d hostwake=%d\n",
		data->chip_type, data->reset_gpio.gpio, data->host_wake_gpio.gpio);
	return 0;

err_free_hostwake:
	rk_wlan_free_gpio(&data->host_wake_gpio);
err_free_reset:
	rk_wlan_free_gpio(&data->reset_gpio);
err_free_vbat:
	rk_wlan_free_gpio(&data->vbat_gpio);
err_free_power:
	rk_wlan_free_gpio(&data->power_gpio);
	return ret;
}

static void rk_wlan_remove(struct platform_device *pdev)
{
	struct rk_wlan_data *data = platform_get_drvdata(pdev);

	if (!data)
		return;

	if (g_rk_wlan == data)
		g_rk_wlan = NULL;

	if (data->clk_wifi)
		clk_disable_unprepare(data->clk_wifi);

	rk_wlan_free_gpio(&data->host_wake_gpio);
	rk_wlan_free_gpio(&data->reset_gpio);
	rk_wlan_free_gpio(&data->vbat_gpio);
	rk_wlan_free_gpio(&data->power_gpio);
}

static const struct of_device_id rk_wlan_of_match[] = {
	{ .compatible = "wlan-platdata" },
	{ }
};
MODULE_DEVICE_TABLE(of, rk_wlan_of_match);

static struct platform_driver rk_wlan_driver = {
	.probe = rk_wlan_probe,
	.remove_new = rk_wlan_remove,
	.driver = {
		.name = "rfkill_wlan",
		.of_match_table = rk_wlan_of_match,
	},
};
module_platform_driver(rk_wlan_driver);

MODULE_DESCRIPTION("Rockchip WLAN platform helper");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");
