/* Copyright (c) 2002,2007-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/msm_kgsl.h>

#include <mach/socinfo.h>
#include <mach/msm_bus_board.h>
#include <mach/msm_bus.h>
#include <mach/msm_dcvs.h>
#include <mach/msm_dcvs_scm.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_cffdump.h"
#include "kgsl_sharedmem.h"
#include "kgsl_iommu.h"

#include "adreno.h"
#include "adreno_pm4types.h"
#include "adreno_trace.h"

#include "a2xx_reg.h"
#include "a3xx_reg.h"

#define DRIVER_VERSION_MAJOR   3
#define DRIVER_VERSION_MINOR   1

/* Adreno MH arbiter config*/
#define ADRENO_CFG_MHARB \
	(0x10 \
		| (0 << MH_ARBITER_CONFIG__SAME_PAGE_GRANULARITY__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__L1_ARB_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__L1_ARB_HOLD_ENABLE__SHIFT) \
		| (0 << MH_ARBITER_CONFIG__L2_ARB_CONTROL__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__PAGE_SIZE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_REORDER_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_ARB_HOLD_ENABLE__SHIFT) \
		| (0 << MH_ARBITER_CONFIG__IN_FLIGHT_LIMIT_ENABLE__SHIFT) \
		| (0x8 << MH_ARBITER_CONFIG__IN_FLIGHT_LIMIT__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__CP_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__VGT_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__RB_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__PA_CLNT_ENABLE__SHIFT))

#define ADRENO_MMU_CONFIG						\
	(0x01								\
	 | (MMU_CONFIG << MH_MMU_CONFIG__RB_W_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_W_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R0_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R1_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R2_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R3_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R4_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__VGT_R0_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__VGT_R1_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__TC_R_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__PA_W_CLNT_BEHAVIOR__SHIFT))

static const struct kgsl_functable adreno_functable;

static struct adreno_device device_3d0 = {
	.dev = {
		KGSL_DEVICE_COMMON_INIT(device_3d0.dev),
		.name = DEVICE_3D0_NAME,
		.id = KGSL_DEVICE_3D0,
		.mh = {
			.mharb  = ADRENO_CFG_MHARB,
			/* Remove 1k boundary check in z470 to avoid a GPU
			 * hang.  Notice that this solution won't work if
			 * both EBI and SMI are used
			 */
			.mh_intf_cfg1 = 0x00032f07,
			/* turn off memory protection unit by setting
			   acceptable physical address range to include
			   all pages. */
			.mpu_base = 0x00000000,
			.mpu_range =  0xFFFFF000,
		},
		.mmu = {
			.config = ADRENO_MMU_CONFIG,
		},
		.pwrctrl = {
			.irq_name = KGSL_3D0_IRQ,
		},
		.iomemname = KGSL_3D0_REG_MEMORY,
		.ftbl = &adreno_functable,
#ifdef CONFIG_HAS_EARLYSUSPEND
		.display_off = {
			.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING,
			.suspend = kgsl_early_suspend_driver,
			.resume = kgsl_late_resume_driver,
		},
#endif
	},
	.gmem_base = 0,
	.gmem_size = SZ_256K,
	.pfp_fw = NULL,
	.pm4_fw = NULL,
	.wait_timeout = 0, /* in milliseconds, 0 means disabled */
	.ib_check_level = 0,
};

#define LONG_IB_DETECT_REG_INDEX_START 1
#define LONG_IB_DETECT_REG_INDEX_END 5

unsigned int ft_detect_regs[FT_DETECT_REGS_COUNT] = {
	A3XX_RBBM_STATUS,
	REG_CP_RB_RPTR,   /* LONG_IB_DETECT_REG_INDEX_START */
	REG_CP_IB1_BASE,
	REG_CP_IB1_BUFSZ,
	REG_CP_IB2_BASE,
	REG_CP_IB2_BUFSZ, /* LONG_IB_DETECT_REG_INDEX_END */
	0,
	0,
	0,
	0,
	0,
	0
};

/*
 * This is the master list of all GPU cores that are supported by this
 * driver.
 */

#define ANY_ID (~0)
#define NO_VER (~0)

static const struct {
	enum adreno_gpurev gpurev;
	unsigned int core, major, minor, patchid;
	const char *pm4fw;
	const char *pfpfw;
	struct adreno_gpudev *gpudev;
	unsigned int istore_size;
	unsigned int pix_shader_start;
	/* Size of an instruction in dwords */
	unsigned int instruction_size;
	/* size of gmem for gpu*/
	unsigned int gmem_size;
	/* version of pm4 microcode that supports sync_lock
	   between CPU and GPU for SMMU-v1 programming */
	unsigned int sync_lock_pm4_ver;
	/* version of pfp microcode that supports sync_lock
	   between CPU and GPU for SMMU-v1 programming */
	unsigned int sync_lock_pfp_ver;
} adreno_gpulist[] = {
	{ ADRENO_REV_A200, 0, 2, ANY_ID, ANY_ID,
		"yamato_pm4.fw", "yamato_pfp.fw", &adreno_a2xx_gpudev,
		512, 384, 3, SZ_256K, NO_VER, NO_VER },
	{ ADRENO_REV_A203, 0, 1, 1, ANY_ID,
		"yamato_pm4.fw", "yamato_pfp.fw", &adreno_a2xx_gpudev,
		512, 384, 3, SZ_256K, NO_VER, NO_VER },
	{ ADRENO_REV_A205, 0, 1, 0, ANY_ID,
		"yamato_pm4.fw", "yamato_pfp.fw", &adreno_a2xx_gpudev,
		512, 384, 3, SZ_256K, NO_VER, NO_VER },
	{ ADRENO_REV_A220, 2, 1, ANY_ID, ANY_ID,
		"leia_pm4_470.fw", "leia_pfp_470.fw", &adreno_a2xx_gpudev,
		512, 384, 3, SZ_512K, NO_VER, NO_VER },
	/*
	 * patchlevel 5 (8960v2) needs special pm4 firmware to work around
	 * a hardware problem.
	 */
	{ ADRENO_REV_A225, 2, 2, 0, 5,
		"a225p5_pm4.fw", "a225_pfp.fw", &adreno_a2xx_gpudev,
		1536, 768, 3, SZ_512K, NO_VER, NO_VER },
	{ ADRENO_REV_A225, 2, 2, 0, 6,
		"a225_pm4.fw", "a225_pfp.fw", &adreno_a2xx_gpudev,
		1536, 768, 3, SZ_512K, 0x225011, 0x225002 },
	{ ADRENO_REV_A225, 2, 2, ANY_ID, ANY_ID,
		"a225_pm4.fw", "a225_pfp.fw", &adreno_a2xx_gpudev,
		1536, 768, 3, SZ_512K, 0x225011, 0x225002 },
	/* A3XX doesn't use the pix_shader_start */
	{ ADRENO_REV_A305, 3, 0, 5, ANY_ID,
		"a300_pm4.fw", "a300_pfp.fw", &adreno_a3xx_gpudev,
		512, 0, 2, SZ_256K, 0x3FF037, 0x3FF016 },
	/* A3XX doesn't use the pix_shader_start */
	{ ADRENO_REV_A320, 3, 2, ANY_ID, ANY_ID,
		"a300_pm4.fw", "a300_pfp.fw", &adreno_a3xx_gpudev,
		512, 0, 2, SZ_512K, 0x3FF037, 0x3FF016 },
	{ ADRENO_REV_A330, 3, 3, 0, ANY_ID,
		"a330_pm4.fw", "a330_pfp.fw", &adreno_a3xx_gpudev,
		512, 0, 2, SZ_1M, NO_VER, NO_VER },
};

/**
 * adreno_perfcounter_init: Reserve kernel performance counters
 * @device: device to configure
 *
 * The kernel needs/wants a certain group of performance counters for
 * its own activities.  Reserve these performance counters at init time
 * to ensure that they are always reserved for the kernel.  The performance
 * counters used by the kernel can be obtained by the user, but these
 * performance counters will remain active as long as the device is alive.
 */

static void adreno_perfcounter_init(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	if (adreno_dev->gpudev->perfcounter_init)
		adreno_dev->gpudev->perfcounter_init(adreno_dev);
};

/**
 * adreno_perfcounter_start: Enable performance counters
 * @adreno_dev: Adreno device to configure
 *
 * Ensure all performance counters are enabled that are allocated.  Since
 * the device was most likely stopped, we can't trust that the counters
 * are still valid so make it so.
 */

static void adreno_perfcounter_start(struct adreno_device *adreno_dev)
{
	struct adreno_perfcounters *counters = adreno_dev->gpudev->perfcounters;
	struct adreno_perfcount_group *group;
	unsigned int i, j;

	/* group id iter */
	for (i = 0; i < counters->group_count; i++) {
		group = &(counters->groups[i]);

		/* countable iter */
		for (j = 0; j < group->reg_count; j++) {
			if (group->regs[j].countable ==
					KGSL_PERFCOUNTER_NOT_USED)
				continue;

			if (adreno_dev->gpudev->perfcounter_enable)
				adreno_dev->gpudev->perfcounter_enable(
					adreno_dev, i, j,
					group->regs[j].countable);
		}
	}
}

/**
 * adreno_perfcounter_read_group: Determine which countables are in counters
 * @adreno_dev: Adreno device to configure
 * @reads: List of kgsl_perfcounter_read_groups
 * @count: Length of list
 *
 * Read the performance counters for the groupid/countable pairs and return
 * the 64 bit result for each pair
 */

int adreno_perfcounter_read_group(struct adreno_device *adreno_dev,
	struct kgsl_perfcounter_read_group *reads, unsigned int count)
{
	struct adreno_perfcounters *counters = adreno_dev->gpudev->perfcounters;
	struct adreno_perfcount_group *group;
	struct kgsl_perfcounter_read_group *list = NULL;
	unsigned int i, j;
	int ret = 0;

	/* perfcounter get/put/query/read not allowed on a2xx */
	if (adreno_is_a2xx(adreno_dev))
		return -EINVAL;

	/* sanity check for later */
	if (!adreno_dev->gpudev->perfcounter_read)
		return -EINVAL;

	/* sanity check params passed in */
	if (reads == NULL || count == 0 || count > 100)
		return -EINVAL;

	/* verify valid inputs group ids and countables */
	for (i = 0; i < count; i++) {
		if (reads[i].groupid >= counters->group_count)
			return -EINVAL;
	}

	list = kmalloc(sizeof(struct kgsl_perfcounter_read_group) * count,
			GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	if (copy_from_user(list, reads,
			sizeof(struct kgsl_perfcounter_read_group) * count)) {
		ret = -EFAULT;
		goto done;
	}

	/* list iterator */
	for (j = 0; j < count; j++) {
		list[j].value = 0;

		group = &(counters->groups[list[j].groupid]);

		/* group/counter iterator */
		for (i = 0; i < group->reg_count; i++) {
			if (group->regs[i].countable == list[j].countable) {
				list[j].value =
					adreno_dev->gpudev->perfcounter_read(
					adreno_dev, list[j].groupid,
					i, group->regs[i].offset);
				break;
			}
		}
	}

	/* write the data */
	if (copy_to_user(reads, list,
			sizeof(struct kgsl_perfcounter_read_group) *
			count) != 0)
		ret = -EFAULT;

done:
	kfree(list);
	return ret;
}

/**
 * adreno_perfcounter_query_group: Determine which countables are in counters
 * @adreno_dev: Adreno device to configure
 * @groupid: Desired performance counter group
 * @countables: Return list of all countables in the groups counters
 * @count: Max length of the array
 * @max_counters: max counters for the groupid
 *
 * Query the current state of counters for the group.
 */

int adreno_perfcounter_query_group(struct adreno_device *adreno_dev,
	unsigned int groupid, unsigned int *countables, unsigned int count,
	unsigned int *max_counters)
{
	struct adreno_perfcounters *counters = adreno_dev->gpudev->perfcounters;
	struct adreno_perfcount_group *group;
	unsigned int i;

	*max_counters = 0;

	/* perfcounter get/put/query not allowed on a2xx */
	if (adreno_is_a2xx(adreno_dev))
		return -EINVAL;

	if (groupid >= counters->group_count)
		return -EINVAL;

	group = &(counters->groups[groupid]);
	*max_counters = group->reg_count;

	/*
	 * if NULL countable or *count of zero, return max reg_count in
	 * *max_counters and return success
	 */
	if (countables == NULL || count == 0)
		return 0;

	/*
	 * Go through all available counters.  Write upto *count * countable
	 * values.
	 */
	for (i = 0; i < group->reg_count && i < count; i++) {
		if (copy_to_user(&countables[i], &(group->regs[i].countable),
				sizeof(unsigned int)) != 0)
			return -EFAULT;
	}

	return 0;
}

/**
 * adreno_perfcounter_get: Try to put a countable in an available counter
 * @adreno_dev: Adreno device to configure
 * @groupid: Desired performance counter group
 * @countable: Countable desired to be in a counter
 * @offset: Return offset of the countable
 * @flags: Used to setup kernel perf counters
 *
 * Try to place a countable in an available counter.  If the countable is
 * already in a counter, reference count the counter/countable pair resource
 * and return success
 */

int adreno_perfcounter_get(struct adreno_device *adreno_dev,
	unsigned int groupid, unsigned int countable, unsigned int *offset,
	unsigned int flags)
{
	struct adreno_perfcounters *counters = adreno_dev->gpudev->perfcounters;
	struct adreno_perfcount_group *group;
	unsigned int i, empty = -1;

	/* always clear return variables */
	if (offset)
		*offset = 0;

	/* perfcounter get/put/query not allowed on a2xx */
	if (adreno_is_a2xx(adreno_dev))
		return -EINVAL;

	if (groupid >= counters->group_count)
		return -EINVAL;

	group = &(counters->groups[groupid]);

	/*
	 * Check if the countable is already associated with a counter.
	 * Refcount and return the offset, otherwise, try and find an empty
	 * counter and assign the countable to it.
	 */
	for (i = 0; i < group->reg_count; i++) {
		if (group->regs[i].countable == countable) {
			/* Countable already associated with counter */
			group->regs[i].refcount++;
			group->regs[i].flags |= flags;
			if (offset)
				*offset = group->regs[i].offset;
			return 0;
		} else if (group->regs[i].countable ==
			KGSL_PERFCOUNTER_NOT_USED) {
			/* keep track of unused counter */
			empty = i;
		}
	}

	/* no available counters, so do nothing else */
	if (empty == -1)
		return -EBUSY;

	/* initialize the new counter */
	group->regs[empty].countable = countable;
	group->regs[empty].refcount = 1;

	/* enable the new counter */
	adreno_dev->gpudev->perfcounter_enable(adreno_dev, groupid, empty,
		countable);

	group->regs[empty].flags = flags;

	if (offset)
		*offset = group->regs[empty].offset;

	return 0;
}


/**
 * adreno_perfcounter_put: Release a countable from counter resource
 * @adreno_dev: Adreno device to configure
 * @groupid: Desired performance counter group
 * @countable: Countable desired to be freed from a  counter
 *
 * Put a performance counter/countable pair that was previously received.  If
 * noone else is using the countable, free up the counter for others.
 */
int adreno_perfcounter_put(struct adreno_device *adreno_dev,
	unsigned int groupid, unsigned int countable)
{
	struct adreno_perfcounters *counters = adreno_dev->gpudev->perfcounters;
	struct adreno_perfcount_group *group;

	unsigned int i;

	/* perfcounter get/put/query not allowed on a2xx */
	if (adreno_is_a2xx(adreno_dev))
		return -EINVAL;

	if (groupid >= counters->group_count)
		return -EINVAL;

	group = &(counters->groups[groupid]);

	for (i = 0; i < group->reg_count; i++) {
		if (group->regs[i].countable == countable) {
			if (group->regs[i].refcount > 0) {
				group->regs[i].refcount--;

				/*
				 * book keeping to ensure we never free a
				 * perf counter used by kernel
				 */
				if (group->regs[i].flags &&
					group->regs[i].refcount == 0)
					group->regs[i].refcount++;

				/* make available if not used */
				if (group->regs[i].refcount == 0)
					group->regs[i].countable =
						KGSL_PERFCOUNTER_NOT_USED;
			}

			return 0;
		}
	}

	return -EINVAL;
}

static irqreturn_t adreno_irq_handler(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	return adreno_dev->gpudev->irq_handler(adreno_dev);
}

static void adreno_cleanup_pt(struct kgsl_device *device,
			struct kgsl_pagetable *pagetable)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	kgsl_mmu_unmap(pagetable, &rb->buffer_desc);

	kgsl_mmu_unmap(pagetable, &rb->memptrs_desc);

	kgsl_mmu_unmap(pagetable, &device->memstore);

	kgsl_mmu_unmap(pagetable, &device->mmu.setstate_memory);
}

static int adreno_setup_pt(struct kgsl_device *device,
			struct kgsl_pagetable *pagetable)
{
	int result = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	result = kgsl_mmu_map_global(pagetable, &rb->buffer_desc);
	if (result)
		goto error;

	result = kgsl_mmu_map_global(pagetable, &rb->memptrs_desc);
	if (result)
		goto unmap_buffer_desc;

	result = kgsl_mmu_map_global(pagetable, &device->memstore);
	if (result)
		goto unmap_memptrs_desc;

	result = kgsl_mmu_map_global(pagetable, &device->mmu.setstate_memory);
	if (result)
		goto unmap_memstore_desc;

	/*
	 * Set the mpu end to the last "normal" global memory we use.
	 * For the IOMMU, this will be used to restrict access to the
	 * mapped registers.
	 */
	device->mh.mpu_range = device->mmu.setstate_memory.gpuaddr +
				device->mmu.setstate_memory.size;
	return result;

unmap_memstore_desc:
	kgsl_mmu_unmap(pagetable, &device->memstore);

unmap_memptrs_desc:
	kgsl_mmu_unmap(pagetable, &rb->memptrs_desc);

unmap_buffer_desc:
	kgsl_mmu_unmap(pagetable, &rb->buffer_desc);

error:
	return result;
}

static int adreno_iommu_setstate(struct kgsl_device *device,
					unsigned int context_id,
					uint32_t flags)
{
	unsigned int pt_val, reg_pt_val;
	unsigned int link[250];
	unsigned int *cmds = &link[0];
	int sizedwords = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int num_iommu_units, i;
	struct kgsl_context *context;
	struct adreno_context *adreno_ctx = NULL;
	int result = 0;

	/*
	 * If we're idle and we don't need to use the GPU to save context
	 * state, use the CPU instead of the GPU to reprogram the
	 * iommu for simplicity's sake.
	 */
	 if (!adreno_dev->drawctxt_active || device->ftbl->isidle(device) ||
		 !atomic_read(&device->active_cnt))
		return kgsl_mmu_device_setstate(&device->mmu, flags);

	num_iommu_units = kgsl_mmu_get_num_iommu_units(&device->mmu);

	context = kgsl_context_get(device, context_id);

	if (context == NULL)
		return 0;

	adreno_ctx = ADRENO_CONTEXT(context);

	result = kgsl_mmu_enable_clk(&device->mmu, KGSL_IOMMU_CONTEXT_USER);

	if (result)
		goto done;

	cmds += __adreno_add_idle_indirect_cmds(cmds,
		device->mmu.setstate_memory.gpuaddr +
		KGSL_IOMMU_SETSTATE_NOP_OFFSET);

	if (cpu_is_msm8960())
		cmds += adreno_add_change_mh_phys_limit_cmds(cmds, 0xFFFFF000,
					device->mmu.setstate_memory.gpuaddr +
					KGSL_IOMMU_SETSTATE_NOP_OFFSET);
	else
		cmds += adreno_add_bank_change_cmds(cmds,
					KGSL_IOMMU_CONTEXT_USER,
					device->mmu.setstate_memory.gpuaddr +
					KGSL_IOMMU_SETSTATE_NOP_OFFSET);

	cmds += adreno_add_idle_cmds(adreno_dev, cmds);

	/* Acquire GPU-CPU sync Lock here */
	cmds += kgsl_mmu_sync_lock(&device->mmu, cmds);

	pt_val = kgsl_mmu_get_pt_base_addr(&device->mmu,
					device->mmu.hwpagetable);
	if (flags & KGSL_MMUFLAGS_PTUPDATE) {
		/*
		 * We need to perfrom the following operations for all
		 * IOMMU units
		 */
		for (i = 0; i < num_iommu_units; i++) {
			reg_pt_val = (pt_val + kgsl_mmu_get_pt_lsb(&device->mmu,
						i, KGSL_IOMMU_CONTEXT_USER));
			/*
			 * Set address of the new pagetable by writng to IOMMU
			 * TTBR0 register
			 */
			*cmds++ = cp_type3_packet(CP_MEM_WRITE, 2);
			*cmds++ = kgsl_mmu_get_reg_gpuaddr(&device->mmu, i,
				KGSL_IOMMU_CONTEXT_USER, KGSL_IOMMU_CTX_TTBR0);
			*cmds++ = reg_pt_val;
			*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
			*cmds++ = 0x00000000;

			/*
			 * Read back the ttbr0 register as a barrier to ensure
			 * above writes have completed
			 */
			cmds += adreno_add_read_cmds(device, cmds,
				kgsl_mmu_get_reg_gpuaddr(&device->mmu, i,
				KGSL_IOMMU_CONTEXT_USER, KGSL_IOMMU_CTX_TTBR0),
				reg_pt_val,
				device->mmu.setstate_memory.gpuaddr +
				KGSL_IOMMU_SETSTATE_NOP_OFFSET);
		}
	}
	if (flags & KGSL_MMUFLAGS_TLBFLUSH) {
		/*
		 * tlb flush
		 */
		for (i = 0; i < num_iommu_units; i++) {
			reg_pt_val = (pt_val + kgsl_mmu_get_pt_lsb(&device->mmu,
						i, KGSL_IOMMU_CONTEXT_USER));

			*cmds++ = cp_type3_packet(CP_MEM_WRITE, 2);
			*cmds++ = kgsl_mmu_get_reg_gpuaddr(&device->mmu, i,
				KGSL_IOMMU_CONTEXT_USER,
				KGSL_IOMMU_CTX_TLBIALL);
			*cmds++ = 1;

			cmds += __adreno_add_idle_indirect_cmds(cmds,
			device->mmu.setstate_memory.gpuaddr +
			KGSL_IOMMU_SETSTATE_NOP_OFFSET);

			cmds += adreno_add_read_cmds(device, cmds,
				kgsl_mmu_get_reg_gpuaddr(&device->mmu, i,
					KGSL_IOMMU_CONTEXT_USER,
					KGSL_IOMMU_CTX_TTBR0),
				reg_pt_val,
				device->mmu.setstate_memory.gpuaddr +
				KGSL_IOMMU_SETSTATE_NOP_OFFSET);
		}
	}

	/* Release GPU-CPU sync Lock here */
	cmds += kgsl_mmu_sync_unlock(&device->mmu, cmds);

	if (cpu_is_msm8960())
		cmds += adreno_add_change_mh_phys_limit_cmds(cmds,
			kgsl_mmu_get_reg_gpuaddr(&device->mmu, 0,
						0, KGSL_IOMMU_GLOBAL_BASE),
			device->mmu.setstate_memory.gpuaddr +
			KGSL_IOMMU_SETSTATE_NOP_OFFSET);
	else
		cmds += adreno_add_bank_change_cmds(cmds,
			KGSL_IOMMU_CONTEXT_PRIV,
			device->mmu.setstate_memory.gpuaddr +
			KGSL_IOMMU_SETSTATE_NOP_OFFSET);

	cmds += adreno_add_idle_cmds(adreno_dev, cmds);

	sizedwords += (cmds - &link[0]);
	if (sizedwords) {

		/* invalidate all base pointers */
		*cmds++ = cp_type3_packet(CP_INVALIDATE_STATE, 1);
		*cmds++ = 0x7fff;
		sizedwords += 2;
		/* This returns the per context timestamp but we need to
		 * use the global timestamp for iommu clock disablement */
		result = adreno_ringbuffer_issuecmds(device, adreno_ctx,
			KGSL_CMD_FLAGS_PMODE,
			&link[0], sizedwords);

		if (result) {
			/* On error disable the IOMMU clock right away */
			kgsl_mmu_disable_clk(&device->mmu);
			goto done;
		}

		kgsl_mmu_disable_clk_on_ts(&device->mmu,
				adreno_dev->ringbuffer.global_ts, true);

		if (result)
			goto done;
	}

	if (sizedwords > (sizeof(link)/sizeof(unsigned int))) {
		KGSL_DRV_ERR(device, "Temp command buffer overflow\n");
		BUG();
	}
done:
	kgsl_context_put(context);
	return result;
}

static int adreno_gpummu_setstate(struct kgsl_device *device,
					unsigned int context_id,
					uint32_t flags)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int link[32];
	unsigned int *cmds = &link[0];
	int sizedwords = 0;
	unsigned int mh_mmu_invalidate = 0x00000003; /*invalidate all and tc */
	struct kgsl_context *context;
	struct adreno_context *adreno_ctx = NULL;
	int ret = 0;

	/*
	 * Fix target freeze issue by adding TLB flush for each submit
	 * on A20X based targets.
	 */
	if (adreno_is_a20x(adreno_dev))
		flags |= KGSL_MMUFLAGS_TLBFLUSH;
	/*
	 * If possible, then set the state via the command stream to avoid
	 * a CPU idle.  Otherwise, use the default setstate which uses register
	 * writes For CFF dump we must idle and use the registers so that it is
	 * easier to filter out the mmu accesses from the dump
	 */
	if (!kgsl_cff_dump_enable && adreno_dev->drawctxt_active) {
		context = kgsl_context_get(device, context_id);
		if (context == NULL)
			return -EINVAL;

		adreno_ctx = ADRENO_CONTEXT(context);

		if (flags & KGSL_MMUFLAGS_PTUPDATE) {
			/* wait for graphics pipe to be idle */
			*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
			*cmds++ = 0x00000000;

			/* set page table base */
			*cmds++ = cp_type0_packet(MH_MMU_PT_BASE, 1);
			*cmds++ = kgsl_mmu_get_pt_base_addr(&device->mmu,
					device->mmu.hwpagetable);
			sizedwords += 4;
		}

		if (flags & KGSL_MMUFLAGS_TLBFLUSH) {
			if (!(flags & KGSL_MMUFLAGS_PTUPDATE)) {
				*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE,
								1);
				*cmds++ = 0x00000000;
				sizedwords += 2;
			}
			*cmds++ = cp_type0_packet(MH_MMU_INVALIDATE, 1);
			*cmds++ = mh_mmu_invalidate;
			sizedwords += 2;
		}

		if (flags & KGSL_MMUFLAGS_PTUPDATE &&
			adreno_is_a20x(adreno_dev)) {
			/* HW workaround: to resolve MMU page fault interrupts
			* caused by the VGT.It prevents the CP PFP from filling
			* the VGT DMA request fifo too early,thereby ensuring
			* that the VGT will not fetch vertex/bin data until
			* after the page table base register has been updated.
			*
			* Two null DRAW_INDX_BIN packets are inserted right
			* after the page table base update, followed by a
			* wait for idle. The null packets will fill up the
			* VGT DMA request fifo and prevent any further
			* vertex/bin updates from occurring until the wait
			* has finished. */
			*cmds++ = cp_type3_packet(CP_SET_CONSTANT, 2);
			*cmds++ = (0x4 << 16) |
				(REG_PA_SU_SC_MODE_CNTL - 0x2000);
			*cmds++ = 0;	  /* disable faceness generation */
			*cmds++ = cp_type3_packet(CP_SET_BIN_BASE_OFFSET, 1);
			*cmds++ = device->mmu.setstate_memory.gpuaddr;
			*cmds++ = cp_type3_packet(CP_DRAW_INDX_BIN, 6);
			*cmds++ = 0;	  /* viz query info */
			*cmds++ = 0x0003C004; /* draw indicator */
			*cmds++ = 0;	  /* bin base */
			*cmds++ = 3;	  /* bin size */
			*cmds++ =
			device->mmu.setstate_memory.gpuaddr; /* dma base */
			*cmds++ = 6;	  /* dma size */
			*cmds++ = cp_type3_packet(CP_DRAW_INDX_BIN, 6);
			*cmds++ = 0;	  /* viz query info */
			*cmds++ = 0x0003C004; /* draw indicator */
			*cmds++ = 0;	  /* bin base */
			*cmds++ = 3;	  /* bin size */
			/* dma base */
			*cmds++ = device->mmu.setstate_memory.gpuaddr;
			*cmds++ = 6;	  /* dma size */
			*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
			*cmds++ = 0x00000000;
			sizedwords += 21;
		}


		if (flags & (KGSL_MMUFLAGS_PTUPDATE | KGSL_MMUFLAGS_TLBFLUSH)) {
			*cmds++ = cp_type3_packet(CP_INVALIDATE_STATE, 1);
			*cmds++ = 0x7fff; /* invalidate all base pointers */
			sizedwords += 2;
		}

		ret = adreno_ringbuffer_issuecmds(device, adreno_ctx,
					KGSL_CMD_FLAGS_PMODE,
					&link[0], sizedwords);

		kgsl_context_put(context);
	} else {
		kgsl_mmu_device_setstate(&device->mmu, flags);
	}

	return ret;
}

static int adreno_setstate(struct kgsl_device *device,
			unsigned int context_id,
			uint32_t flags)
{
	/* call the mmu specific handler */
	if (KGSL_MMU_TYPE_GPU == kgsl_mmu_get_mmutype())
		return adreno_gpummu_setstate(device, context_id, flags);
	else if (KGSL_MMU_TYPE_IOMMU == kgsl_mmu_get_mmutype())
		return adreno_iommu_setstate(device, context_id, flags);

	return 0;
}

static unsigned int
a3xx_getchipid(struct kgsl_device *device)
{
	struct kgsl_device_platform_data *pdata =
		kgsl_device_get_drvdata(device);

	/*
	 * All current A3XX chipids are detected at the SOC level. Leave this
	 * function here to support any future GPUs that have working
	 * chip ID registers
	 */

	return pdata->chipid;
}

static unsigned int
a2xx_getchipid(struct kgsl_device *device)
{
	unsigned int chipid = 0;
	unsigned int coreid, majorid, minorid, patchid, revid;
	struct kgsl_device_platform_data *pdata =
		kgsl_device_get_drvdata(device);

	/* If the chip id is set at the platform level, then just use that */

	if (pdata->chipid != 0)
		return pdata->chipid;

	adreno_regread(device, REG_RBBM_PERIPHID1, &coreid);
	adreno_regread(device, REG_RBBM_PERIPHID2, &majorid);
	adreno_regread(device, REG_RBBM_PATCH_RELEASE, &revid);

	/*
	* adreno 22x gpus are indicated by coreid 2,
	* but REG_RBBM_PERIPHID1 always contains 0 for this field
	*/
	if (cpu_is_msm8x60())
		chipid = 2 << 24;
	else
		chipid = (coreid & 0xF) << 24;

	chipid |= ((majorid >> 4) & 0xF) << 16;

	minorid = ((revid >> 0)  & 0xFF);

	patchid = ((revid >> 16) & 0xFF);

	/* 8x50 returns 0 for patch release, but it should be 1 */
	/* 8x25 returns 0 for minor id, but it should be 1 */
	if (cpu_is_qsd8x50())
		patchid = 1;
	else if (cpu_is_msm8625() && minorid == 0)
		minorid = 1;

	chipid |= (minorid << 8) | patchid;

	return chipid;
}

static unsigned int
adreno_getchipid(struct kgsl_device *device)
{
	struct kgsl_device_platform_data *pdata =
		kgsl_device_get_drvdata(device);

	/*
	 * All A3XX chipsets will have pdata set, so assume !pdata->chipid is
	 * an A2XX processor
	 */

	if (pdata->chipid == 0 || ADRENO_CHIPID_MAJOR(pdata->chipid) == 2)
		return a2xx_getchipid(device);
	else
		return a3xx_getchipid(device);
}

static inline bool _rev_match(unsigned int id, unsigned int entry)
{
	return (entry == ANY_ID || entry == id);
}

static void
adreno_identify_gpu(struct adreno_device *adreno_dev)
{
	unsigned int i, core, major, minor, patchid;

	adreno_dev->chip_id = adreno_getchipid(&adreno_dev->dev);

	core = ADRENO_CHIPID_CORE(adreno_dev->chip_id);
	major = ADRENO_CHIPID_MAJOR(adreno_dev->chip_id);
	minor = ADRENO_CHIPID_MINOR(adreno_dev->chip_id);
	patchid = ADRENO_CHIPID_PATCH(adreno_dev->chip_id);

	for (i = 0; i < ARRAY_SIZE(adreno_gpulist); i++) {
		if (core == adreno_gpulist[i].core &&
		    _rev_match(major, adreno_gpulist[i].major) &&
		    _rev_match(minor, adreno_gpulist[i].minor) &&
		    _rev_match(patchid, adreno_gpulist[i].patchid))
			break;
	}

	if (i == ARRAY_SIZE(adreno_gpulist)) {
		adreno_dev->gpurev = ADRENO_REV_UNKNOWN;
		return;
	}

	adreno_dev->gpurev = adreno_gpulist[i].gpurev;
	adreno_dev->gpudev = adreno_gpulist[i].gpudev;
	adreno_dev->pfp_fwfile = adreno_gpulist[i].pfpfw;
	adreno_dev->pm4_fwfile = adreno_gpulist[i].pm4fw;
	adreno_dev->istore_size = adreno_gpulist[i].istore_size;
	adreno_dev->pix_shader_start = adreno_gpulist[i].pix_shader_start;
	adreno_dev->instruction_size = adreno_gpulist[i].instruction_size;
	adreno_dev->gmem_size = adreno_gpulist[i].gmem_size;
	adreno_dev->gpulist_index = i;

}

static struct platform_device_id adreno_id_table[] = {
	{ DEVICE_3D0_NAME, (kernel_ulong_t)&device_3d0.dev, },
	{},
};

MODULE_DEVICE_TABLE(platform, adreno_id_table);

static struct of_device_id adreno_match_table[] = {
	{ .compatible = "qcom,kgsl-3d0", },
	{}
};

static inline int adreno_of_read_property(struct device_node *node,
	const char *prop, unsigned int *ptr)
{
	int ret = of_property_read_u32(node, prop, ptr);
	if (ret)
		KGSL_CORE_ERR("Unable to read '%s'\n", prop);
	return ret;
}

static struct device_node *adreno_of_find_subnode(struct device_node *parent,
	const char *name)
{
	struct device_node *child;

	for_each_child_of_node(parent, child) {
		if (of_device_is_compatible(child, name))
			return child;
	}

	return NULL;
}

static int adreno_of_get_pwrlevels(struct device_node *parent,
	struct kgsl_device_platform_data *pdata)
{
	struct device_node *node, *child;
	int ret = -EINVAL;

	node = adreno_of_find_subnode(parent, "qcom,gpu-pwrlevels");

	if (node == NULL) {
		KGSL_CORE_ERR("Unable to find 'qcom,gpu-pwrlevels'\n");
		return -EINVAL;
	}

	pdata->num_levels = 0;

	for_each_child_of_node(node, child) {
		unsigned int index;
		struct kgsl_pwrlevel *level;

		if (adreno_of_read_property(child, "reg", &index))
			goto done;

		if (index >= KGSL_MAX_PWRLEVELS) {
			KGSL_CORE_ERR("Pwrlevel index %d is out of range\n",
				index);
			continue;
		}

		if (index >= pdata->num_levels)
			pdata->num_levels = index + 1;

		level = &pdata->pwrlevel[index];

		if (adreno_of_read_property(child, "qcom,gpu-freq",
			&level->gpu_freq))
			goto done;

		if (adreno_of_read_property(child, "qcom,bus-freq",
			&level->bus_freq))
			goto done;

		if (adreno_of_read_property(child, "qcom,io-fraction",
			&level->io_fraction))
			level->io_fraction = 0;
	}

	if (adreno_of_read_property(parent, "qcom,initial-pwrlevel",
		&pdata->init_level))
		pdata->init_level = 1;

	if (pdata->init_level < 0 || pdata->init_level > pdata->num_levels) {
		KGSL_CORE_ERR("Initial power level out of range\n");
		pdata->init_level = 1;
	}

	ret = 0;
done:
	return ret;

}

static struct msm_dcvs_core_info *adreno_of_get_dcvs(struct device_node *parent)
{
	struct device_node *node, *child;
	struct msm_dcvs_core_info *info = NULL;
	int count = 0;
	int ret = -EINVAL;

	node = adreno_of_find_subnode(parent, "qcom,dcvs-core-info");
	if (node == NULL)
		return ERR_PTR(-EINVAL);

	info = kzalloc(sizeof(*info), GFP_KERNEL);

	if (info == NULL) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n", sizeof(*info));
		ret = -ENOMEM;
		goto err;
	}

	for_each_child_of_node(node, child)
		count++;

	info->power_param.num_freq = count;

	info->freq_tbl = kzalloc(info->power_param.num_freq *
			sizeof(struct msm_dcvs_freq_entry),
			GFP_KERNEL);

	if (info->freq_tbl == NULL) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n",
			info->power_param.num_freq *
			sizeof(struct msm_dcvs_freq_entry));
		ret = -ENOMEM;
		goto err;
	}

	for_each_child_of_node(node, child) {
		unsigned int index;

		if (adreno_of_read_property(child, "reg", &index))
			goto err;

		if (index >= info->power_param.num_freq) {
			KGSL_CORE_ERR("DCVS freq entry %d is out of range\n",
				index);
			continue;
		}

		if (adreno_of_read_property(child, "qcom,freq",
			&info->freq_tbl[index].freq))
			goto err;

		if (adreno_of_read_property(child, "qcom,voltage",
			&info->freq_tbl[index].voltage))
			info->freq_tbl[index].voltage = 0;

		if (adreno_of_read_property(child, "qcom,is_trans_level",
			&info->freq_tbl[index].is_trans_level))
			info->freq_tbl[index].is_trans_level = 0;

		if (adreno_of_read_property(child, "qcom,active-energy-offset",
			&info->freq_tbl[index].active_energy_offset))
			info->freq_tbl[index].active_energy_offset = 0;

		if (adreno_of_read_property(child, "qcom,leakage-energy-offset",
			&info->freq_tbl[index].leakage_energy_offset))
			info->freq_tbl[index].leakage_energy_offset = 0;
	}

	if (adreno_of_read_property(node, "qcom,num-cores", &info->num_cores))
		goto err;

	info->sensors = kzalloc(info->num_cores *
			sizeof(int),
			GFP_KERNEL);

	for (count = 0; count < info->num_cores; count++) {
		if (adreno_of_read_property(node, "qcom,sensors",
			&(info->sensors[count])))
			goto err;
	}

	if (adreno_of_read_property(node, "qcom,core-core-type",
		&info->core_param.core_type))
		goto err;

	if (adreno_of_read_property(node, "qcom,algo-disable-pc-threshold",
		&info->algo_param.disable_pc_threshold))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-em-win-size-min-us",
		&info->algo_param.em_win_size_min_us))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-em-win-size-max-us",
		&info->algo_param.em_win_size_max_us))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-em-max-util-pct",
		&info->algo_param.em_max_util_pct))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-group-id",
		&info->algo_param.group_id))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-max-freq-chg-time-us",
		&info->algo_param.max_freq_chg_time_us))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-slack-mode-dynamic",
		&info->algo_param.slack_mode_dynamic))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-slack-weight-thresh-pct",
		&info->algo_param.slack_weight_thresh_pct))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-slack-time-min-us",
		&info->algo_param.slack_time_min_us))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-slack-time-max-us",
		&info->algo_param.slack_time_max_us))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-ss-win-size-min-us",
		&info->algo_param.ss_win_size_min_us))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-ss-win-size-max-us",
		&info->algo_param.ss_win_size_max_us))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-ss-util-pct",
		&info->algo_param.ss_util_pct))
		goto err;
	if (adreno_of_read_property(node, "qcom,algo-ss-no-corr-below-freq",
		&info->algo_param.ss_no_corr_below_freq))
		goto err;

	if (adreno_of_read_property(node, "qcom,energy-active-coeff-a",
		&info->energy_coeffs.active_coeff_a))
		goto err;
	if (adreno_of_read_property(node, "qcom,energy-active-coeff-b",
		&info->energy_coeffs.active_coeff_b))
		goto err;
	if (adreno_of_read_property(node, "qcom,energy-active-coeff-c",
		&info->energy_coeffs.active_coeff_c))
		goto err;
	if (adreno_of_read_property(node, "qcom,energy-leakage-coeff-a",
		&info->energy_coeffs.leakage_coeff_a))
		goto err;
	if (adreno_of_read_property(node, "qcom,energy-leakage-coeff-b",
		&info->energy_coeffs.leakage_coeff_b))
		goto err;
	if (adreno_of_read_property(node, "qcom,energy-leakage-coeff-c",
		&info->energy_coeffs.leakage_coeff_c))
		goto err;
	if (adreno_of_read_property(node, "qcom,energy-leakage-coeff-d",
		&info->energy_coeffs.leakage_coeff_d))
		goto err;

	if (adreno_of_read_property(node, "qcom,power-current-temp",
		&info->power_param.current_temp))
		goto err;

	return info;

err:
	if (info)
		kfree(info->freq_tbl);

	kfree(info);

	return ERR_PTR(ret);
}

static int adreno_of_get_iommu(struct device_node *parent,
	struct kgsl_device_platform_data *pdata)
{
	struct device_node *node, *child;
	struct kgsl_device_iommu_data *data = NULL;
	struct kgsl_iommu_ctx *ctxs = NULL;
	u32 reg_val[2];
	int ctx_index = 0;

	node = of_parse_phandle(parent, "iommu", 0);
	if (node == NULL)
		return -EINVAL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n", sizeof(*data));
		goto err;
	}

	if (of_property_read_u32_array(node, "reg", reg_val, 2))
		goto err;

	data->physstart = reg_val[0];
	data->physend = data->physstart + reg_val[1] - 1;

	data->iommu_ctx_count = 0;

	for_each_child_of_node(node, child)
		data->iommu_ctx_count++;

	ctxs = kzalloc(data->iommu_ctx_count * sizeof(struct kgsl_iommu_ctx),
		GFP_KERNEL);

	if (ctxs == NULL) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n",
			data->iommu_ctx_count * sizeof(struct kgsl_iommu_ctx));
		goto err;
	}

	for_each_child_of_node(node, child) {
		int ret = of_property_read_string(child, "label",
				&ctxs[ctx_index].iommu_ctx_name);

		if (ret) {
			KGSL_CORE_ERR("Unable to read KGSL IOMMU 'label'\n");
			goto err;
		}

		if (adreno_of_read_property(child, "qcom,iommu-ctx-sids",
			&ctxs[ctx_index].ctx_id))
			goto err;

		ctx_index++;
	}

	data->iommu_ctxs = ctxs;

	pdata->iommu_data = data;
	pdata->iommu_count = 1;

	return 0;

err:
	kfree(ctxs);
	kfree(data);

	return -EINVAL;
}

static int adreno_of_get_pdata(struct platform_device *pdev)
{
	struct kgsl_device_platform_data *pdata = NULL;
	struct kgsl_device *device;
	int ret = -EINVAL;

	pdev->id_entry = adreno_id_table;

	pdata = pdev->dev.platform_data;
	if (pdata)
		return 0;

	if (of_property_read_string(pdev->dev.of_node, "label", &pdev->name)) {
		KGSL_CORE_ERR("Unable to read 'label'\n");
		goto err;
	}

	if (adreno_of_read_property(pdev->dev.of_node, "qcom,id", &pdev->id))
		goto err;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (pdata == NULL) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n", sizeof(*pdata));
		ret = -ENOMEM;
		goto err;
	}

	if (adreno_of_read_property(pdev->dev.of_node, "qcom,chipid",
		&pdata->chipid))
		goto err;

	/* pwrlevel Data */
	ret = adreno_of_get_pwrlevels(pdev->dev.of_node, pdata);
	if (ret)
		goto err;

	/* Default value is 83, if not found in DT */
	if (adreno_of_read_property(pdev->dev.of_node, "qcom,idle-timeout",
		&pdata->idle_timeout))
		pdata->idle_timeout = 83;

	if (adreno_of_read_property(pdev->dev.of_node, "qcom,nap-allowed",
		&pdata->nap_allowed))
		pdata->nap_allowed = 1;

	if (adreno_of_read_property(pdev->dev.of_node, "qcom,clk-map",
		&pdata->clk_map))
		goto err;

	device = (struct kgsl_device *)pdev->id_entry->driver_data;

	if (device->id != KGSL_DEVICE_3D0)
		goto err;

	/* Bus Scale Data */

	pdata->bus_scale_table = msm_bus_cl_get_pdata(pdev);
	if (IS_ERR_OR_NULL(pdata->bus_scale_table)) {
		ret = PTR_ERR(pdata->bus_scale_table);
		goto err;
	}

	pdata->core_info = adreno_of_get_dcvs(pdev->dev.of_node);
	if (IS_ERR_OR_NULL(pdata->core_info)) {
		ret = PTR_ERR(pdata->core_info);
		goto err;
	}

	ret = adreno_of_get_iommu(pdev->dev.of_node, pdata);
	if (ret)
		goto err;

	pdev->dev.platform_data = pdata;
	return 0;

err:
	if (pdata) {
		if (pdata->core_info)
			kfree(pdata->core_info->freq_tbl);
		kfree(pdata->core_info);

		if (pdata->iommu_data)
			kfree(pdata->iommu_data->iommu_ctxs);

		kfree(pdata->iommu_data);
	}

	kfree(pdata);

	return ret;
}

#ifdef CONFIG_MSM_OCMEM
static int
adreno_ocmem_gmem_malloc(struct adreno_device *adreno_dev)
{
	if (!adreno_is_a330(adreno_dev))
		return 0;

	/* OCMEM is only needed once, do not support consective allocation */
	if (adreno_dev->ocmem_hdl != NULL)
		return 0;

	adreno_dev->ocmem_hdl =
		ocmem_allocate(OCMEM_GRAPHICS, adreno_dev->gmem_size);
	if (adreno_dev->ocmem_hdl == NULL)
		return -ENOMEM;

	adreno_dev->gmem_size = adreno_dev->ocmem_hdl->len;
	adreno_dev->ocmem_base = adreno_dev->ocmem_hdl->addr;

	return 0;
}

static void
adreno_ocmem_gmem_free(struct adreno_device *adreno_dev)
{
	if (!adreno_is_a330(adreno_dev))
		return;

	if (adreno_dev->ocmem_hdl == NULL)
		return;

	ocmem_free(OCMEM_GRAPHICS, adreno_dev->ocmem_hdl);
	adreno_dev->ocmem_hdl = NULL;
}
#else
static int
adreno_ocmem_gmem_malloc(struct adreno_device *adreno_dev)
{
	return 0;
}

static void
adreno_ocmem_gmem_free(struct adreno_device *adreno_dev)
{
}
#endif

static int __devinit
adreno_probe(struct platform_device *pdev)
{
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;
	int status = -EINVAL;
	bool is_dt;

	is_dt = of_match_device(adreno_match_table, &pdev->dev);

	if (is_dt && pdev->dev.of_node) {
		status = adreno_of_get_pdata(pdev);
		if (status)
			goto error_return;
	}

	device = (struct kgsl_device *)pdev->id_entry->driver_data;
	adreno_dev = ADRENO_DEVICE(device);
	device->parentdev = &pdev->dev;

	status = adreno_ringbuffer_init(device);
	if (status != 0)
		goto error;

	status = kgsl_device_platform_probe(device);
	if (status)
		goto error_close_rb;

	status = adreno_dispatcher_init(adreno_dev);
	if (status)
		goto error_close_device;

	adreno_debugfs_init(device);

	kgsl_pwrscale_init(device);
	kgsl_pwrscale_attach_policy(device, ADRENO_DEFAULT_PWRSCALE_POLICY);

	device->flags &= ~KGSL_FLAGS_SOFT_RESET;
	return 0;

error_close_device:
	kgsl_device_platform_remove(device);
error_close_rb:
	adreno_ringbuffer_close(&adreno_dev->ringbuffer);
error:
	device->parentdev = NULL;
error_return:
	return status;
}

static int __devexit adreno_remove(struct platform_device *pdev)
{
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;

	device = (struct kgsl_device *)pdev->id_entry->driver_data;
	adreno_dev = ADRENO_DEVICE(device);

	kgsl_pwrscale_detach_policy(device);
	kgsl_pwrscale_close(device);

	adreno_dispatcher_close(adreno_dev);
	adreno_ringbuffer_close(&adreno_dev->ringbuffer);
	kgsl_device_platform_remove(device);

	return 0;
}

static int adreno_init(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	kgsl_pwrctrl_set_state(device, KGSL_STATE_INIT);

	/* Power up the device */
	kgsl_pwrctrl_enable(device);

	/* Identify the specific GPU */
	adreno_identify_gpu(adreno_dev);

	if (adreno_ringbuffer_read_pm4_ucode(device)) {
		KGSL_DRV_ERR(device, "Reading pm4 microcode failed %s\n",
			adreno_dev->pm4_fwfile);
		BUG_ON(1);
	}

	if (adreno_ringbuffer_read_pfp_ucode(device)) {
		KGSL_DRV_ERR(device, "Reading pfp microcode failed %s\n",
			adreno_dev->pfp_fwfile);
		BUG_ON(1);
	}

	if (adreno_dev->gpurev == ADRENO_REV_UNKNOWN) {
		KGSL_DRV_ERR(device, "Unknown chip ID %x\n",
			adreno_dev->chip_id);
		BUG_ON(1);
	}

	/*
	 * Check if firmware supports the sync lock PM4 packets needed
	 * for IOMMUv1
	 */

	if ((adreno_dev->pm4_fw_version >=
		adreno_gpulist[adreno_dev->gpulist_index].sync_lock_pm4_ver) &&
		(adreno_dev->pfp_fw_version >=
		adreno_gpulist[adreno_dev->gpulist_index].sync_lock_pfp_ver))
		device->mmu.flags |= KGSL_MMU_FLAGS_IOMMU_SYNC;

	/* Assign correct RBBM status register to hang detect regs
	 */
	ft_detect_regs[0] = adreno_dev->gpudev->reg_rbbm_status;

	if (!adreno_is_a2xx(adreno_dev))
		adreno_perfcounter_init(device);

	/* Power down the device */
	kgsl_pwrctrl_disable(device);

	return 0;
}

static int adreno_start(struct kgsl_device *device)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int state = device->state;

	kgsl_cffdump_open(device);

	kgsl_pwrctrl_set_state(device, KGSL_STATE_INIT);

	/* Power up the device */
	kgsl_pwrctrl_enable(device);

	/* Set up a2xx special case */
	if (adreno_is_a2xx(adreno_dev)) {
		/*
		 * the MH_CLNT_INTF_CTRL_CONFIG registers aren't present
		 * on older gpus
		 */
		if (adreno_is_a20x(adreno_dev)) {
			device->mh.mh_intf_cfg1 = 0;
			device->mh.mh_intf_cfg2 = 0;
		}

		kgsl_mh_start(device);
	}

	status = kgsl_mmu_start(device);
	if (status)
		goto error_clk_off;

	status = adreno_ocmem_gmem_malloc(adreno_dev);
	if (status) {
		KGSL_DRV_ERR(device, "OCMEM malloc failed\n");
		goto error_mmu_off;
	}

	/* Start the GPU */
	adreno_dev->gpudev->start(adreno_dev);

	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_ON);
	device->ftbl->irqctrl(device, 1);

	status = adreno_ringbuffer_start(&adreno_dev->ringbuffer);
	if (status)
		goto error_irq_off;

	/* Start the dispatcher */
	adreno_dispatcher_start(adreno_dev);

	if (!adreno_is_a2xx(adreno_dev))
		adreno_perfcounter_start(adreno_dev);

	device->reset_counter++;

	return 0;

error_irq_off:
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_OFF);

error_mmu_off:
	kgsl_mmu_stop(&device->mmu);

error_clk_off:
	if (KGSL_STATE_DUMP_AND_FT != device->state) {
		kgsl_pwrctrl_disable(device);
		/* set the state back to original state */
		kgsl_pwrctrl_set_state(device, state);
	}

	return status;
}

static int adreno_stop(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	if (adreno_dev->drawctxt_active)
		kgsl_context_put(&adreno_dev->drawctxt_active->base);

	adreno_dev->drawctxt_active = NULL;

	adreno_dispatcher_stop(adreno_dev);
	adreno_ringbuffer_stop(&adreno_dev->ringbuffer);

	kgsl_mmu_stop(&device->mmu);

	device->ftbl->irqctrl(device, 0);
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_OFF);
	del_timer_sync(&device->idle_timer);

	adreno_ocmem_gmem_free(adreno_dev);

	/* Power down the device */
	kgsl_pwrctrl_disable(device);

	kgsl_cffdump_close(device->id);

	return 0;
}

/**
 * adreno_reset() - Helper function to reset the GPU
 * @device: Pointer to the KGSL device structure for the GPU
 *
 * Helper function to reset the GPU hardware by toggling the footswitch
 */
int adreno_reset(struct kgsl_device *device)
{
	int ret;

	ret = adreno_stop(device);
	if (ret)
		return ret;

	ret = adreno_init(device);
	if (ret)
		return ret;

	ret = adreno_start(device);

	if (ret == 0) {
		/*
		 * If active_cnt is non-zero then the system was active before
		 * going into a reset - put it back in that state
		 */

		if (atomic_read(&device->active_cnt))
			kgsl_pwrctrl_set_state(device, KGSL_STATE_ACTIVE);
	}

	return ret;
}

static int adreno_getproperty(struct kgsl_device *device,
				enum kgsl_property_type type,
				void *value,
				unsigned int sizebytes)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	switch (type) {
	case KGSL_PROP_DEVICE_INFO:
		{
			struct kgsl_devinfo devinfo;

			if (sizebytes != sizeof(devinfo)) {
				status = -EINVAL;
				break;
			}

			memset(&devinfo, 0, sizeof(devinfo));
			devinfo.device_id = device->id+1;
			devinfo.chip_id = adreno_dev->chip_id;
			devinfo.mmu_enabled = kgsl_mmu_enabled();
			devinfo.gpu_id = adreno_dev->gpurev;
			devinfo.gmem_gpubaseaddr = adreno_dev->gmem_base;
			devinfo.gmem_sizebytes = adreno_dev->gmem_size;

			if (copy_to_user(value, &devinfo, sizeof(devinfo)) !=
					0) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_DEVICE_SHADOW:
		{
			struct kgsl_shadowprop shadowprop;

			if (sizebytes != sizeof(shadowprop)) {
				status = -EINVAL;
				break;
			}
			memset(&shadowprop, 0, sizeof(shadowprop));
			if (device->memstore.hostptr) {
				/*NOTE: with mmu enabled, gpuaddr doesn't mean
				 * anything to mmap().
				 */
				shadowprop.gpuaddr = device->memstore.gpuaddr;
				shadowprop.size = device->memstore.size;
				/* GSL needs this to be set, even if it
				   appears to be meaningless */
				shadowprop.flags = KGSL_FLAGS_INITIALIZED |
					KGSL_FLAGS_PER_CONTEXT_TIMESTAMPS;
			}
			if (copy_to_user(value, &shadowprop,
				sizeof(shadowprop))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_MMU_ENABLE:
		{
			int mmu_prop = kgsl_mmu_enabled();

			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &mmu_prop, sizeof(mmu_prop))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_INTERRUPT_WAITS:
		{
			int int_waits = 1;
			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &int_waits, sizeof(int))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	default:
		status = -EINVAL;
	}

	return status;
}

static int adreno_setproperty(struct kgsl_device *device,
				enum kgsl_property_type type,
				void *value,
				unsigned int sizebytes)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	switch (type) {
	case KGSL_PROP_PWRCTRL: {
			unsigned int enable;
			struct kgsl_device_platform_data *pdata =
				kgsl_device_get_drvdata(device);

			if (sizebytes != sizeof(enable))
				break;

			if (copy_from_user(&enable, (void __user *) value,
				sizeof(enable))) {
				status = -EFAULT;
				break;
			}

			if (enable) {
				if (pdata->nap_allowed)
					device->pwrctrl.nap_allowed = true;
				adreno_dev->fast_hang_detect = 1;
				kgsl_pwrscale_enable(device);
			} else {
				device->pwrctrl.nap_allowed = false;
				adreno_dev->fast_hang_detect = 0;
				kgsl_pwrscale_disable(device);
			}

			status = 0;
		}
		break;
	default:
		break;
	}

	return status;
}

/**
 * adreno_hw_isidle() - Check if the GPU core is idle
 * @device: Pointer to the KGSL device structure for the GPU
 *
 * Return true if the RBBM status register for the GPU type indicates that the
 * hardware is idle
 */
static bool adreno_hw_isidle(struct kgsl_device *device)
{
	unsigned int reg_rbbm_status;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	/* Don't consider ourselves idle if there is an IRQ pending */
	if (adreno_dev->gpudev->irq_pending(adreno_dev))
		return false;

	/* Read the correct RBBM status for the GPU type */
	adreno_regread(device,
		adreno_dev->gpudev->reg_rbbm_status,
		&reg_rbbm_status);

	if (adreno_is_a2xx(adreno_dev)) {
		if (reg_rbbm_status == 0x110)
			return true;
	} else if (adreno_is_a3xx(adreno_dev)) {
		if (!(reg_rbbm_status & 0x80000000))
			return true;
	} else {
		BUG();
	}

	return false;
}

/**
 * adreno_isidle() - return true if the GPU hardware is idle
 * @device: Pointer to the KGSL device structure for the GPU
 *
 * Return true if the GPU hardware is idle and there are no commands pending in
 * the ringbuffer
 */
static bool adreno_isidle(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	/* If the device isn't active, don't force it on. */
	if (device->state != KGSL_STATE_ACTIVE)
		return true;

	GSL_RB_GET_READPTR(rb, &rb->rptr);

	if (rb->rptr == rb->wptr)
		return adreno_hw_isidle(device);

	return false;
}

/**
 * adreno_idle() - wait for the GPU hardware to go idle
 * @device: Pointer to the KGSL device structure for the GPU
 *
 * Wait up to ADRENO_IDLE_TIMEOUT milliseconds for the GPU hardware to go quiet.
 */

int adreno_idle(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned long wait = jiffies + msecs_to_jiffies(ADRENO_IDLE_TIMEOUT);

	/*
	 * Make sure the device mutex is held so the dispatcher can't send any
	 * more commands to the hardware
	 */

	BUG_ON(!mutex_is_locked(&device->mutex));

	if (adreno_is_a2xx(adreno_dev))
		kgsl_cffdump_regpoll(device,
			adreno_dev->gpudev->reg_rbbm_status << 2, 0x110, 0x110);
	else
		kgsl_cffdump_regpoll(device,
			adreno_dev->gpudev->reg_rbbm_status << 2, 0,
			0x80000000);

	while (time_before(jiffies, wait)) {
		if (adreno_isidle(device))
			return 0;
	}

	kgsl_postmortem_dump(device, 0);

	return -ETIMEDOUT;
}

/**
 * adreno_drain() - Drain the dispatch queue
 * @device: Pointer to the KGSL device structure for the GPU
 *
 * Tell the dispatcher to pause - this has the effect of draining the inflight
 * command batches
 */
static int adreno_drain(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	adreno_dispatcher_pause(adreno_dev);
	return 0;
}

/* Caller must hold the device mutex. */
static int adreno_suspend_context(struct kgsl_device *device)
{
	int status = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	/* switch to NULL ctxt */
	if (adreno_dev->drawctxt_active != NULL) {
		adreno_drawctxt_switch(adreno_dev, NULL, 0);
		status = adreno_idle(device);
	}

	return status;
}

/* Find a memory structure attached to an adreno context */

struct kgsl_memdesc *adreno_find_ctxtmem(struct kgsl_device *device,
	unsigned int pt_base, unsigned int gpuaddr, unsigned int size)
{
	struct kgsl_context *context;
	int next = 0;
	struct kgsl_memdesc *desc = NULL;

	read_lock(&device->context_lock);
	while (1) {
		context = idr_get_next(&device->context_idr, &next);
		if (context == NULL)
			break;

		if (kgsl_mmu_pt_equal(&device->mmu, context->pagetable,
					pt_base)) {
			struct adreno_context *adreno_context;

			adreno_context = ADRENO_CONTEXT(context);
			desc = &adreno_context->gpustate;
			if (kgsl_gpuaddr_in_memdesc(desc, gpuaddr, size))
				break;

			desc = &adreno_context->context_gmem_shadow.gmemshadow;
			if (kgsl_gpuaddr_in_memdesc(desc, gpuaddr, size))
				break;
		}
		next = next + 1;
		desc = NULL;
	}
	read_unlock(&device->context_lock);
	return desc;
}

struct kgsl_memdesc *adreno_find_region(struct kgsl_device *device,
						unsigned int pt_base,
						unsigned int gpuaddr,
						unsigned int size)
{
	struct kgsl_mem_entry *entry;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *ringbuffer = &adreno_dev->ringbuffer;

	if (kgsl_gpuaddr_in_memdesc(&ringbuffer->buffer_desc, gpuaddr, size))
		return &ringbuffer->buffer_desc;

	if (kgsl_gpuaddr_in_memdesc(&ringbuffer->memptrs_desc, gpuaddr, size))
		return &ringbuffer->memptrs_desc;

	if (kgsl_gpuaddr_in_memdesc(&device->memstore, gpuaddr, size))
		return &device->memstore;

	if (kgsl_gpuaddr_in_memdesc(&device->mmu.setstate_memory, gpuaddr,
					size))
		return &device->mmu.setstate_memory;

	entry = kgsl_get_mem_entry(device, pt_base, gpuaddr, size);

	if (entry)
		return &entry->memdesc;

	return adreno_find_ctxtmem(device, pt_base, gpuaddr, size);
}

uint8_t *adreno_convertaddr(struct kgsl_device *device, unsigned int pt_base,
			    unsigned int gpuaddr, unsigned int size)
{
	struct kgsl_memdesc *memdesc;

	memdesc = adreno_find_region(device, pt_base, gpuaddr, size);

	return memdesc ? kgsl_gpuaddr_to_vaddr(memdesc, gpuaddr) : NULL;
}

void adreno_regread(struct kgsl_device *device, unsigned int offsetwords,
				unsigned int *value)
{
	unsigned int *reg;
	BUG_ON(offsetwords*sizeof(uint32_t) >= device->reg_len);
	reg = (unsigned int *)(device->reg_virt + (offsetwords << 2));

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	/*ensure this read finishes before the next one.
	 * i.e. act like normal readl() */
	*value = __raw_readl(reg);
	rmb();
}

void adreno_regwrite(struct kgsl_device *device, unsigned int offsetwords,
				unsigned int value)
{
	unsigned int *reg;

	BUG_ON(offsetwords*sizeof(uint32_t) >= device->reg_len);

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	kgsl_trace_regwrite(device, offsetwords, value);

	kgsl_cffdump_regwrite(device->id, offsetwords << 2, value);
	reg = (unsigned int *)(device->reg_virt + (offsetwords << 2));

	/*ensure previous writes post before this one,
	 * i.e. act like normal writel() */
	wmb();
	__raw_writel(value, reg);
}

/**
 * adreno_waittimestamp - sleep while waiting for the specified timestamp
 * @device - pointer to a KGSL device structure
 * @context - pointer to the active kgsl context
 * @timestamp - GPU timestamp to wait for
 * @msecs - amount of time to wait (in milliseconds)
 *
 * Wait up to 'msecs' milliseconds for the specified timestamp to expire.
 */
static int adreno_waittimestamp(struct kgsl_device *device,
		struct kgsl_context *context,
		unsigned int timestamp,
		unsigned int msecs)
{
	int ret;
	struct adreno_context *drawctxt;

	if (context == NULL) {
		/* If they are doing then complain once */
		dev_WARN_ONCE(device->dev, 1,
			"IOCTL_KGSL_DEVICE_WAITTIMESTAMP is deprecated\n");
		return -EINVAL;
	}

	/* Return -EINVAL if the context has been detached */
	if (kgsl_context_detached(context))
		return -EINVAL;

	ret = adreno_drawctxt_wait(ADRENO_DEVICE(device), context,
		timestamp, msecs_to_jiffies(msecs));

	/* If the context got invalidated then return a specific error */
	drawctxt = ADRENO_CONTEXT(context);

	if (drawctxt->state == ADRENO_CONTEXT_STATE_INVALID)
		ret = -EDEADLK;

	return ret;
}

static unsigned int adreno_readtimestamp(struct kgsl_device *device,
		struct kgsl_context *context, enum kgsl_timestamp_type type)
{
	unsigned int timestamp = 0;
	unsigned int id = context ? context->id : KGSL_MEMSTORE_GLOBAL;

	/*
	 * If the context is detached we are in a race with
	 * the context being destroyed by userspace so bail.
	 */
	if (context && kgsl_context_detached(context)) {
		KGSL_DRV_WARN(device, "context was detached");
		return timestamp;
	}
	switch (type) {
	case KGSL_TIMESTAMP_QUEUED: {
		struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

		timestamp = adreno_context_timestamp(context,
				&adreno_dev->ringbuffer);
		break;
	}
	case KGSL_TIMESTAMP_CONSUMED:
		kgsl_sharedmem_readl(&device->memstore, &timestamp,
			KGSL_MEMSTORE_OFFSET(id, soptimestamp));
		break;
	case KGSL_TIMESTAMP_RETIRED:
		kgsl_sharedmem_readl(&device->memstore, &timestamp,
			KGSL_MEMSTORE_OFFSET(id, eoptimestamp));
		break;
	}

	rmb();

	return timestamp;
}

static long adreno_ioctl(struct kgsl_device_private *dev_priv,
			      unsigned int cmd, void *data)
{
	struct kgsl_device *device = dev_priv->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int result = 0;

	switch (cmd) {
	case IOCTL_KGSL_DRAWCTXT_SET_BIN_BASE_OFFSET: {
		struct kgsl_drawctxt_set_bin_base_offset *binbase = data;
		struct kgsl_context *context;

		binbase = data;

		context = kgsl_context_get_owner(dev_priv,
			binbase->drawctxt_id);
		if (context) {
			adreno_drawctxt_set_bin_base_offset(
				device, context, binbase->offset);
		} else {
			result = -EINVAL;
			KGSL_DRV_ERR(device,
				"invalid drawctxt drawctxt_id %d "
				"device_id=%d\n",
				binbase->drawctxt_id, device->id);
		}

		kgsl_context_put(context);
		break;
	}
	case IOCTL_KGSL_PERFCOUNTER_GET: {
		struct kgsl_perfcounter_get *get = data;
		result = adreno_perfcounter_get(adreno_dev, get->groupid,
			get->countable, &get->offset, PERFCOUNTER_FLAG_NONE);
		break;
	}
	case IOCTL_KGSL_PERFCOUNTER_PUT: {
		struct kgsl_perfcounter_put *put = data;
		result = adreno_perfcounter_put(adreno_dev, put->groupid,
			put->countable);
		break;
	}
	case IOCTL_KGSL_PERFCOUNTER_QUERY: {
		struct kgsl_perfcounter_query *query = data;
		result = adreno_perfcounter_query_group(adreno_dev,
			query->groupid, query->countables,
			query->count, &query->max_counters);
		break;
	}
	case IOCTL_KGSL_PERFCOUNTER_READ: {
		struct kgsl_perfcounter_read *read = data;
		result = adreno_perfcounter_read_group(adreno_dev,
			read->reads, read->count);
		break;
	}
	default:
		KGSL_DRV_INFO(dev_priv->device,
			"invalid ioctl code %08x\n", cmd);
		result = -ENOIOCTLCMD;
		break;
	}
	return result;

}

static inline s64 adreno_ticks_to_us(u32 ticks, u32 gpu_freq)
{
	gpu_freq /= 1000000;
	return ticks / gpu_freq;
}

static void adreno_power_stats(struct kgsl_device *device,
				struct kgsl_power_stats *stats)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	unsigned int cycles = 0;

	/*
	 * Get the busy cycles counted since the counter was last reset.
	 * If we're not currently active, there shouldn't have been
	 * any cycles since the last time this function was called.
	 */
	if (device->state == KGSL_STATE_ACTIVE)
		cycles = adreno_dev->gpudev->busy_cycles(adreno_dev);

	/*
	 * In order to calculate idle you have to have run the algorithm
	 * at least once to get a start time.
	 */
	if (pwr->time != 0) {
		s64 tmp = ktime_to_us(ktime_get());
		stats->total_time = tmp - pwr->time;
		pwr->time = tmp;
		stats->busy_time = adreno_ticks_to_us(cycles, device->pwrctrl.
				pwrlevels[device->pwrctrl.active_pwrlevel].
				gpu_freq);
	} else {
		stats->total_time = 0;
		stats->busy_time = 0;
		pwr->time = ktime_to_us(ktime_get());
	}
}

void adreno_irqctrl(struct kgsl_device *device, int state)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	adreno_dev->gpudev->irq_control(adreno_dev, state);
}

static unsigned int adreno_gpuid(struct kgsl_device *device,
	unsigned int *chipid)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	/* Some applications need to know the chip ID too, so pass
	 * that as a parameter */

	if (chipid != NULL)
		*chipid = adreno_dev->chip_id;

	/* Standard KGSL gpuid format:
	 * top word is 0x0002 for 2D or 0x0003 for 3D
	 * Bottom word is core specific identifer
	 */

	return (0x0003 << 16) | ((int) adreno_dev->gpurev);
}

static const struct kgsl_functable adreno_functable = {
	/* Mandatory functions */
	.regread = adreno_regread,
	.regwrite = adreno_regwrite,
	.idle = adreno_idle,
	.isidle = adreno_isidle,
	.suspend_context = adreno_suspend_context,
	.init = adreno_init,
	.start = adreno_start,
	.stop = adreno_stop,
	.getproperty = adreno_getproperty,
	.waittimestamp = adreno_waittimestamp,
	.readtimestamp = adreno_readtimestamp,
	.issueibcmds = adreno_ringbuffer_issueibcmds,
	.ioctl = adreno_ioctl,
	.setup_pt = adreno_setup_pt,
	.cleanup_pt = adreno_cleanup_pt,
	.power_stats = adreno_power_stats,
	.irqctrl = adreno_irqctrl,
	.gpuid = adreno_gpuid,
	.snapshot = adreno_snapshot,
	.irq_handler = adreno_irq_handler,
	.drain = adreno_drain,
	/* Optional functions */
	.setstate = adreno_setstate,
	.drawctxt_create = adreno_drawctxt_create,
	.drawctxt_detach = adreno_drawctxt_detach,
	.drawctxt_destroy = adreno_drawctxt_destroy,
	.setproperty = adreno_setproperty,
	.postmortem_dump = adreno_dump,
	.drawctxt_sched = adreno_drawctxt_sched,
};

static struct platform_driver adreno_platform_driver = {
	.probe = adreno_probe,
	.remove = __devexit_p(adreno_remove),
	.suspend = kgsl_suspend_driver,
	.resume = kgsl_resume_driver,
	.id_table = adreno_id_table,
	.driver = {
		.owner = THIS_MODULE,
		.name = DEVICE_3D_NAME,
		.pm = &kgsl_pm_ops,
		.of_match_table = adreno_match_table,
	}
};

static int __init kgsl_3d_init(void)
{
	return platform_driver_register(&adreno_platform_driver);
}

static void __exit kgsl_3d_exit(void)
{
	platform_driver_unregister(&adreno_platform_driver);
}

module_init(kgsl_3d_init);
module_exit(kgsl_3d_exit);

MODULE_DESCRIPTION("3D Graphics driver");
MODULE_VERSION("1.2");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:kgsl_3d");
