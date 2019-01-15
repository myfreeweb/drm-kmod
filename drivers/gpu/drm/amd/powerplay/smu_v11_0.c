/*
 * Copyright 2019 Advanced Micro Devices, Inc.
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
 */

#include "pp_debug.h"
#include <linux/firmware.h>
#include "amdgpu.h"
#include "amdgpu_smu.h"
#include "atomfirmware.h"
#include "amdgpu_atomfirmware.h"
#include "smu_v11_0.h"
#include "smu_v11_0_ppsmc.h"
#include "smu11_driver_if.h"
#include "soc15_common.h"
#include "atom.h"
#include "vega20_ppt.h"

#include "asic_reg/thm/thm_11_0_2_offset.h"
#include "asic_reg/thm/thm_11_0_2_sh_mask.h"
#include "asic_reg/mp/mp_9_0_offset.h"
#include "asic_reg/mp/mp_9_0_sh_mask.h"
#include "asic_reg/nbio/nbio_7_4_offset.h"

MODULE_FIRMWARE("amdgpu/vega20_smc.bin");

static int smu_v11_0_send_msg_without_waiting(struct smu_context *smu,
					      uint16_t msg)
{
	struct amdgpu_device *adev = smu->adev;
	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_66, msg);
	return 0;
}

static int smu_v11_0_read_arg(struct smu_context *smu, uint32_t *arg)
{
	struct amdgpu_device *adev = smu->adev;

	*arg = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_82);
	return 0;
}

static int smu_v11_0_wait_for_response(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	uint32_t cur_value, i;

	for (i = 0; i < adev->usec_timeout; i++) {
		cur_value = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90);
		if ((cur_value & MP1_C2PMSG_90__CONTENT_MASK) != 0)
			break;
		udelay(1);
	}

	/* timeout means wrong logic */
	if (i == adev->usec_timeout)
		return -ETIME;

	return RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90) ==  PPSMC_Result_OK ? 0:-EIO;
}

static int smu_v11_0_send_msg(struct smu_context *smu, uint16_t msg)
{
	struct amdgpu_device *adev = smu->adev;
	int ret = 0;

	smu_v11_0_wait_for_response(smu);

	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90, 0);

	smu_v11_0_send_msg_without_waiting(smu, msg);

	ret = smu_v11_0_wait_for_response(smu);

	if (ret)
		pr_err("Failed to send message 0x%x, response 0x%x\n", msg,
		       ret);

	return ret;

}

static int
smu_v11_0_send_msg_with_param(struct smu_context *smu, uint16_t msg,
			      uint32_t param)
{

	struct amdgpu_device *adev = smu->adev;
	int ret = 0;

	ret = smu_v11_0_wait_for_response(smu);
	if (ret)
		pr_err("Failed to send message 0x%x, response 0x%x\n", msg,
		       ret);

	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90, 0);

	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_82, param);

	smu_v11_0_send_msg_without_waiting(smu, msg);

	ret = smu_v11_0_wait_for_response(smu);
	if (ret)
		pr_err("Failed to send message 0x%x, response 0x%x\n", msg,
		       ret);

	return ret;
}

static int smu_v11_0_init_microcode(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;

	return 0;
}

static int smu_v11_0_load_microcode(struct smu_context *smu)
{
	return 0;
}

static int smu_v11_0_check_fw_status(struct smu_context *smu)
{
	return 0;
}

static int smu_v11_0_read_pptable_from_vbios(struct smu_context *smu)
{
	int ret, index;
	uint16_t size;
	uint8_t frev, crev;
	struct smu_11_0_powerplay_table *table;

	index = get_index_into_master_table(atom_master_list_of_data_tables_v2_1,
					    powerplayinfo);

	ret = smu_get_atom_data_table(smu, index, &size, &frev, &crev,
				      (uint8_t **)&table);
	if (ret)
		return ret;

	smu->smu_table.power_play_table = table;
	smu->smu_table.power_play_table_size = size;

	return 0;
}

static int smu_v11_0_init_dpm_context(struct smu_context *smu)
{
	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;

	if (smu_dpm->dpm_context || smu_dpm->dpm_context_size != 0)
		return -EINVAL;

	smu_dpm->dpm_context = kzalloc(sizeof(struct smu_11_0_dpm_context), GFP_KERNEL);
	if (!smu_dpm->dpm_context)
		return -ENOMEM;
	smu_dpm->dpm_context_size = sizeof(struct smu_11_0_dpm_context);

	return 0;
}

static int smu_v11_0_fini_dpm_context(struct smu_context *smu)
{
	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;

	if (!smu_dpm->dpm_context || smu_dpm->dpm_context_size == 0)
		return -EINVAL;

	kfree(smu_dpm->dpm_context);
	smu_dpm->dpm_context = NULL;
	smu_dpm->dpm_context_size = 0;

	return 0;
}

static int smu_v11_0_init_smc_tables(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *tables = NULL;
	int ret = 0;

	if (smu_table->tables || smu_table->table_count != 0)
		return -EINVAL;

	tables = kcalloc(TABLE_COUNT, sizeof(struct smu_table), GFP_KERNEL);
	if (!tables)
		return -ENOMEM;

	smu_table->tables = tables;
	smu_table->table_count = TABLE_COUNT;

	SMU_TABLE_INIT(tables, TABLE_PPTABLE, sizeof(PPTable_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);
	SMU_TABLE_INIT(tables, TABLE_WATERMARKS, sizeof(Watermarks_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);
	SMU_TABLE_INIT(tables, TABLE_SMU_METRICS, sizeof(SmuMetrics_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);
	SMU_TABLE_INIT(tables, TABLE_OVERDRIVE, sizeof(OverDriveTable_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	ret = smu_v11_0_init_dpm_context(smu);
	if (ret)
		return ret;

	return 0;
}

static int smu_v11_0_fini_smc_tables(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	int ret = 0;

	if (!smu_table->tables || smu_table->table_count == 0)
		return -EINVAL;

	kfree(smu_table->tables);
	smu_table->tables = NULL;
	smu_table->table_count = 0;

	ret = smu_v11_0_fini_dpm_context(smu);
	if (ret)
		return ret;
	return 0;

}

static int smu_v11_0_init_power(struct smu_context *smu)
{
	struct smu_power_context *smu_power = &smu->smu_power;

	if (smu_power->power_context || smu_power->power_context_size != 0)
		return -EINVAL;

	smu_power->power_context = kzalloc(sizeof(struct smu_11_0_dpm_context),
					   GFP_KERNEL);
	if (!smu_power->power_context)
		return -ENOMEM;
	smu_power->power_context_size = sizeof(struct smu_11_0_dpm_context);

	return 0;
}

static int smu_v11_0_fini_power(struct smu_context *smu)
{
	struct smu_power_context *smu_power = &smu->smu_power;

	if (!smu_power->power_context || smu_power->power_context_size == 0)
		return -EINVAL;

	kfree(smu_power->power_context);
	smu_power->power_context = NULL;
	smu_power->power_context_size = 0;

	return 0;
}

int smu_v11_0_get_vbios_bootup_values(struct smu_context *smu)
{
	int ret, index;
	uint16_t size;
	uint8_t frev, crev;
	struct atom_common_table_header *header;
	struct atom_firmware_info_v3_3 *v_3_3;
	struct atom_firmware_info_v3_1 *v_3_1;

	index = get_index_into_master_table(atom_master_list_of_data_tables_v2_1,
					    firmwareinfo);

	ret = smu_get_atom_data_table(smu, index, &size, &frev, &crev,
				      (uint8_t **)&header);
	if (ret)
		return ret;

	if (header->format_revision != 3) {
		pr_err("unknown atom_firmware_info version! for smu11\n");
		return -EINVAL;
	}

	switch (header->content_revision) {
	case 0:
	case 1:
	case 2:
		v_3_1 = (struct atom_firmware_info_v3_1 *)header;
		smu->smu_table.boot_values.revision = v_3_1->firmware_revision;
		smu->smu_table.boot_values.gfxclk = v_3_1->bootup_sclk_in10khz;
		smu->smu_table.boot_values.uclk = v_3_1->bootup_mclk_in10khz;
		smu->smu_table.boot_values.socclk = 0;
		smu->smu_table.boot_values.dcefclk = 0;
		smu->smu_table.boot_values.vddc = v_3_1->bootup_vddc_mv;
		smu->smu_table.boot_values.vddci = v_3_1->bootup_vddci_mv;
		smu->smu_table.boot_values.mvddc = v_3_1->bootup_mvddc_mv;
		smu->smu_table.boot_values.vdd_gfx = v_3_1->bootup_vddgfx_mv;
		smu->smu_table.boot_values.cooling_id = v_3_1->coolingsolution_id;
		smu->smu_table.boot_values.pp_table_id = 0;
		break;
	case 3:
	default:
		v_3_3 = (struct atom_firmware_info_v3_3 *)header;
		smu->smu_table.boot_values.revision = v_3_3->firmware_revision;
		smu->smu_table.boot_values.gfxclk = v_3_3->bootup_sclk_in10khz;
		smu->smu_table.boot_values.uclk = v_3_3->bootup_mclk_in10khz;
		smu->smu_table.boot_values.socclk = 0;
		smu->smu_table.boot_values.dcefclk = 0;
		smu->smu_table.boot_values.vddc = v_3_3->bootup_vddc_mv;
		smu->smu_table.boot_values.vddci = v_3_3->bootup_vddci_mv;
		smu->smu_table.boot_values.mvddc = v_3_3->bootup_mvddc_mv;
		smu->smu_table.boot_values.vdd_gfx = v_3_3->bootup_vddgfx_mv;
		smu->smu_table.boot_values.cooling_id = v_3_3->coolingsolution_id;
		smu->smu_table.boot_values.pp_table_id = v_3_3->pplib_pptable_id;
	}

	return 0;
}

static int smu_v11_0_get_clk_info_from_vbios(struct smu_context *smu)
{
	int ret, index;
	struct amdgpu_device *adev = smu->adev;
	struct atom_get_smu_clock_info_parameters_v3_1 input = {0};
	struct atom_get_smu_clock_info_output_parameters_v3_1 *output;

	input.clk_id = SMU11_SYSPLL0_SOCCLK_ID;
	input.command = GET_SMU_CLOCK_INFO_V3_1_GET_CLOCK_FREQ;
	index = get_index_into_master_table(atom_master_list_of_command_functions_v2_1,
					    getsmuclockinfo);

	ret = amdgpu_atom_execute_table(adev->mode_info.atom_context, index,
					(uint32_t *)&input);
	if (ret)
		return -EINVAL;

	output = (struct atom_get_smu_clock_info_output_parameters_v3_1 *)&input;
	smu->smu_table.boot_values.socclk = le32_to_cpu(output->atom_smu_outputclkfreq.smu_clock_freq_hz) / 10000;

	memset(&input, 0, sizeof(input));
	input.clk_id = SMU11_SYSPLL0_DCEFCLK_ID;
	input.command = GET_SMU_CLOCK_INFO_V3_1_GET_CLOCK_FREQ;
	index = get_index_into_master_table(atom_master_list_of_command_functions_v2_1,
					    getsmuclockinfo);

	ret = amdgpu_atom_execute_table(adev->mode_info.atom_context, index,
					(uint32_t *)&input);
	if (ret)
		return -EINVAL;

	output = (struct atom_get_smu_clock_info_output_parameters_v3_1 *)&input;
	smu->smu_table.boot_values.dcefclk = le32_to_cpu(output->atom_smu_outputclkfreq.smu_clock_freq_hz) / 10000;

	return 0;
}

static int smu_v11_0_notify_memory_pool_location(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *memory_pool = &smu_table->memory_pool;
	int ret = 0;
	uint64_t address;
	uint32_t address_low, address_high;

	if (memory_pool->size == 0 || memory_pool->cpu_addr == NULL)
		return ret;

	address = (uint64_t)memory_pool->cpu_addr;
	address_high = (uint32_t)upper_32_bits(address);
	address_low  = (uint32_t)lower_32_bits(address);

	ret = smu_send_smc_msg_with_param(smu,
					  PPSMC_MSG_SetSystemVirtualDramAddrHigh,
					  address_high);
	if (ret)
		return ret;
	ret = smu_send_smc_msg_with_param(smu,
					  PPSMC_MSG_SetSystemVirtualDramAddrLow,
					  address_low);
	if (ret)
		return ret;

	address = memory_pool->mc_address;
	address_high = (uint32_t)upper_32_bits(address);
	address_low  = (uint32_t)lower_32_bits(address);

	ret = smu_send_smc_msg_with_param(smu, PPSMC_MSG_DramLogSetDramAddrHigh,
					  address_high);
	if (ret)
		return ret;
	ret = smu_send_smc_msg_with_param(smu, PPSMC_MSG_DramLogSetDramAddrLow,
					  address_low);
	if (ret)
		return ret;
	ret = smu_send_smc_msg_with_param(smu, PPSMC_MSG_DramLogSetDramSize,
					  (uint32_t)memory_pool->size);
	if (ret)
		return ret;

	return ret;
}

static int smu_v11_0_parse_pptable(struct smu_context *smu)
{
	int ret;

	struct smu_table_context *table_context = &smu->smu_table;

	if (table_context->driver_pptable)
		return -EINVAL;

	table_context->driver_pptable = kzalloc(sizeof(PPTable_t), GFP_KERNEL);

	if (!table_context->driver_pptable)
		return -ENOMEM;

	ret = smu_store_powerplay_table(smu);
	if (ret)
		return -EINVAL;

	ret = smu_append_powerplay_table(smu);

	return ret;
}

static int smu_v11_0_populate_smc_pptable(struct smu_context *smu)
{
	int ret;

	ret = smu_set_default_dpm_table(smu);

	return ret;
}

static int smu_v11_0_copy_table_to_smc(struct smu_context *smu,
				       uint32_t table_id)
{
	struct smu_table_context *table_context = &smu->smu_table;
	struct smu_table *driver_pptable = &smu->smu_table.tables[table_id];
	int ret = 0;

	if (table_id >= TABLE_COUNT) {
		pr_err("Invalid SMU Table ID for smu11!");
		return -EINVAL;
	}

	if (!driver_pptable->cpu_addr) {
		pr_err("Invalid virtual address for smu11!");
		return -EINVAL;
	}
	if (!driver_pptable->mc_address) {
		pr_err("Invalid MC address for smu11!");
		return -EINVAL;
	}
	if (!driver_pptable->size) {
		pr_err("Invalid SMU Table size for smu11!");
		return -EINVAL;
	}

	memcpy(driver_pptable->cpu_addr, table_context->driver_pptable,
	       driver_pptable->size);

	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_SetDriverDramAddrHigh,
			upper_32_bits(driver_pptable->mc_address));
	if (ret) {
		pr_err("[CopyTableToSMC] Attempt to Set Dram Addr High Failed!");
		return ret;
	}
	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_SetDriverDramAddrLow,
			lower_32_bits(driver_pptable->mc_address));
	if (ret) {
		pr_err("[CopyTableToSMC] Attempt to Set Dram Addr Low Failed!");
		return ret;
	}
	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_TransferTableDram2Smu,
					  table_id);
	if (ret) {
		pr_err("[CopyTableToSMC] Attempt to Transfer Table To SMU Failed!");
		return ret;
	}

	return 0;
}

static int smu_v11_0_write_pptable(struct smu_context *smu)
{
	int ret = 0;

	ret = smu_v11_0_copy_table_to_smc(smu, TABLE_PPTABLE);

	return ret;
}

static int smu_v11_0_set_min_dcef_deep_sleep(struct smu_context *smu)
{
	int ret = 0;
	struct smu_table_context *table_context = &smu->smu_table;

	if (!table_context)
		return -EINVAL;

	ret = smu_send_smc_msg_with_param(smu,
					  SMU_MSG_SetMinDeepSleepDcefclk,
					  table_context->boot_values.dcefclk / 100);
	if (ret)
		pr_err("SMU11 attempt to set divider for DCEFCLK Failed!");

	return ret;
}

static int smu_v11_0_set_tool_table_location(struct smu_context *smu)
{
	int ret = 0;
	struct smu_table *tool_table = &smu->smu_table.tables[TABLE_PMSTATUSLOG];

	if (tool_table->mc_address) {
		ret = smu_send_smc_msg_with_param(smu,
				SMU_MSG_SetToolsDramAddrHigh,
				upper_32_bits(tool_table->mc_address));
		if (!ret)
			ret = smu_send_smc_msg_with_param(smu,
				SMU_MSG_SetToolsDramAddrLow,
				lower_32_bits(tool_table->mc_address));
	}

	return ret;
}

static int smu_v11_0_init_display(struct smu_context *smu)
{
	int ret = 0;
	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_NumOfDisplays, 0);
	return ret;
}

static int smu_v11_0_set_allowed_mask(struct smu_context *smu)
{
	struct smu_feature *feature = &smu->smu_feature;
	int ret = 0;
	uint32_t feature_mask[2];

	if (bitmap_empty(feature->allowed, SMU_FEATURE_MAX) || feature->feature_num < 64)
		return -EINVAL;

	bitmap_copy((unsigned long *)feature_mask, feature->allowed, 64);

	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_SetAllowedFeaturesMaskHigh,
					  feature_mask[1]);
	if (ret)
		return ret;

	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_SetAllowedFeaturesMaskLow,
					  feature_mask[0]);
	if (ret)
		return ret;

	return ret;
}

static int smu_v11_0_get_enabled_mask(struct smu_context *smu,
				      uint32_t *feature_mask, uint32_t num)
{
	uint32_t feature_mask_high = 0, feature_mask_low = 0;
	int ret = 0;

	if (!feature_mask || num < 2)
		return -EINVAL;

	ret = smu_send_smc_msg(smu, SMU_MSG_GetEnabledSmuFeaturesHigh);
	if (ret)
		return ret;
	ret = smu_read_smc_arg(smu, &feature_mask_high);
	if (ret)
		return ret;

	ret = smu_send_smc_msg(smu, SMU_MSG_GetEnabledSmuFeaturesLow);
	if (ret)
		return ret;
	ret = smu_read_smc_arg(smu, &feature_mask_low);
	if (ret)
		return ret;

	feature_mask[0] = feature_mask_low;
	feature_mask[1] = feature_mask_high;

	return ret;
}

static int smu_v11_0_enable_all_mask(struct smu_context *smu)
{
	struct smu_feature *feature = &smu->smu_feature;
	uint32_t feature_mask[2];
	int ret = 0;

	ret = smu_send_smc_msg(smu, SMU_MSG_EnableAllSmuFeatures);
	if (ret)
		return ret;
	ret = smu_feature_get_enabled_mask(smu, feature_mask, 2);
	if (ret)
		return ret;

	bitmap_copy(feature->enabled, (unsigned long *)&feature_mask,
		    feature->feature_num);
	bitmap_copy(feature->supported, (unsigned long *)&feature_mask,
		    feature->feature_num);

	return ret;
}

static int smu_v11_0_disable_all_mask(struct smu_context *smu)
{
	struct smu_feature *feature = &smu->smu_feature;
	uint32_t feature_mask[2];
	int ret = 0;

	ret = smu_send_smc_msg(smu, SMU_MSG_DisableAllSmuFeatures);
	if (ret)
		return ret;
	ret = smu_feature_get_enabled_mask(smu, feature_mask, 2);
	if (ret)
		return ret;

	bitmap_copy(feature->enabled, (unsigned long *)&feature_mask,
		    feature->feature_num);
	bitmap_copy(feature->supported, (unsigned long *)&feature_mask,
		    feature->feature_num);

	return ret;
}

static int smu_v11_0_notify_display_change(struct smu_context *smu)
{
	int ret = 0;

	if (smu_feature_is_enabled(smu, FEATURE_DPM_UCLK_BIT))
	    ret = smu_send_smc_msg_with_param(smu, SMU_MSG_SetUclkFastSwitch, 1);

	return ret;
}

static int smu_v11_0_get_power_limit(struct smu_context *smu)
{
	int ret;
	uint32_t power_limit_value;

	ret = smu_send_smc_msg_with_param(smu,
			SMU_MSG_GetPptLimit,
			POWER_SOURCE_AC << 16);
	if (ret) {
		pr_err("[GetPptLimit] get default PPT limit failed!");
		return ret;
	}

	smu_read_smc_arg(smu, &power_limit_value);
	smu->power_limit = smu->default_power_limit = power_limit_value;

	return 0;
}

static int smu_v11_0_get_current_clk_freq(struct smu_context *smu, uint32_t clk_id, uint32_t *value)
{
	int ret = 0;
	uint32_t freq;

	if (clk_id >= PPCLK_COUNT || !value)
		return -EINVAL;

	ret = smu_send_smc_msg_with_param(smu,
			SMU_MSG_GetDpmClockFreq, (clk_id << 16));
	if (ret)
		return ret;

	ret = smu_read_smc_arg(smu, &freq);
	if (ret)
		return ret;

	freq *= 100;
	*value = freq;

	return ret;
}

static const struct smu_funcs smu_v11_0_funcs = {
	.init_microcode = smu_v11_0_init_microcode,
	.load_microcode = smu_v11_0_load_microcode,
	.check_fw_status = smu_v11_0_check_fw_status,
	.check_fw_version = smu_v11_0_check_fw_version,
	.send_smc_msg = smu_v11_0_send_msg,
	.send_smc_msg_with_param = smu_v11_0_send_msg_with_param,
	.read_pptable_from_vbios = smu_v11_0_read_pptable_from_vbios,
	.init_smc_tables = smu_v11_0_init_smc_tables,
	.fini_smc_tables = smu_v11_0_fini_smc_tables,
	.init_power = smu_v11_0_init_power,
	.fini_power = smu_v11_0_fini_power,
	.get_vbios_bootup_values = smu_v11_0_get_vbios_bootup_values,
	.get_clk_info_from_vbios = smu_v11_0_get_clk_info_from_vbios,
	.notify_memory_pool_location = smu_v11_0_notify_memory_pool_location,
	.parse_pptable = smu_v11_0_parse_pptable,
	.populate_smc_pptable = smu_v11_0_populate_smc_pptable,
	.write_pptable = smu_v11_0_write_pptable,
	.set_min_dcef_deep_sleep = smu_v11_0_set_min_dcef_deep_sleep,
	.set_tool_table_location = smu_v11_0_set_tool_table_location,
	.init_display = smu_v11_0_init_display,
	.set_allowed_mask = smu_v11_0_set_allowed_mask,
	.get_enabled_mask = smu_v11_0_get_enabled_mask,
	.enable_all_mask = smu_v11_0_enable_all_mask,
	.disable_all_mask = smu_v11_0_disable_all_mask,
	.notify_display_change = smu_v11_0_notify_display_change,
	.get_power_limit = smu_v11_0_get_power_limit,
	.get_current_clk_freq = smu_v11_0_get_current_clk_freq,
};

void smu_v11_0_set_smu_funcs(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;

	smu->funcs = &smu_v11_0_funcs;

	switch (adev->asic_type) {
	case CHIP_VEGA20:
		vega20_set_ppt_funcs(smu);
		break;
	default:
		pr_warn("Unknow asic for smu11\n");
	}
}
