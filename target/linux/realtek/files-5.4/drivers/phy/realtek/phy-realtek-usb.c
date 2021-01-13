/*
 * Copyright (C) 2020 Dimazhan <dimazhan@list.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define RTL819X_SYSC_REG_CLK_MANAGE		0x10
#define RTL819X_SYSC_REG_USB_SIE		0x34
#define RTL819X_SYSC_REG_USB_PHY		0x90

#define RTL819X_USB_SIE_PHY_ENABLE		(1<<12)
#define RTL819X_USB_SIE_PHY1_HOST_MODE	(1<<18)

struct realtek_usb_phy {
	struct phy		*phy;
	struct regmap	*sysctl;
	bool			oneportsel;
};

static void realtek_usb_phy_enable(struct realtek_usb_phy *phy, int portnum)
{
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_USB_PHY,
		(1<<(8+portnum*11))|(1<<(9+portnum*11)),
		(1<<(8+portnum*11)) | //USBPHY_EN=1
		(1<<(9+portnum*11))); //usbphy_reset=1, active high
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_USB_PHY,
		(1<<(9+portnum*11))|(1<<(10+portnum*11)),
		//usbphy_reset=0, active high
		(1<<(10+portnum*11))); //active_usbphyt=1
}

static int realtek_usb_phy_power_on(struct phy *_phy)
{
	struct realtek_usb_phy *phy = phy_get_drvdata(_phy);

	/* setup phy1 host mode */
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_USB_SIE,
		RTL819X_USB_SIE_PHY1_HOST_MODE,
		(phy->oneportsel ? 0 : RTL819X_USB_SIE_PHY1_HOST_MODE));

	/* enable the phy */
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_USB_SIE,
		(1<<11)|RTL819X_USB_SIE_PHY_ENABLE|(1<<17),
		(1<<11) | //s_utmi_suspend0=1
		RTL819X_USB_SIE_PHY_ENABLE |
		(1<<17)); //enable pgbndry_disable=1
	if (phy->oneportsel) {
		realtek_usb_phy_enable(phy, 0);
		realtek_usb_phy_enable(phy, 1);
	} else
		realtek_usb_phy_enable(phy, 1);
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_CLK_MANAGE,
		(1<<12)|(1<<13)|(1<<19)|(1<<20)|(1<<21),
		(1<<12)|(1<<13)|(1<<19)|(1<<20)| //enable lx1, lx2
		(1<<21)); //enable host ip

	mdelay(100);

	return 0;
}

static int realtek_usb_phy_power_off(struct phy *_phy)
{
	struct realtek_usb_phy *phy = phy_get_drvdata(_phy);

	/* disable the phy */
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_CLK_MANAGE,
		(1<<12)|(1<<13)|(1<<19)|(1<<20)| //enable lx1, lx2
		(1<<21), 0); //enable host ip
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_USB_PHY,
		(1<<8)|(1<<19), 0); //USBPHY_EN=1
	regmap_update_bits(phy->sysctl, RTL819X_SYSC_REG_USB_SIE,
		RTL819X_USB_SIE_PHY_ENABLE, 0);

	return 0;
}

static struct phy_ops realtek_usb_phy_ops = {
	.power_on	= realtek_usb_phy_power_on,
	.power_off	= realtek_usb_phy_power_off,
	.owner		= THIS_MODULE,
};

static const struct of_device_id realtek_usb_phy_of_match[] = {
	{
		.compatible = "realtek,rtl819x-usbphy",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, realtek_usb_phy_of_match);

static int realtek_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	const struct of_device_id *match;
	struct realtek_usb_phy *phy;

	match = of_match_device(realtek_usb_phy_of_match, &pdev->dev);
	if (!match)
		return -ENODEV;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->sysctl = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sysctl");
	if (IS_ERR(phy->sysctl)) {
		dev_err(dev, "failed to get sysctl registers\n");
		return PTR_ERR(phy->sysctl);
	}

	phy->oneportsel = of_get_property(dev->of_node, "realtek,oneportsel", NULL);

	phy->phy = devm_phy_create(dev, NULL, &realtek_usb_phy_ops);
	if (IS_ERR(phy->phy)) {
		dev_err(dev, "failed to create PHY\n");
		return PTR_ERR(phy->phy);
	}
	phy_set_drvdata(phy->phy, phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static struct platform_driver realtek_usb_phy_driver = {
	.probe	= realtek_usb_phy_probe,
	.driver = {
		.of_match_table	= realtek_usb_phy_of_match,
		.name  = "realtek-usb-phy",
	}
};
module_platform_driver(realtek_usb_phy_driver);

MODULE_DESCRIPTION("Realtek USB phy driver");
MODULE_AUTHOR("Dimazhan <dimazhan@list.ru>");
MODULE_LICENSE("GPL v2");
