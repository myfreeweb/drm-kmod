// SPDX-License-Identifier: MIT
/*
 * Copyright © 2018-2019 Intel Corporation
 *
 * Autogenerated file by GPU Top : https://github.com/rib/gputop
 * DO NOT EDIT manually!
 */

#include <linux/sysfs.h>

#include "i915_drv.h"
#include "i915_oa_bdw.h"

static const struct i915_oa_reg b_counter_config_test_oa[] = {
	{ _MMIO(0x2740), 0x00000000 },
	{ _MMIO(0x2744), 0x00800000 },
	{ _MMIO(0x2714), 0xf0800000 },
	{ _MMIO(0x2710), 0x00000000 },
	{ _MMIO(0x2724), 0xf0800000 },
	{ _MMIO(0x2720), 0x00000000 },
	{ _MMIO(0x2770), 0x00000004 },
	{ _MMIO(0x2774), 0x00000000 },
	{ _MMIO(0x2778), 0x00000003 },
	{ _MMIO(0x277c), 0x00000000 },
	{ _MMIO(0x2780), 0x00000007 },
	{ _MMIO(0x2784), 0x00000000 },
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
	{ _MMIO(0x9840), 0x000000a0 },
	{ _MMIO(0x9888), 0x198b0000 },
	{ _MMIO(0x9888), 0x078b0066 },
	{ _MMIO(0x9888), 0x118b0000 },
	{ _MMIO(0x9888), 0x258b0000 },
	{ _MMIO(0x9888), 0x21850008 },
	{ _MMIO(0x9888), 0x0d834000 },
	{ _MMIO(0x9888), 0x07844000 },
	{ _MMIO(0x9888), 0x17804000 },
	{ _MMIO(0x9888), 0x21800000 },
	{ _MMIO(0x9888), 0x4f800000 },
	{ _MMIO(0x9888), 0x41800000 },
	{ _MMIO(0x9888), 0x31800000 },
	{ _MMIO(0x9840), 0x00000080 },
};

static ssize_t
show_test_oa_id(struct device *kdev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "1\n");
}

void
i915_perf_load_test_config_bdw(struct drm_i915_private *dev_priv)
{
	strlcpy(dev_priv->perf.oa.test_config.uuid,
		"d6de6f55-e526-4f79-a6a6-d7315c09044e",
		sizeof(dev_priv->perf.oa.test_config.uuid));
	dev_priv->perf.oa.test_config.id = 1;

	dev_priv->perf.oa.test_config.mux_regs = mux_config_test_oa;
	dev_priv->perf.oa.test_config.mux_regs_len = ARRAY_SIZE(mux_config_test_oa);

	dev_priv->perf.oa.test_config.b_counter_regs = b_counter_config_test_oa;
	dev_priv->perf.oa.test_config.b_counter_regs_len = ARRAY_SIZE(b_counter_config_test_oa);

	dev_priv->perf.oa.test_config.flex_regs = flex_eu_config_test_oa;
	dev_priv->perf.oa.test_config.flex_regs_len = ARRAY_SIZE(flex_eu_config_test_oa);

	dev_priv->perf.oa.test_config.sysfs_metric.name = "d6de6f55-e526-4f79-a6a6-d7315c09044e";
	dev_priv->perf.oa.test_config.sysfs_metric.attrs = dev_priv->perf.oa.test_config.attrs;

	dev_priv->perf.oa.test_config.attrs[0] = &dev_priv->perf.oa.test_config.sysfs_metric_id.attr;

	dev_priv->perf.oa.test_config.sysfs_metric_id.attr.name = "id";
	dev_priv->perf.oa.test_config.sysfs_metric_id.attr.mode = 0444;
	dev_priv->perf.oa.test_config.sysfs_metric_id.show = show_test_oa_id;
}
