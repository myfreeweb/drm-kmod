/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include <linux/uaccess.h>

#include <drm/drm_debugfs.h>

#include "dc.h"
#include "amdgpu.h"
#include "amdgpu_dm.h"
#include "amdgpu_dm_debugfs.h"
#include "dm_helpers.h"
#include "dmub/inc/dmub_srv.h"

struct dmub_debugfs_trace_header {
	uint32_t entry_count;
	uint32_t reserved[3];
};

struct dmub_debugfs_trace_entry {
	uint32_t trace_code;
	uint32_t tick_count;
	uint32_t param0;
	uint32_t param1;
};

/* function description
 * get/ set DP configuration: lane_count, link_rate, spread_spectrum
 *
 * valid lane count value: 1, 2, 4
 * valid link rate value:
 * 06h = 1.62Gbps per lane
 * 0Ah = 2.7Gbps per lane
 * 0Ch = 3.24Gbps per lane
 * 14h = 5.4Gbps per lane
 * 1Eh = 8.1Gbps per lane
 *
 * debugfs is located at /sys/kernel/debug/dri/0/DP-x/link_settings
 *
 * --- to get dp configuration
 *
 * cat link_settings
 *
 * It will list current, verified, reported, preferred dp configuration.
 * current -- for current video mode
 * verified --- maximum configuration which pass link training
 * reported --- DP rx report caps (DPCD register offset 0, 1 2)
 * preferred --- user force settings
 *
 * --- set (or force) dp configuration
 *
 * echo <lane_count>  <link_rate> > link_settings
 *
 * for example, to force to  2 lane, 2.7GHz,
 * echo 4 0xa > link_settings
 *
 * spread_spectrum could not be changed dynamically.
 *
 * in case invalid lane count, link rate are force, no hw programming will be
 * done. please check link settings after force operation to see if HW get
 * programming.
 *
 * cat link_settings
 *
 * check current and preferred settings.
 *
 */
static ssize_t dp_link_settings_read(struct file *f, char __user *buf,
				 size_t size, loff_t *pos)
{
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dc_link *link = connector->dc_link;
	char *rd_buf = NULL;
	char *rd_buf_ptr = NULL;
	const uint32_t rd_buf_size = 100;
	uint32_t result = 0;
	uint8_t str_len = 0;
	int r;

	if (*pos & 3 || size & 3)
		return -EINVAL;

	rd_buf = kcalloc(rd_buf_size, sizeof(char), GFP_KERNEL);
	if (!rd_buf)
		return 0;

	rd_buf_ptr = rd_buf;

	str_len = strlen("Current:  %d  %d  %d  ");
	snprintf(rd_buf_ptr, str_len, "Current:  %d  %d  %d  ",
			link->cur_link_settings.lane_count,
			link->cur_link_settings.link_rate,
			link->cur_link_settings.link_spread);
	rd_buf_ptr += str_len;

	str_len = strlen("Verified:  %d  %d  %d  ");
	snprintf(rd_buf_ptr, str_len, "Verified:  %d  %d  %d  ",
			link->verified_link_cap.lane_count,
			link->verified_link_cap.link_rate,
			link->verified_link_cap.link_spread);
	rd_buf_ptr += str_len;

	str_len = strlen("Reported:  %d  %d  %d  ");
	snprintf(rd_buf_ptr, str_len, "Reported:  %d  %d  %d  ",
			link->reported_link_cap.lane_count,
			link->reported_link_cap.link_rate,
			link->reported_link_cap.link_spread);
	rd_buf_ptr += str_len;

	str_len = strlen("Preferred:  %d  %d  %d  ");
	snprintf(rd_buf_ptr, str_len, "Preferred:  %d  %d  %d\n",
			link->preferred_link_setting.lane_count,
			link->preferred_link_setting.link_rate,
			link->preferred_link_setting.link_spread);

	while (size) {
		if (*pos >= rd_buf_size)
			break;

		r = put_user(*(rd_buf + result), buf);
		if (r)
			return r; /* r = -EFAULT */

		buf += 1;
		size -= 1;
		*pos += 1;
		result += 1;
	}

	kfree(rd_buf);
	return result;
}

static ssize_t dp_link_settings_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dc_link *link = connector->dc_link;
	struct dc *dc = (struct dc *)link->dc;
	struct dc_link_settings prefer_link_settings;
	char *wr_buf = NULL;
	char *wr_buf_ptr = NULL;
	const uint32_t wr_buf_size = 40;
	int r;
	int bytes_from_user;
	char *sub_str;
	/* 0: lane_count; 1: link_rate */
	uint8_t param_index = 0;
	long param[2];
	const char delimiter[3] = {' ', '\n', '\0'};
	bool valid_input = false;

	if (size == 0)
		return -EINVAL;

	wr_buf = kcalloc(wr_buf_size, sizeof(char), GFP_KERNEL);
	if (!wr_buf)
		return -EINVAL;
	wr_buf_ptr = wr_buf;

	r = copy_from_user(wr_buf_ptr, buf, wr_buf_size);

	/* r is bytes not be copied */
	if (r >= wr_buf_size) {
		kfree(wr_buf);
		DRM_DEBUG_DRIVER("user data not read\n");
		return -EINVAL;
	}

	bytes_from_user = wr_buf_size - r;

	while (isspace(*wr_buf_ptr))
		wr_buf_ptr++;

	while ((*wr_buf_ptr != '\0') && (param_index < 2)) {

		sub_str = strsep(&wr_buf_ptr, delimiter);

		r = kstrtol(sub_str, 16, &param[param_index]);

		if (r)
			DRM_DEBUG_DRIVER("string to int convert error code: %d\n", r);

		param_index++;
		while (isspace(*wr_buf_ptr))
			wr_buf_ptr++;
	}

	switch (param[0]) {
	case LANE_COUNT_ONE:
	case LANE_COUNT_TWO:
	case LANE_COUNT_FOUR:
		valid_input = true;
		break;
	default:
		break;
	}

	switch (param[1]) {
	case LINK_RATE_LOW:
	case LINK_RATE_HIGH:
	case LINK_RATE_RBR2:
	case LINK_RATE_HIGH2:
	case LINK_RATE_HIGH3:
		valid_input = true;
		break;
	default:
		break;
	}

	if (!valid_input) {
		kfree(wr_buf);
		DRM_DEBUG_DRIVER("Invalid Input value No HW will be programmed\n");
		return bytes_from_user;
	}

	/* save user force lane_count, link_rate to preferred settings
	 * spread spectrum will not be changed
	 */
	prefer_link_settings.link_spread = link->cur_link_settings.link_spread;
	prefer_link_settings.lane_count = param[0];
	prefer_link_settings.link_rate = param[1];

	dc_link_set_preferred_link_settings(dc, &prefer_link_settings, link);

	kfree(wr_buf);
	return bytes_from_user;
}

/* function: get current DP PHY settings: voltage swing, pre-emphasis,
 * post-cursor2 (defined by VESA DP specification)
 *
 * valid values
 * voltage swing: 0,1,2,3
 * pre-emphasis : 0,1,2,3
 * post cursor2 : 0,1,2,3
 *
 *
 * how to use this debugfs
 *
 * debugfs is located at /sys/kernel/debug/dri/0/DP-x
 *
 * there will be directories, like DP-1, DP-2,DP-3, etc. for DP display
 *
 * To figure out which DP-x is the display for DP to be check,
 * cd DP-x
 * ls -ll
 * There should be debugfs file, like link_settings, phy_settings.
 * cat link_settings
 * from lane_count, link_rate to figure which DP-x is for display to be worked
 * on
 *
 * To get current DP PHY settings,
 * cat phy_settings
 *
 * To change DP PHY settings,
 * echo <voltage_swing> <pre-emphasis> <post_cursor2> > phy_settings
 * for examle, to change voltage swing to 2, pre-emphasis to 3, post_cursor2 to
 * 0,
 * echo 2 3 0 > phy_settings
 *
 * To check if change be applied, get current phy settings by
 * cat phy_settings
 *
 * In case invalid values are set by user, like
 * echo 1 4 0 > phy_settings
 *
 * HW will NOT be programmed by these settings.
 * cat phy_settings will show the previous valid settings.
 */
static ssize_t dp_phy_settings_read(struct file *f, char __user *buf,
				 size_t size, loff_t *pos)
{
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dc_link *link = connector->dc_link;
	char *rd_buf = NULL;
	const uint32_t rd_buf_size = 20;
	uint32_t result = 0;
	int r;

	if (*pos & 3 || size & 3)
		return -EINVAL;

	rd_buf = kcalloc(rd_buf_size, sizeof(char), GFP_KERNEL);
	if (!rd_buf)
		return -EINVAL;

	snprintf(rd_buf, rd_buf_size, "  %d  %d  %d  ",
			link->cur_lane_setting.VOLTAGE_SWING,
			link->cur_lane_setting.PRE_EMPHASIS,
			link->cur_lane_setting.POST_CURSOR2);

	while (size) {
		if (*pos >= rd_buf_size)
			break;

		r = put_user((*(rd_buf + result)), buf);
		if (r)
			return r; /* r = -EFAULT */

		buf += 1;
		size -= 1;
		*pos += 1;
		result += 1;
	}

	kfree(rd_buf);
	return result;
}

static ssize_t dp_phy_settings_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dc_link *link = connector->dc_link;
	struct dc *dc = (struct dc *)link->dc;
	char *wr_buf = NULL;
	char *wr_buf_ptr = NULL;
	uint32_t wr_buf_size = 40;
	int r;
	int bytes_from_user;
	char *sub_str;
	uint8_t param_index = 0;
	long param[3];
	const char delimiter[3] = {' ', '\n', '\0'};
	bool use_prefer_link_setting;
	struct link_training_settings link_lane_settings;

	if (size == 0)
		return 0;

	wr_buf = kcalloc(wr_buf_size, sizeof(char), GFP_KERNEL);
	if (!wr_buf)
		return 0;
	wr_buf_ptr = wr_buf;

	r = copy_from_user(wr_buf_ptr, buf, wr_buf_size);

	/* r is bytes not be copied */
	if (r >= wr_buf_size) {
		kfree(wr_buf);
		DRM_DEBUG_DRIVER("user data not be read\n");
		return 0;
	}

	bytes_from_user = wr_buf_size - r;

	while (isspace(*wr_buf_ptr))
		wr_buf_ptr++;

	while ((*wr_buf_ptr != '\0') && (param_index < 3)) {

		sub_str = strsep(&wr_buf_ptr, delimiter);

		r = kstrtol(sub_str, 16, &param[param_index]);

		if (r)
			DRM_DEBUG_DRIVER("string to int convert error code: %d\n", r);

		param_index++;
		while (isspace(*wr_buf_ptr))
			wr_buf_ptr++;
	}

	if ((param[0] > VOLTAGE_SWING_MAX_LEVEL) ||
			(param[1] > PRE_EMPHASIS_MAX_LEVEL) ||
			(param[2] > POST_CURSOR2_MAX_LEVEL)) {
		kfree(wr_buf);
		DRM_DEBUG_DRIVER("Invalid Input No HW will be programmed\n");
		return bytes_from_user;
	}

	/* get link settings: lane count, link rate */
	use_prefer_link_setting =
		((link->preferred_link_setting.link_rate != LINK_RATE_UNKNOWN) &&
		(link->test_pattern_enabled));

	memset(&link_lane_settings, 0, sizeof(link_lane_settings));

	if (use_prefer_link_setting) {
		link_lane_settings.link_settings.lane_count =
				link->preferred_link_setting.lane_count;
		link_lane_settings.link_settings.link_rate =
				link->preferred_link_setting.link_rate;
		link_lane_settings.link_settings.link_spread =
				link->preferred_link_setting.link_spread;
	} else {
		link_lane_settings.link_settings.lane_count =
				link->cur_link_settings.lane_count;
		link_lane_settings.link_settings.link_rate =
				link->cur_link_settings.link_rate;
		link_lane_settings.link_settings.link_spread =
				link->cur_link_settings.link_spread;
	}

	/* apply phy settings from user */
	for (r = 0; r < link_lane_settings.link_settings.lane_count; r++) {
		link_lane_settings.lane_settings[r].VOLTAGE_SWING =
				(enum dc_voltage_swing) (param[0]);
		link_lane_settings.lane_settings[r].PRE_EMPHASIS =
				(enum dc_pre_emphasis) (param[1]);
		link_lane_settings.lane_settings[r].POST_CURSOR2 =
				(enum dc_post_cursor2) (param[2]);
	}

	/* program ASIC registers and DPCD registers */
	dc_link_set_drive_settings(dc, &link_lane_settings, link);

	kfree(wr_buf);
	return bytes_from_user;
}

/* function description
 *
 * set PHY layer or Link layer test pattern
 * PHY test pattern is used for PHY SI check.
 * Link layer test will not affect PHY SI.
 *
 * Reset Test Pattern:
 * 0 = DP_TEST_PATTERN_VIDEO_MODE
 *
 * PHY test pattern supported:
 * 1 = DP_TEST_PATTERN_D102
 * 2 = DP_TEST_PATTERN_SYMBOL_ERROR
 * 3 = DP_TEST_PATTERN_PRBS7
 * 4 = DP_TEST_PATTERN_80BIT_CUSTOM
 * 5 = DP_TEST_PATTERN_CP2520_1
 * 6 = DP_TEST_PATTERN_CP2520_2 = DP_TEST_PATTERN_HBR2_COMPLIANCE_EYE
 * 7 = DP_TEST_PATTERN_CP2520_3
 *
 * DP PHY Link Training Patterns
 * 8 = DP_TEST_PATTERN_TRAINING_PATTERN1
 * 9 = DP_TEST_PATTERN_TRAINING_PATTERN2
 * a = DP_TEST_PATTERN_TRAINING_PATTERN3
 * b = DP_TEST_PATTERN_TRAINING_PATTERN4
 *
 * DP Link Layer Test pattern
 * c = DP_TEST_PATTERN_COLOR_SQUARES
 * d = DP_TEST_PATTERN_COLOR_SQUARES_CEA
 * e = DP_TEST_PATTERN_VERTICAL_BARS
 * f = DP_TEST_PATTERN_HORIZONTAL_BARS
 * 10= DP_TEST_PATTERN_COLOR_RAMP
 *
 * debugfs phy_test_pattern is located at /syskernel/debug/dri/0/DP-x
 *
 * --- set test pattern
 * echo <test pattern #> > test_pattern
 *
 * If test pattern # is not supported, NO HW programming will be done.
 * for DP_TEST_PATTERN_80BIT_CUSTOM, it needs extra 10 bytes of data
 * for the user pattern. input 10 bytes data are separated by space
 *
 * echo 0x4 0x11 0x22 0x33 0x44 0x55 0x66 0x77 0x88 0x99 0xaa > test_pattern
 *
 * --- reset test pattern
 * echo 0 > test_pattern
 *
 * --- HPD detection is disabled when set PHY test pattern
 *
 * when PHY test pattern (pattern # within [1,7]) is set, HPD pin of HW ASIC
 * is disable. User could unplug DP display from DP connected and plug scope to
 * check test pattern PHY SI.
 * If there is need unplug scope and plug DP display back, do steps below:
 * echo 0 > phy_test_pattern
 * unplug scope
 * plug DP display.
 *
 * "echo 0 > phy_test_pattern" will re-enable HPD pin again so that video sw
 * driver could detect "unplug scope" and "plug DP display"
 */
static ssize_t dp_phy_test_pattern_debugfs_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dc_link *link = connector->dc_link;
	char *wr_buf = NULL;
	char *wr_buf_ptr = NULL;
	uint32_t wr_buf_size = 100;
	uint32_t wr_buf_count = 0;
	int r;
	int bytes_from_user;
	char *sub_str = NULL;
	uint8_t param_index = 0;
	uint8_t param_nums = 0;
	long param[11] = {0x0};
	const char delimiter[3] = {' ', '\n', '\0'};
	enum dp_test_pattern test_pattern = DP_TEST_PATTERN_UNSUPPORTED;
	bool disable_hpd = false;
	bool valid_test_pattern = false;
	/* init with defalut 80bit custom pattern */
	uint8_t custom_pattern[10] = {
			0x1f, 0x7c, 0xf0, 0xc1, 0x07,
			0x1f, 0x7c, 0xf0, 0xc1, 0x07
			};
	struct dc_link_settings prefer_link_settings = {LANE_COUNT_UNKNOWN,
			LINK_RATE_UNKNOWN, LINK_SPREAD_DISABLED};
	struct dc_link_settings cur_link_settings = {LANE_COUNT_UNKNOWN,
			LINK_RATE_UNKNOWN, LINK_SPREAD_DISABLED};
	struct link_training_settings link_training_settings;
	int i;

	if (size == 0)
		return 0;

	wr_buf = kcalloc(wr_buf_size, sizeof(char), GFP_KERNEL);
	if (!wr_buf)
		return 0;
	wr_buf_ptr = wr_buf;

	r = copy_from_user(wr_buf_ptr, buf, wr_buf_size);

	/* r is bytes not be copied */
	if (r >= wr_buf_size) {
		kfree(wr_buf);
		DRM_DEBUG_DRIVER("user data not be read\n");
		return 0;
	}

	bytes_from_user = wr_buf_size - r;

	/* check number of parameters. isspace could not differ space and \n */
	while ((*wr_buf_ptr != 0xa) && (wr_buf_count < wr_buf_size)) {
		/* skip space*/
		while (isspace(*wr_buf_ptr) && (wr_buf_count < wr_buf_size)) {
			wr_buf_ptr++;
			wr_buf_count++;
			}

		if (wr_buf_count == wr_buf_size)
			break;

		/* skip non-space*/
		while ((!isspace(*wr_buf_ptr)) && (wr_buf_count < wr_buf_size)) {
			wr_buf_ptr++;
			wr_buf_count++;
			}

		param_nums++;

		if (wr_buf_count == wr_buf_size)
			break;
	}

	/* max 11 parameters */
	if (param_nums > 11)
		param_nums = 11;

	wr_buf_ptr = wr_buf; /* reset buf pinter */
	wr_buf_count = 0; /* number of char already checked */

	while (isspace(*wr_buf_ptr) && (wr_buf_count < wr_buf_size)) {
		wr_buf_ptr++;
		wr_buf_count++;
	}

	while (param_index < param_nums) {
		/* after strsep, wr_buf_ptr will be moved to after space */
		sub_str = strsep(&wr_buf_ptr, delimiter);

		r = kstrtol(sub_str, 16, &param[param_index]);

		if (r)
			DRM_DEBUG_DRIVER("string to int convert error code: %d\n", r);

		param_index++;
	}

	test_pattern = param[0];

	switch (test_pattern) {
	case DP_TEST_PATTERN_VIDEO_MODE:
	case DP_TEST_PATTERN_COLOR_SQUARES:
	case DP_TEST_PATTERN_COLOR_SQUARES_CEA:
	case DP_TEST_PATTERN_VERTICAL_BARS:
	case DP_TEST_PATTERN_HORIZONTAL_BARS:
	case DP_TEST_PATTERN_COLOR_RAMP:
		valid_test_pattern = true;
		break;

	case DP_TEST_PATTERN_D102:
	case DP_TEST_PATTERN_SYMBOL_ERROR:
	case DP_TEST_PATTERN_PRBS7:
	case DP_TEST_PATTERN_80BIT_CUSTOM:
	case DP_TEST_PATTERN_HBR2_COMPLIANCE_EYE:
	case DP_TEST_PATTERN_TRAINING_PATTERN4:
		disable_hpd = true;
		valid_test_pattern = true;
		break;

	default:
		valid_test_pattern = false;
		test_pattern = DP_TEST_PATTERN_UNSUPPORTED;
		break;
	}

	if (!valid_test_pattern) {
		kfree(wr_buf);
		DRM_DEBUG_DRIVER("Invalid Test Pattern Parameters\n");
		return bytes_from_user;
	}

	if (test_pattern == DP_TEST_PATTERN_80BIT_CUSTOM) {
		for (i = 0; i < 10; i++) {
			if ((uint8_t) param[i + 1] != 0x0)
				break;
		}

		if (i < 10) {
			/* not use default value */
			for (i = 0; i < 10; i++)
				custom_pattern[i] = (uint8_t) param[i + 1];
		}
	}

	/* Usage: set DP physical test pattern using debugfs with normal DP
	 * panel. Then plug out DP panel and connect a scope to measure
	 * For normal video mode and test pattern generated from CRCT,
	 * they are visibile to user. So do not disable HPD.
	 * Video Mode is also set to clear the test pattern, so enable HPD
	 * because it might have been disabled after a test pattern was set.
	 * AUX depends on HPD * sequence dependent, do not move!
	 */
	if (!disable_hpd)
		dc_link_enable_hpd(link);

	prefer_link_settings.lane_count = link->verified_link_cap.lane_count;
	prefer_link_settings.link_rate = link->verified_link_cap.link_rate;
	prefer_link_settings.link_spread = link->verified_link_cap.link_spread;

	cur_link_settings.lane_count = link->cur_link_settings.lane_count;
	cur_link_settings.link_rate = link->cur_link_settings.link_rate;
	cur_link_settings.link_spread = link->cur_link_settings.link_spread;

	link_training_settings.link_settings = cur_link_settings;


	if (test_pattern != DP_TEST_PATTERN_VIDEO_MODE) {
		if (prefer_link_settings.lane_count != LANE_COUNT_UNKNOWN &&
			prefer_link_settings.link_rate !=  LINK_RATE_UNKNOWN &&
			(prefer_link_settings.lane_count != cur_link_settings.lane_count ||
			prefer_link_settings.link_rate != cur_link_settings.link_rate))
			link_training_settings.link_settings = prefer_link_settings;
	}

	for (i = 0; i < (unsigned int)(link_training_settings.link_settings.lane_count); i++)
		link_training_settings.lane_settings[i] = link->cur_lane_setting;

	dc_link_set_test_pattern(
		link,
		test_pattern,
		DP_TEST_PATTERN_COLOR_SPACE_RGB,
		&link_training_settings,
		custom_pattern,
		10);

	/* Usage: Set DP physical test pattern using AMDDP with normal DP panel
	 * Then plug out DP panel and connect a scope to measure DP PHY signal.
	 * Need disable interrupt to avoid SW driver disable DP output. This is
	 * done after the test pattern is set.
	 */
	if (valid_test_pattern && disable_hpd)
		dc_link_disable_hpd(link);

	kfree(wr_buf);

	return bytes_from_user;
}

/**
 * Returns the DMCUB tracebuffer contents.
 * Example usage: cat /sys/kernel/debug/dri/0/amdgpu_dm_dmub_tracebuffer
 */
static int dmub_tracebuffer_show(struct seq_file *m, void *data)
{
	struct amdgpu_device *adev = m->private;
	struct dmub_srv_fb_info *fb_info = adev->dm.dmub_fb_info;
	struct dmub_debugfs_trace_entry *entries;
	uint8_t *tbuf_base;
	uint32_t tbuf_size, max_entries, num_entries, i;

	if (!fb_info)
		return 0;

	tbuf_base = (uint8_t *)fb_info->fb[DMUB_WINDOW_5_TRACEBUFF].cpu_addr;
	if (!tbuf_base)
		return 0;

	tbuf_size = fb_info->fb[DMUB_WINDOW_5_TRACEBUFF].size;
	max_entries = (tbuf_size - sizeof(struct dmub_debugfs_trace_header)) /
		      sizeof(struct dmub_debugfs_trace_entry);

	num_entries =
		((struct dmub_debugfs_trace_header *)tbuf_base)->entry_count;

	num_entries = min(num_entries, max_entries);

	entries = (struct dmub_debugfs_trace_entry
			   *)(tbuf_base +
			      sizeof(struct dmub_debugfs_trace_header));

	for (i = 0; i < num_entries; ++i) {
		struct dmub_debugfs_trace_entry *entry = &entries[i];

		seq_printf(m,
			   "trace_code=%u tick_count=%u param0=%u param1=%u\n",
			   entry->trace_code, entry->tick_count, entry->param0,
			   entry->param1);
	}

	return 0;
}

/*
 * Returns the current and maximum output bpc for the connector.
 * Example usage: cat /sys/kernel/debug/dri/0/DP-1/output_bpc
 */
static int output_bpc_show(struct seq_file *m, void *data)
{
	struct drm_connector *connector = m->private;
	struct drm_device *dev = connector->dev;
	struct drm_crtc *crtc = NULL;
	struct dm_crtc_state *dm_crtc_state = NULL;
	int res = -ENODEV;
	unsigned int bpc;

	mutex_lock(&dev->mode_config.mutex);
	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);

	if (connector->state == NULL)
		goto unlock;

	crtc = connector->state->crtc;
	if (crtc == NULL)
		goto unlock;

	drm_modeset_lock(&crtc->mutex, NULL);
	if (crtc->state == NULL)
		goto unlock;

	dm_crtc_state = to_dm_crtc_state(crtc->state);
	if (dm_crtc_state->stream == NULL)
		goto unlock;

	switch (dm_crtc_state->stream->timing.display_color_depth) {
	case COLOR_DEPTH_666:
		bpc = 6;
		break;
	case COLOR_DEPTH_888:
		bpc = 8;
		break;
	case COLOR_DEPTH_101010:
		bpc = 10;
		break;
	case COLOR_DEPTH_121212:
		bpc = 12;
		break;
	case COLOR_DEPTH_161616:
		bpc = 16;
		break;
	default:
		goto unlock;
	}

	seq_printf(m, "Current: %u\n", bpc);
	seq_printf(m, "Maximum: %u\n", connector->display_info.bpc);
	res = 0;

unlock:
	if (crtc)
		drm_modeset_unlock(&crtc->mutex);

	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	mutex_unlock(&dev->mode_config.mutex);

	return res;
}

/*
 * Returns the min and max vrr vfreq through the connector's debugfs file.
 * Example usage: cat /sys/kernel/debug/dri/0/DP-1/vrr_range
 */
static int vrr_range_show(struct seq_file *m, void *data)
{
	struct drm_connector *connector = m->private;
	struct amdgpu_dm_connector *aconnector = to_amdgpu_dm_connector(connector);

	if (connector->status != connector_status_connected)
		return -ENODEV;

	seq_printf(m, "Min: %u\n", (unsigned int)aconnector->min_vfreq);
	seq_printf(m, "Max: %u\n", (unsigned int)aconnector->max_vfreq);

	return 0;
}

/* function description
 *
 * generic SDP message access for testing
 *
 * debugfs sdp_message is located at /syskernel/debug/dri/0/DP-x
 *
 * SDP header
 * Hb0 : Secondary-Data Packet ID
 * Hb1 : Secondary-Data Packet type
 * Hb2 : Secondary-Data-packet-specific header, Byte 0
 * Hb3 : Secondary-Data-packet-specific header, Byte 1
 *
 * for using custom sdp message: input 4 bytes SDP header and 32 bytes raw data
 */
static ssize_t dp_sdp_message_debugfs_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	int r;
	uint8_t data[36];
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dm_crtc_state *acrtc_state;
	uint32_t write_size = 36;

	if (connector->base.status != connector_status_connected)
		return -ENODEV;

	if (size == 0)
		return 0;

	acrtc_state = to_dm_crtc_state(connector->base.state->crtc->state);

	r = copy_from_user(data, buf, write_size);

	write_size -= r;

	dc_stream_send_dp_sdp(acrtc_state->stream, data, write_size);

	return write_size;
}

static ssize_t dp_dpcd_address_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	int r;
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;

	if (size < sizeof(connector->debugfs_dpcd_address))
		return 0;

	r = copy_from_user(&connector->debugfs_dpcd_address,
			buf, sizeof(connector->debugfs_dpcd_address));

	return size - r;
}

static ssize_t dp_dpcd_size_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	int r;
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;

	if (size < sizeof(connector->debugfs_dpcd_size))
		return 0;

	r = copy_from_user(&connector->debugfs_dpcd_size,
			buf, sizeof(connector->debugfs_dpcd_size));

	if (connector->debugfs_dpcd_size > 256)
		connector->debugfs_dpcd_size = 0;

	return size - r;
}

static ssize_t dp_dpcd_data_write(struct file *f, const char __user *buf,
				 size_t size, loff_t *pos)
{
	int r;
	char *data;
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dc_link *link = connector->dc_link;
	uint32_t write_size = connector->debugfs_dpcd_size;

	if (size < write_size)
		return 0;

	data = kzalloc(write_size, GFP_KERNEL);
	if (!data)
		return 0;

	r = copy_from_user(data, buf, write_size);

	dm_helpers_dp_write_dpcd(link->ctx, link,
			connector->debugfs_dpcd_address, data, write_size - r);
	kfree(data);
	return write_size - r;
}

static ssize_t dp_dpcd_data_read(struct file *f, char __user *buf,
				 size_t size, loff_t *pos)
{
	int r;
	char *data;
	struct amdgpu_dm_connector *connector = file_inode(f)->i_private;
	struct dc_link *link = connector->dc_link;
	uint32_t read_size = connector->debugfs_dpcd_size;

	if (size < read_size)
		return 0;

	data = kzalloc(read_size, GFP_KERNEL);
	if (!data)
		return 0;

	dm_helpers_dp_read_dpcd(link->ctx, link,
			connector->debugfs_dpcd_address, data, read_size);

	r = copy_to_user(buf, data, read_size);

	kfree(data);
	return read_size - r;
}

DEFINE_SHOW_ATTRIBUTE(dmub_tracebuffer);
DEFINE_SHOW_ATTRIBUTE(output_bpc);
DEFINE_SHOW_ATTRIBUTE(vrr_range);

static const struct file_operations dp_link_settings_debugfs_fops = {
	.owner = THIS_MODULE,
	.read = dp_link_settings_read,
	.write = dp_link_settings_write,
	.llseek = default_llseek
};

static const struct file_operations dp_phy_settings_debugfs_fop = {
	.owner = THIS_MODULE,
	.read = dp_phy_settings_read,
	.write = dp_phy_settings_write,
	.llseek = default_llseek
};

static const struct file_operations dp_phy_test_pattern_fops = {
	.owner = THIS_MODULE,
	.write = dp_phy_test_pattern_debugfs_write,
	.llseek = default_llseek
};

static const struct file_operations sdp_message_fops = {
	.owner = THIS_MODULE,
	.write = dp_sdp_message_debugfs_write,
	.llseek = default_llseek
};

static const struct file_operations dp_dpcd_address_debugfs_fops = {
	.owner = THIS_MODULE,
	.write = dp_dpcd_address_write,
	.llseek = default_llseek
};

static const struct file_operations dp_dpcd_size_debugfs_fops = {
	.owner = THIS_MODULE,
	.write = dp_dpcd_size_write,
	.llseek = default_llseek
};

static const struct file_operations dp_dpcd_data_debugfs_fops = {
	.owner = THIS_MODULE,
	.read = dp_dpcd_data_read,
	.write = dp_dpcd_data_write,
	.llseek = default_llseek
};

static const struct {
	char *name;
	const struct file_operations *fops;
} dp_debugfs_entries[] = {
		{"link_settings", &dp_link_settings_debugfs_fops},
		{"phy_settings", &dp_phy_settings_debugfs_fop},
		{"test_pattern", &dp_phy_test_pattern_fops},
		{"output_bpc", &output_bpc_fops},
		{"vrr_range", &vrr_range_fops},
		{"sdp_message", &sdp_message_fops},
		{"aux_dpcd_address", &dp_dpcd_address_debugfs_fops},
		{"aux_dpcd_size", &dp_dpcd_size_debugfs_fops},
		{"aux_dpcd_data", &dp_dpcd_data_debugfs_fops}
};

/*
 * Force YUV420 output if available from the given mode
 */
static int force_yuv420_output_set(void *data, u64 val)
{
	struct amdgpu_dm_connector *connector = data;

	connector->force_yuv420_output = (bool)val;

	return 0;
}

/*
 * Check if YUV420 is forced when available from the given mode
 */
static int force_yuv420_output_get(void *data, u64 *val)
{
	struct amdgpu_dm_connector *connector = data;

	*val = connector->force_yuv420_output;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(force_yuv420_output_fops, force_yuv420_output_get,
			 force_yuv420_output_set, "%llu\n");

/*
 *  Read PSR state
 */
static int psr_get(void *data, u64 *val)
{
	struct amdgpu_dm_connector *connector = data;
	struct dc_link *link = connector->dc_link;
	uint32_t psr_state = 0;

	dc_link_get_psr_state(link, &psr_state);

	*val = psr_state;

	return 0;
}


DEFINE_DEBUGFS_ATTRIBUTE(psr_fops, psr_get, NULL, "%llu\n");

void connector_debugfs_init(struct amdgpu_dm_connector *connector)
{
	int i;
	struct dentry *dir = connector->base.debugfs_entry;

	if (connector->base.connector_type == DRM_MODE_CONNECTOR_DisplayPort ||
	    connector->base.connector_type == DRM_MODE_CONNECTOR_eDP) {
		for (i = 0; i < ARRAY_SIZE(dp_debugfs_entries); i++) {
			debugfs_create_file(dp_debugfs_entries[i].name,
					    0644, dir, connector,
					    dp_debugfs_entries[i].fops);
		}
	}
	if (connector->base.connector_type == DRM_MODE_CONNECTOR_eDP)
		debugfs_create_file_unsafe("psr_state", 0444, dir, connector, &psr_fops);

	debugfs_create_file_unsafe("force_yuv420_output", 0644, dir, connector,
				   &force_yuv420_output_fops);

}

/*
 * Writes DTN log state to the user supplied buffer.
 * Example usage: cat /sys/kernel/debug/dri/0/amdgpu_dm_dtn_log
 */
static ssize_t dtn_log_read(
	struct file *f,
	char __user *buf,
	size_t size,
	loff_t *pos)
{
	struct amdgpu_device *adev = file_inode(f)->i_private;
	struct dc *dc = adev->dm.dc;
	struct dc_log_buffer_ctx log_ctx = { 0 };
	ssize_t result = 0;

	if (!buf || !size)
		return -EINVAL;

	if (!dc->hwss.log_hw_state)
		return 0;

	dc->hwss.log_hw_state(dc, &log_ctx);

	if (*pos < log_ctx.pos) {
		size_t to_copy = log_ctx.pos - *pos;

		to_copy = min(to_copy, size);

		if (!copy_to_user(buf, log_ctx.buf + *pos, to_copy)) {
			*pos += to_copy;
			result = to_copy;
		}
	}

	kfree(log_ctx.buf);

	return result;
}

/*
 * Writes DTN log state to dmesg when triggered via a write.
 * Example usage: echo 1 > /sys/kernel/debug/dri/0/amdgpu_dm_dtn_log
 */
static ssize_t dtn_log_write(
	struct file *f,
	const char __user *buf,
	size_t size,
	loff_t *pos)
{
	struct amdgpu_device *adev = file_inode(f)->i_private;
	struct dc *dc = adev->dm.dc;

	/* Write triggers log output via dmesg. */
	if (size == 0)
		return 0;

	if (dc->hwss.log_hw_state)
		dc->hwss.log_hw_state(dc, NULL);

	return size;
}

/*
 * Backlight at this moment.  Read only.
 * As written to display, taking ABM and backlight lut into account.
 * Ranges from 0x0 to 0x10000 (= 100% PWM)
 */
static int current_backlight_read(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct amdgpu_device *adev = dev->dev_private;
	struct dc *dc = adev->dm.dc;
	unsigned int backlight = dc_get_current_backlight_pwm(dc);

	seq_printf(m, "0x%x\n", backlight);
	return 0;
}

/*
 * Backlight value that is being approached.  Read only.
 * As written to display, taking ABM and backlight lut into account.
 * Ranges from 0x0 to 0x10000 (= 100% PWM)
 */
static int target_backlight_read(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct amdgpu_device *adev = dev->dev_private;
	struct dc *dc = adev->dm.dc;
	unsigned int backlight = dc_get_target_backlight_pwm(dc);

	seq_printf(m, "0x%x\n", backlight);
	return 0;
}

static int mst_topo(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	struct amdgpu_dm_connector *aconnector;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		if (connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort)
			continue;

		aconnector = to_amdgpu_dm_connector(connector);

		seq_printf(m, "\nMST topology for connector %d\n", aconnector->connector_id);
		drm_dp_mst_dump_topology(m, &aconnector->mst_mgr);
	}
	drm_connector_list_iter_end(&conn_iter);

	return 0;
}

static const struct drm_info_list amdgpu_dm_debugfs_list[] = {
	{"amdgpu_current_backlight_pwm", &current_backlight_read},
	{"amdgpu_target_backlight_pwm", &target_backlight_read},
	{"amdgpu_mst_topology", &mst_topo},
};

/*
 * Sets the DC visual confirm debug option from the given string.
 * Example usage: echo 1 > /sys/kernel/debug/dri/0/amdgpu_visual_confirm
 */
static int visual_confirm_set(void *data, u64 val)
{
	struct amdgpu_device *adev = data;

	adev->dm.dc->debug.visual_confirm = (enum visual_confirm)val;

	return 0;
}

/*
 * Reads the DC visual confirm debug option value into the given buffer.
 * Example usage: cat /sys/kernel/debug/dri/0/amdgpu_dm_visual_confirm
 */
static int visual_confirm_get(void *data, u64 *val)
{
	struct amdgpu_device *adev = data;

	*val = adev->dm.dc->debug.visual_confirm;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(visual_confirm_fops, visual_confirm_get,
			 visual_confirm_set, "%llu\n");

int dtn_debugfs_init(struct amdgpu_device *adev)
{
	static const struct file_operations dtn_log_fops = {
		.owner = THIS_MODULE,
		.read = dtn_log_read,
		.write = dtn_log_write,
		.llseek = default_llseek
	};

	struct drm_minor *minor = adev->ddev->primary;
	struct dentry *root = minor->debugfs_root;
	int ret;

	ret = amdgpu_debugfs_add_files(adev, amdgpu_dm_debugfs_list,
				ARRAY_SIZE(amdgpu_dm_debugfs_list));
	if (ret)
		return ret;

	debugfs_create_file("amdgpu_dm_dtn_log", 0644, root, adev,
			    &dtn_log_fops);

	debugfs_create_file_unsafe("amdgpu_dm_visual_confirm", 0644, root, adev,
				   &visual_confirm_fops);

	debugfs_create_file_unsafe("amdgpu_dm_dmub_tracebuffer", 0644, root,
				   adev, &dmub_tracebuffer_fops);

	return 0;
}
