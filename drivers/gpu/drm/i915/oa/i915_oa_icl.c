// SPDX-License-Identifier: MIT
/*
 * Copyright © 2018-2019 Intel Corporation
 *
 * Autogenerated file by GPU Top : https://github.com/rib/gputop
 * DO NOT EDIT manually!
 */

#include <linux/sysfs.h>

#include "i915_drv.h"
#include "i915_oa_icl.h"

static const struct i915_oa_reg b_counter_config_test_oa[] = {
	{ _MMIO(0x2740), 0x00000000 },
	{ _MMIO(0x2710), 0x00000000 },
	{ _MMIO(0x2714), 0xf0800000 },
	{ _MMIO(0x2720), 0x00000000 },
	{ _MMIO(0x2724), 0xf0800000 },
	{ _MMIO(0x2770), 0x00000004 },
	{ _MMIO(0x2774), 0x0000ffff },
	{ _MMIO(0x2778), 0x00000003 },
	{ _MMIO(0x277c), 0x0000ffff },
	{ _MMIO(0x2780), 0x00000007 },
	{ _MMIO(0x2784), 0x0000ffff },
	{ _MMIO(0x2788), 0x00100002 },
	{ _MMIO(0x278c), 0x0000fff7 },
	{ _MMIO(0x2790), 0x00100002 },
	{ _MMIO(0x2794), 0x0000ffcf },
	{ _MMIO(0x2798), 0x00100082 },
	{ _MMIO(0x279c), 0x0000ffef },
	{ _MMIO(0x27a0), 0x001000c2 },
	{ _MMIO(0x27a4), 0x0000ffe7 },
	{ _MMIO(0x27a8), 0x00100001 },
	{ _MMIO(0x27ac), 0x0000ffe7 },
};

static const struct i915_oa_reg flex_eu_config_test_oa[] = {
};

static const struct i915_oa_reg mux_config_test_oa[] = {
	{ _MMIO(0xd04), 0x00000200 },
	{ _MMIO(0x9840), 0x00000000 },
	{ _MMIO(0x9884), 0x00000000 },
	{ _MMIO(0x9888), 0x10060000 },
	{ _MMIO(0x9888), 0x22060000 },
	{ _MMIO(0x9888), 0x16060000 },
	{ _MMIO(0x9888), 0x24060000 },
	{ _MMIO(0x9888), 0x18060000 },
	{ _MMIO(0x9888), 0x1a060000 },
	{ _MMIO(0x9888), 0x12060000 },
	{ _MMIO(0x9888), 0x14060000 },
	{ _MMIO(0x9888), 0x10060000 },
	{ _MMIO(0x9888), 0x22060000 },
	{ _MMIO(0x9884), 0x00000003 },
	{ _MMIO(0x9888), 0x16130000 },
	{ _MMIO(0x9888), 0x24000001 },
	{ _MMIO(0x9888), 0x0e130056 },
	{ _MMIO(0x9888), 0x10130000 },
	{ _MMIO(0x9888), 0x1a130000 },
	{ _MMIO(0x9888), 0x541f0001 },
	{ _MMIO(0x9888), 0x181f0000 },
	{ _MMIO(0x9888), 0x4c1f0000 },
	{ _MMIO(0x9888), 0x301f0000 },
};

static ssize_t
show_test_oa_id(struct device *kdev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "1\n");
}

void
i915_perf_load_test_config_icl(struct drm_i915_private *dev_priv)
{
	strlcpy(dev_priv->perf.oa.test_config.uuid,
		"a291665e-244b-4b76-9b9a-01de9d3c8068",
		sizeof(dev_priv->perf.oa.test_config.uuid));
	dev_priv->perf.oa.test_config.id = 1;

	dev_priv->perf.oa.test_config.mux_regs = mux_config_test_oa;
	dev_priv->perf.oa.test_config.mux_regs_len = ARRAY_SIZE(mux_config_test_oa);

	dev_priv->perf.oa.test_config.b_counter_regs = b_counter_config_test_oa;
	dev_priv->perf.oa.test_config.b_counter_regs_len = ARRAY_SIZE(b_counter_config_test_oa);

	dev_priv->perf.oa.test_config.flex_regs = flex_eu_config_test_oa;
	dev_priv->perf.oa.test_config.flex_regs_len = ARRAY_SIZE(flex_eu_config_test_oa);

	dev_priv->perf.oa.test_config.sysfs_metric.name = "a291665e-244b-4b76-9b9a-01de9d3c8068";
	dev_priv->perf.oa.test_config.sysfs_metric.attrs = dev_priv->perf.oa.test_config.attrs;

	dev_priv->perf.oa.test_config.attrs[0] = &dev_priv->perf.oa.test_config.sysfs_metric_id.attr;

	dev_priv->perf.oa.test_config.sysfs_metric_id.attr.name = "id";
	dev_priv->perf.oa.test_config.sysfs_metric_id.attr.mode = 0444;
	dev_priv->perf.oa.test_config.sysfs_metric_id.show = show_test_oa_id;
}
