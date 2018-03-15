/*
 * Copyright 2015 Advanced Micro Devices, Inc.
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
 *
 */
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <drm/drmP.h>
#include <linux/firmware.h>
#include <drm/amdgpu_drm.h>
#include "amdgpu.h"
#include "cgs_linux.h"
#include "atom.h"
#include "amdgpu_ucode.h"

#undef min_offset
#undef max_offset

struct amdgpu_cgs_device {
	struct cgs_device base;
	struct amdgpu_device *adev;
};

#define CGS_FUNC_ADEV							\
	struct amdgpu_device *adev =					\
		((struct amdgpu_cgs_device *)cgs_device)->adev


static uint32_t amdgpu_cgs_read_register(struct cgs_device *cgs_device, unsigned offset)
{
	CGS_FUNC_ADEV;
	return RREG32(offset);
}

static void amdgpu_cgs_write_register(struct cgs_device *cgs_device, unsigned offset,
				      uint32_t value)
{
	CGS_FUNC_ADEV;
	WREG32(offset, value);
}

static uint32_t amdgpu_cgs_read_ind_register(struct cgs_device *cgs_device,
					     enum cgs_ind_reg space,
					     unsigned index)
{
	CGS_FUNC_ADEV;
	switch (space) {
	case CGS_IND_REG__MMIO:
		return RREG32_IDX(index);
	case CGS_IND_REG__PCIE:
		return RREG32_PCIE(index);
	case CGS_IND_REG__SMC:
		return RREG32_SMC(index);
	case CGS_IND_REG__UVD_CTX:
		return RREG32_UVD_CTX(index);
	case CGS_IND_REG__DIDT:
		return RREG32_DIDT(index);
	case CGS_IND_REG_GC_CAC:
		return RREG32_GC_CAC(index);
	case CGS_IND_REG_SE_CAC:
		return RREG32_SE_CAC(index);
	case CGS_IND_REG__AUDIO_ENDPT:
		DRM_ERROR("audio endpt register access not implemented.\n");
		return 0;
	}
	WARN(1, "Invalid indirect register space");
	return 0;
}

static void amdgpu_cgs_write_ind_register(struct cgs_device *cgs_device,
					  enum cgs_ind_reg space,
					  unsigned index, uint32_t value)
{
	CGS_FUNC_ADEV;
	switch (space) {
	case CGS_IND_REG__MMIO:
		return WREG32_IDX(index, value);
	case CGS_IND_REG__PCIE:
		return WREG32_PCIE(index, value);
	case CGS_IND_REG__SMC:
		return WREG32_SMC(index, value);
	case CGS_IND_REG__UVD_CTX:
		return WREG32_UVD_CTX(index, value);
	case CGS_IND_REG__DIDT:
		return WREG32_DIDT(index, value);
	case CGS_IND_REG_GC_CAC:
		return WREG32_GC_CAC(index, value);
	case CGS_IND_REG_SE_CAC:
		return WREG32_SE_CAC(index, value);
	case CGS_IND_REG__AUDIO_ENDPT:
		DRM_ERROR("audio endpt register access not implemented.\n");
		return;
	}
	WARN(1, "Invalid indirect register space");
}

static int amdgpu_cgs_get_pci_resource(struct cgs_device *cgs_device,
				       enum cgs_resource_type resource_type,
				       uint64_t size,
				       uint64_t offset,
				       uint64_t *resource_base)
{
	CGS_FUNC_ADEV;

	if (resource_base == NULL)
		return -EINVAL;

	switch (resource_type) {
	case CGS_RESOURCE_TYPE_MMIO:
		if (adev->rmmio_size == 0)
			return -ENOENT;
		if ((offset + size) > adev->rmmio_size)
			return -EINVAL;
		*resource_base = adev->rmmio_base;
		return 0;
	case CGS_RESOURCE_TYPE_DOORBELL:
		if (adev->doorbell.size == 0)
			return -ENOENT;
		if ((offset + size) > adev->doorbell.size)
			return -EINVAL;
		*resource_base = adev->doorbell.base;
		return 0;
	case CGS_RESOURCE_TYPE_FB:
	case CGS_RESOURCE_TYPE_IO:
	case CGS_RESOURCE_TYPE_ROM:
	default:
		return -EINVAL;
	}
}

static const void *amdgpu_cgs_atom_get_data_table(struct cgs_device *cgs_device,
						  unsigned table, uint16_t *size,
						  uint8_t *frev, uint8_t *crev)
{
	CGS_FUNC_ADEV;
	uint16_t data_start;

	if (amdgpu_atom_parse_data_header(
		    adev->mode_info.atom_context, table, size,
		    frev, crev, &data_start))
		return (uint8_t*)adev->mode_info.atom_context->bios +
			data_start;

	return NULL;
}

static int amdgpu_cgs_atom_get_cmd_table_revs(struct cgs_device *cgs_device, unsigned table,
					      uint8_t *frev, uint8_t *crev)
{
	CGS_FUNC_ADEV;

	if (amdgpu_atom_parse_cmd_header(
		    adev->mode_info.atom_context, table,
		    frev, crev))
		return 0;

	return -EINVAL;
}

static int amdgpu_cgs_atom_exec_cmd_table(struct cgs_device *cgs_device, unsigned table,
					  void *args)
{
	CGS_FUNC_ADEV;

	return amdgpu_atom_execute_table(
		adev->mode_info.atom_context, table, args);
}

struct cgs_irq_params {
	unsigned src_id;
	cgs_irq_source_set_func_t set;
	cgs_irq_handler_func_t handler;
	void *private_data;
};

static int cgs_set_irq_state(struct amdgpu_device *adev,
			     struct amdgpu_irq_src *src,
			     unsigned type,
			     enum amdgpu_interrupt_state state)
{
	struct cgs_irq_params *irq_params =
		(struct cgs_irq_params *)src->data;
	if (!irq_params)
		return -EINVAL;
	if (!irq_params->set)
		return -EINVAL;
	return irq_params->set(irq_params->private_data,
			       irq_params->src_id,
			       type,
			       (int)state);
}

static int cgs_process_irq(struct amdgpu_device *adev,
			   struct amdgpu_irq_src *source,
			   struct amdgpu_iv_entry *entry)
{
	struct cgs_irq_params *irq_params =
		(struct cgs_irq_params *)source->data;
	if (!irq_params)
		return -EINVAL;
	if (!irq_params->handler)
		return -EINVAL;
	return irq_params->handler(irq_params->private_data,
				   irq_params->src_id,
				   entry->iv_entry);
}

static const struct amdgpu_irq_src_funcs cgs_irq_funcs = {
	.set = cgs_set_irq_state,
	.process = cgs_process_irq,
};

static int amdgpu_cgs_add_irq_source(void *cgs_device,
				     unsigned client_id,
				     unsigned src_id,
				     unsigned num_types,
				     cgs_irq_source_set_func_t set,
				     cgs_irq_handler_func_t handler,
				     void *private_data)
{
	CGS_FUNC_ADEV;
	int ret = 0;
	struct cgs_irq_params *irq_params;
	struct amdgpu_irq_src *source =
		kzalloc(sizeof(struct amdgpu_irq_src), GFP_KERNEL);
	if (!source)
		return -ENOMEM;
	irq_params =
		kzalloc(sizeof(struct cgs_irq_params), GFP_KERNEL);
	if (!irq_params) {
		kfree(source);
		return -ENOMEM;
	}
	source->num_types = num_types;
	source->funcs = &cgs_irq_funcs;
	irq_params->src_id = src_id;
	irq_params->set = set;
	irq_params->handler = handler;
	irq_params->private_data = private_data;
	source->data = (void *)irq_params;
	ret = amdgpu_irq_add_id(adev, client_id, src_id, source);
	if (ret) {
		kfree(irq_params);
		kfree(source);
	}

	return ret;
}

static int amdgpu_cgs_irq_get(void *cgs_device, unsigned client_id,
			      unsigned src_id, unsigned type)
{
	CGS_FUNC_ADEV;

	if (!adev->irq.client[client_id].sources)
		return -EINVAL;

	return amdgpu_irq_get(adev, adev->irq.client[client_id].sources[src_id], type);
}

static int amdgpu_cgs_irq_put(void *cgs_device, unsigned client_id,
			      unsigned src_id, unsigned type)
{
	CGS_FUNC_ADEV;

	if (!adev->irq.client[client_id].sources)
		return -EINVAL;

	return amdgpu_irq_put(adev, adev->irq.client[client_id].sources[src_id], type);
}

static int amdgpu_cgs_set_clockgating_state(struct cgs_device *cgs_device,
				  enum amd_ip_block_type block_type,
				  enum amd_clockgating_state state)
{
	CGS_FUNC_ADEV;
	int i, r = -1;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;

		if (adev->ip_blocks[i].version->type == block_type) {
			r = adev->ip_blocks[i].version->funcs->set_clockgating_state(
								(void *)adev,
									state);
			break;
		}
	}
	return r;
}

static int amdgpu_cgs_set_powergating_state(struct cgs_device *cgs_device,
				  enum amd_ip_block_type block_type,
				  enum amd_powergating_state state)
{
	CGS_FUNC_ADEV;
	int i, r = -1;

	for (i = 0; i < adev->num_ip_blocks; i++) {
		if (!adev->ip_blocks[i].status.valid)
			continue;

		if (adev->ip_blocks[i].version->type == block_type) {
			r = adev->ip_blocks[i].version->funcs->set_powergating_state(
								(void *)adev,
									state);
			break;
		}
	}
	return r;
}


static uint32_t fw_type_convert(struct cgs_device *cgs_device, uint32_t fw_type)
{
	CGS_FUNC_ADEV;
	enum AMDGPU_UCODE_ID result = AMDGPU_UCODE_ID_MAXIMUM;

	switch (fw_type) {
	case CGS_UCODE_ID_SDMA0:
		result = AMDGPU_UCODE_ID_SDMA0;
		break;
	case CGS_UCODE_ID_SDMA1:
		result = AMDGPU_UCODE_ID_SDMA1;
		break;
	case CGS_UCODE_ID_CP_CE:
		result = AMDGPU_UCODE_ID_CP_CE;
		break;
	case CGS_UCODE_ID_CP_PFP:
		result = AMDGPU_UCODE_ID_CP_PFP;
		break;
	case CGS_UCODE_ID_CP_ME:
		result = AMDGPU_UCODE_ID_CP_ME;
		break;
	case CGS_UCODE_ID_CP_MEC:
	case CGS_UCODE_ID_CP_MEC_JT1:
		result = AMDGPU_UCODE_ID_CP_MEC1;
		break;
	case CGS_UCODE_ID_CP_MEC_JT2:
		/* for VI. JT2 should be the same as JT1, because:
			1, MEC2 and MEC1 use exactly same FW.
			2, JT2 is not pached but JT1 is.
		*/
		if (adev->asic_type >= CHIP_TOPAZ)
			result = AMDGPU_UCODE_ID_CP_MEC1;
		else
			result = AMDGPU_UCODE_ID_CP_MEC2;
		break;
	case CGS_UCODE_ID_RLC_G:
		result = AMDGPU_UCODE_ID_RLC_G;
		break;
	case CGS_UCODE_ID_STORAGE:
		result = AMDGPU_UCODE_ID_STORAGE;
		break;
	default:
		DRM_ERROR("Firmware type not supported\n");
	}
	return result;
}

static int amdgpu_cgs_rel_firmware(struct cgs_device *cgs_device, enum cgs_ucode_id type)
{
	CGS_FUNC_ADEV;
	if ((CGS_UCODE_ID_SMU == type) || (CGS_UCODE_ID_SMU_SK == type)) {
		release_firmware(adev->pm.fw);
		adev->pm.fw = NULL;
		return 0;
	}
	/* cannot release other firmware because they are not created by cgs */
	return -EINVAL;
}

static uint16_t amdgpu_get_firmware_version(struct cgs_device *cgs_device,
					enum cgs_ucode_id type)
{
	CGS_FUNC_ADEV;
	uint16_t fw_version = 0;

	switch (type) {
		case CGS_UCODE_ID_SDMA0:
			fw_version = adev->sdma.instance[0].fw_version;
			break;
		case CGS_UCODE_ID_SDMA1:
			fw_version = adev->sdma.instance[1].fw_version;
			break;
		case CGS_UCODE_ID_CP_CE:
			fw_version = adev->gfx.ce_fw_version;
			break;
		case CGS_UCODE_ID_CP_PFP:
			fw_version = adev->gfx.pfp_fw_version;
			break;
		case CGS_UCODE_ID_CP_ME:
			fw_version = adev->gfx.me_fw_version;
			break;
		case CGS_UCODE_ID_CP_MEC:
			fw_version = adev->gfx.mec_fw_version;
			break;
		case CGS_UCODE_ID_CP_MEC_JT1:
			fw_version = adev->gfx.mec_fw_version;
			break;
		case CGS_UCODE_ID_CP_MEC_JT2:
			fw_version = adev->gfx.mec_fw_version;
			break;
		case CGS_UCODE_ID_RLC_G:
			fw_version = adev->gfx.rlc_fw_version;
			break;
		case CGS_UCODE_ID_STORAGE:
			break;
		default:
			DRM_ERROR("firmware type %d do not have version\n", type);
			break;
	}
	return fw_version;
}

static int amdgpu_cgs_enter_safe_mode(struct cgs_device *cgs_device,
					bool en)
{
	CGS_FUNC_ADEV;

	if (adev->gfx.rlc.funcs->enter_safe_mode == NULL ||
		adev->gfx.rlc.funcs->exit_safe_mode == NULL)
		return 0;

	if (en)
		adev->gfx.rlc.funcs->enter_safe_mode(adev);
	else
		adev->gfx.rlc.funcs->exit_safe_mode(adev);

	return 0;
}

static void amdgpu_cgs_lock_grbm_idx(struct cgs_device *cgs_device,
					bool lock)
{
	CGS_FUNC_ADEV;

	if (lock)
		mutex_lock(&adev->grbm_idx_mutex);
	else
		mutex_unlock(&adev->grbm_idx_mutex);
}

static int amdgpu_cgs_get_firmware_info(struct cgs_device *cgs_device,
					enum cgs_ucode_id type,
					struct cgs_firmware_info *info)
{
	CGS_FUNC_ADEV;

	if ((CGS_UCODE_ID_SMU != type) && (CGS_UCODE_ID_SMU_SK != type)) {
		uint64_t gpu_addr;
		uint32_t data_size;
		const struct gfx_firmware_header_v1_0 *header;
		enum AMDGPU_UCODE_ID id;
		struct amdgpu_firmware_info *ucode;

		id = fw_type_convert(cgs_device, type);
		ucode = &adev->firmware.ucode[id];
		if (ucode->fw == NULL)
			return -EINVAL;

		gpu_addr  = ucode->mc_addr;
		header = (const struct gfx_firmware_header_v1_0 *)ucode->fw->data;
		data_size = le32_to_cpu(header->header.ucode_size_bytes);

		if ((type == CGS_UCODE_ID_CP_MEC_JT1) ||
		    (type == CGS_UCODE_ID_CP_MEC_JT2)) {
			gpu_addr += ALIGN(le32_to_cpu(header->header.ucode_size_bytes), PAGE_SIZE);
			data_size = le32_to_cpu(header->jt_size) << 2;
		}

		info->kptr = ucode->kaddr;
		info->image_size = data_size;
		info->mc_addr = gpu_addr;
		info->version = (uint16_t)le32_to_cpu(header->header.ucode_version);

		if (CGS_UCODE_ID_CP_MEC == type)
			info->image_size = le32_to_cpu(header->jt_offset) << 2;

		info->fw_version = amdgpu_get_firmware_version(cgs_device, type);
		info->feature_version = (uint16_t)le32_to_cpu(header->ucode_feature_version);
	} else {
		char fw_name[30] = {0};
		int err = 0;
		uint32_t ucode_size;
		uint32_t ucode_start_address;
		const uint8_t *src;
		const struct smc_firmware_header_v1_0 *hdr;
		const struct common_firmware_header *header;
		struct amdgpu_firmware_info *ucode = NULL;

		if (!adev->pm.fw) {
			switch (adev->asic_type) {
			case CHIP_TAHITI:
				strcpy(fw_name, "radeon/tahiti_smc.bin");
				break;
			case CHIP_PITCAIRN:
				if ((adev->pdev->revision == 0x81) &&
				    ((adev->pdev->device == 0x6810) ||
				    (adev->pdev->device == 0x6811))) {
					info->is_kicker = true;
					strcpy(fw_name, "radeon/pitcairn_k_smc.bin");
				} else {
					strcpy(fw_name, "radeon/pitcairn_smc.bin");
				}
				break;
			case CHIP_VERDE:
				if (((adev->pdev->device == 0x6820) &&
					((adev->pdev->revision == 0x81) ||
					(adev->pdev->revision == 0x83))) ||
				    ((adev->pdev->device == 0x6821) &&
					((adev->pdev->revision == 0x83) ||
					(adev->pdev->revision == 0x87))) ||
				    ((adev->pdev->revision == 0x87) &&
					((adev->pdev->device == 0x6823) ||
					(adev->pdev->device == 0x682b)))) {
					info->is_kicker = true;
					strcpy(fw_name, "radeon/verde_k_smc.bin");
				} else {
					strcpy(fw_name, "radeon/verde_smc.bin");
				}
				break;
			case CHIP_OLAND:
				if (((adev->pdev->revision == 0x81) &&
					((adev->pdev->device == 0x6600) ||
					(adev->pdev->device == 0x6604) ||
					(adev->pdev->device == 0x6605) ||
					(adev->pdev->device == 0x6610))) ||
				    ((adev->pdev->revision == 0x83) &&
					(adev->pdev->device == 0x6610))) {
					info->is_kicker = true;
					strcpy(fw_name, "radeon/oland_k_smc.bin");
				} else {
					strcpy(fw_name, "radeon/oland_smc.bin");
				}
				break;
			case CHIP_HAINAN:
				if (((adev->pdev->revision == 0x81) &&
					(adev->pdev->device == 0x6660)) ||
				    ((adev->pdev->revision == 0x83) &&
					((adev->pdev->device == 0x6660) ||
					(adev->pdev->device == 0x6663) ||
					(adev->pdev->device == 0x6665) ||
					 (adev->pdev->device == 0x6667)))) {
					info->is_kicker = true;
					strcpy(fw_name, "radeon/hainan_k_smc.bin");
				} else if ((adev->pdev->revision == 0xc3) &&
					 (adev->pdev->device == 0x6665)) {
					info->is_kicker = true;
					strcpy(fw_name, "radeon/banks_k_2_smc.bin");
				} else {
					strcpy(fw_name, "radeon/hainan_smc.bin");
				}
				break;
			case CHIP_BONAIRE:
				if ((adev->pdev->revision == 0x80) ||
					(adev->pdev->revision == 0x81) ||
					(adev->pdev->device == 0x665f)) {
					info->is_kicker = true;
					strcpy(fw_name, "radeon/bonaire_k_smc.bin");
				} else {
					strcpy(fw_name, "radeon/bonaire_smc.bin");
				}
				break;
			case CHIP_HAWAII:
				if (adev->pdev->revision == 0x80) {
					info->is_kicker = true;
					strcpy(fw_name, "radeon/hawaii_k_smc.bin");
				} else {
					strcpy(fw_name, "radeon/hawaii_smc.bin");
				}
				break;
			case CHIP_TOPAZ:
				if (((adev->pdev->device == 0x6900) && (adev->pdev->revision == 0x81)) ||
				    ((adev->pdev->device == 0x6900) && (adev->pdev->revision == 0x83)) ||
				    ((adev->pdev->device == 0x6907) && (adev->pdev->revision == 0x87))) {
					info->is_kicker = true;
					strcpy(fw_name, "amdgpu/topaz_k_smc.bin");
				} else
					strcpy(fw_name, "amdgpu/topaz_smc.bin");
				break;
			case CHIP_TONGA:
				if (((adev->pdev->device == 0x6939) && (adev->pdev->revision == 0xf1)) ||
				    ((adev->pdev->device == 0x6938) && (adev->pdev->revision == 0xf1))) {
					info->is_kicker = true;
					strcpy(fw_name, "amdgpu/tonga_k_smc.bin");
				} else
					strcpy(fw_name, "amdgpu/tonga_smc.bin");
				break;
			case CHIP_FIJI:
				strcpy(fw_name, "amdgpu/fiji_smc.bin");
				break;
			case CHIP_POLARIS11:
				if (type == CGS_UCODE_ID_SMU) {
					if (((adev->pdev->device == 0x67ef) &&
					     ((adev->pdev->revision == 0xe0) ||
					      (adev->pdev->revision == 0xe2) ||
					      (adev->pdev->revision == 0xe5))) ||
					    ((adev->pdev->device == 0x67ff) &&
					     ((adev->pdev->revision == 0xcf) ||
					      (adev->pdev->revision == 0xef) ||
					      (adev->pdev->revision == 0xff)))) {
						info->is_kicker = true;
						strcpy(fw_name, "amdgpu/polaris11_k_smc.bin");
					} else
						strcpy(fw_name, "amdgpu/polaris11_smc.bin");
				} else if (type == CGS_UCODE_ID_SMU_SK) {
					strcpy(fw_name, "amdgpu/polaris11_smc_sk.bin");
				}
				break;
			case CHIP_POLARIS10:
				if (type == CGS_UCODE_ID_SMU) {
					if ((adev->pdev->device == 0x67df) &&
					    ((adev->pdev->revision == 0xe0) ||
					     (adev->pdev->revision == 0xe3) ||
					     (adev->pdev->revision == 0xe4) ||
					     (adev->pdev->revision == 0xe5) ||
					     (adev->pdev->revision == 0xe7) ||
					     (adev->pdev->revision == 0xef))) {
						info->is_kicker = true;
						strcpy(fw_name, "amdgpu/polaris10_k_smc.bin");
					} else
						strcpy(fw_name, "amdgpu/polaris10_smc.bin");
				} else if (type == CGS_UCODE_ID_SMU_SK) {
					strcpy(fw_name, "amdgpu/polaris10_smc_sk.bin");
				}
				break;
			case CHIP_POLARIS12:
				strcpy(fw_name, "amdgpu/polaris12_smc.bin");
				break;
			case CHIP_VEGA10:
				if ((adev->pdev->device == 0x687f) &&
					((adev->pdev->revision == 0xc0) ||
					(adev->pdev->revision == 0xc1) ||
					(adev->pdev->revision == 0xc3)))
					strcpy(fw_name, "amdgpu/vega10_acg_smc.bin");
				else
					strcpy(fw_name, "amdgpu/vega10_smc.bin");
				break;
			default:
				DRM_ERROR("SMC firmware not supported\n");
				return -EINVAL;
			}

			err = request_firmware(&adev->pm.fw, fw_name, adev->dev);
			if (err) {
				DRM_ERROR("Failed to request firmware\n");
				return err;
			}

			err = amdgpu_ucode_validate(adev->pm.fw);
			if (err) {
				DRM_ERROR("Failed to load firmware \"%s\"", fw_name);
				release_firmware(adev->pm.fw);
				adev->pm.fw = NULL;
				return err;
			}

			if (adev->firmware.load_type == AMDGPU_FW_LOAD_PSP) {
				ucode = &adev->firmware.ucode[AMDGPU_UCODE_ID_SMC];
				ucode->ucode_id = AMDGPU_UCODE_ID_SMC;
				ucode->fw = adev->pm.fw;
				header = (const struct common_firmware_header *)ucode->fw->data;
				adev->firmware.fw_size +=
					ALIGN(le32_to_cpu(header->ucode_size_bytes), PAGE_SIZE);
			}
		}

		hdr = (const struct smc_firmware_header_v1_0 *)	adev->pm.fw->data;
		amdgpu_ucode_print_smc_hdr(&hdr->header);
		adev->pm.fw_version = le32_to_cpu(hdr->header.ucode_version);
		ucode_size = le32_to_cpu(hdr->header.ucode_size_bytes);
		ucode_start_address = le32_to_cpu(hdr->ucode_start_addr);
		src = (const uint8_t *)(adev->pm.fw->data +
		       le32_to_cpu(hdr->header.ucode_array_offset_bytes));

		info->version = adev->pm.fw_version;
		info->image_size = ucode_size;
		info->ucode_start_address = ucode_start_address;
		info->kptr = (void *)src;
	}
	return 0;
}

static int amdgpu_cgs_is_virtualization_enabled(void *cgs_device)
{
	CGS_FUNC_ADEV;
	return amdgpu_sriov_vf(adev);
}

static int amdgpu_cgs_get_active_displays_info(struct cgs_device *cgs_device,
					  struct cgs_display_info *info)
{
	CGS_FUNC_ADEV;
	struct cgs_mode_info *mode_info;

	if (info == NULL)
		return -EINVAL;

	mode_info = info->mode_info;
	if (mode_info) {
		/* if the displays are off, vblank time is max */
		mode_info->vblank_time_us = 0xffffffff;
		/* always set the reference clock */
		mode_info->ref_clock = adev->clock.spll.reference_freq;
	}

	if (!amdgpu_device_has_dc_support(adev)) {
		struct amdgpu_crtc *amdgpu_crtc;
		struct drm_device *ddev = adev->ddev;
		struct drm_crtc *crtc;
		uint32_t line_time_us, vblank_lines;

		if (adev->mode_info.num_crtc && adev->mode_info.mode_config_initialized) {
			list_for_each_entry(crtc,
					&ddev->mode_config.crtc_list, head) {
				amdgpu_crtc = to_amdgpu_crtc(crtc);
				if (crtc->enabled) {
					info->active_display_mask |= (1 << amdgpu_crtc->crtc_id);
					info->display_count++;
				}
				if (mode_info != NULL &&
					crtc->enabled && amdgpu_crtc->enabled &&
					amdgpu_crtc->hw_mode.clock) {
					line_time_us = (amdgpu_crtc->hw_mode.crtc_htotal * 1000) /
								amdgpu_crtc->hw_mode.clock;
					vblank_lines = amdgpu_crtc->hw_mode.crtc_vblank_end -
								amdgpu_crtc->hw_mode.crtc_vdisplay +
								(amdgpu_crtc->v_border * 2);
					mode_info->vblank_time_us = vblank_lines * line_time_us;
					mode_info->refresh_rate = drm_mode_vrefresh(&amdgpu_crtc->hw_mode);
					/* we have issues with mclk switching with refresh rates
					 * over 120 hz on the non-DC code.
					 */
					if (mode_info->refresh_rate > 120)
						mode_info->vblank_time_us = 0;
					mode_info = NULL;
				}
			}
		}
	} else {
		info->display_count = adev->pm.pm_display_cfg.num_display;
		if (mode_info != NULL) {
			mode_info->vblank_time_us = adev->pm.pm_display_cfg.min_vblank_time;
			mode_info->refresh_rate = adev->pm.pm_display_cfg.vrefresh;
		}
	}
	return 0;
}


static int amdgpu_cgs_notify_dpm_enabled(struct cgs_device *cgs_device, bool enabled)
{
	CGS_FUNC_ADEV;

	adev->pm.dpm_enabled = enabled;

	return 0;
}

static const struct cgs_ops amdgpu_cgs_ops = {
	.read_register = amdgpu_cgs_read_register,
	.write_register = amdgpu_cgs_write_register,
	.read_ind_register = amdgpu_cgs_read_ind_register,
	.write_ind_register = amdgpu_cgs_write_ind_register,
	.get_pci_resource = amdgpu_cgs_get_pci_resource,
	.atom_get_data_table = amdgpu_cgs_atom_get_data_table,
	.atom_get_cmd_table_revs = amdgpu_cgs_atom_get_cmd_table_revs,
	.atom_exec_cmd_table = amdgpu_cgs_atom_exec_cmd_table,
	.get_firmware_info = amdgpu_cgs_get_firmware_info,
	.rel_firmware = amdgpu_cgs_rel_firmware,
	.set_powergating_state = amdgpu_cgs_set_powergating_state,
	.set_clockgating_state = amdgpu_cgs_set_clockgating_state,
	.get_active_displays_info = amdgpu_cgs_get_active_displays_info,
	.notify_dpm_enabled = amdgpu_cgs_notify_dpm_enabled,
	.is_virtualization_enabled = amdgpu_cgs_is_virtualization_enabled,
	.enter_safe_mode = amdgpu_cgs_enter_safe_mode,
	.lock_grbm_idx = amdgpu_cgs_lock_grbm_idx,
};

static const struct cgs_os_ops amdgpu_cgs_os_ops = {
	.add_irq_source = amdgpu_cgs_add_irq_source,
	.irq_get = amdgpu_cgs_irq_get,
	.irq_put = amdgpu_cgs_irq_put
};

struct cgs_device *amdgpu_cgs_create_device(struct amdgpu_device *adev)
{
	struct amdgpu_cgs_device *cgs_device =
		kmalloc(sizeof(*cgs_device), GFP_KERNEL);

	if (!cgs_device) {
		DRM_ERROR("Couldn't allocate CGS device structure\n");
		return NULL;
	}

	cgs_device->base.ops = &amdgpu_cgs_ops;
	cgs_device->base.os_ops = &amdgpu_cgs_os_ops;
	cgs_device->adev = adev;

	return (struct cgs_device *)cgs_device;
}

void amdgpu_cgs_destroy_device(struct cgs_device *cgs_device)
{
	kfree(cgs_device);
}

#define min_offset header.start
#define max_offset header.end
