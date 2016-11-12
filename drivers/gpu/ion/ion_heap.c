/*
 * drivers/gpu/ion/ion_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/ion.h>
#include "ion_priv.h"
#include <linux/msm_ion.h>

struct ion_heap *ion_heap_create(struct ion_platform_heap *heap_data)
{
	struct ion_heap *heap = NULL;

	switch ((int) heap_data->type) {
	case ION_HEAP_TYPE_SYSTEM_CONTIG:
		pr_err("%s: Heap type is disabled: %d\n", __func__,
				heap_data->type);
		return ERR_PTR(-EINVAL);
	case ION_HEAP_TYPE_SYSTEM:
		heap = ion_system_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_CARVEOUT:
		heap = ion_carveout_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_IOMMU:
		heap = ion_iommu_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_CP:
		heap = ion_cp_heap_create(heap_data);
		break;
#ifdef CONFIG_CMA
	case ION_HEAP_TYPE_DMA:
		heap = ion_cma_heap_create(heap_data);
		break;
#endif
	default:
		pr_err("%s: Invalid heap type %d\n", __func__,
		       heap_data->type);
		return ERR_PTR(-EINVAL);
	}

	if (IS_ERR_OR_NULL(heap)) {
		pr_err("%s: error creating heap %s type %d base %lu size %u\n",
		       __func__, heap_data->name, heap_data->type,
		       heap_data->base, heap_data->size);
		return ERR_PTR(-EINVAL);
	}

	heap->name = heap_data->name;
	heap->id = heap_data->id;
	heap->priv = heap_data->priv;
	return heap;
}

void ion_heap_destroy(struct ion_heap *heap)
{
	if (!heap)
		return;

	switch ((int) heap->type) {
	case ION_HEAP_TYPE_SYSTEM_CONTIG:
		pr_err("%s: Heap type is disabled: %d\n", __func__,
		       heap->type);
		break;
	case ION_HEAP_TYPE_SYSTEM:
		ion_system_heap_destroy(heap);
		break;
	case ION_HEAP_TYPE_CARVEOUT:
		ion_carveout_heap_destroy(heap);
		break;
	case ION_HEAP_TYPE_IOMMU:
		ion_iommu_heap_destroy(heap);
		break;
	case ION_HEAP_TYPE_CP:
		ion_cp_heap_destroy(heap);
		break;
#ifdef CONFIG_CMA
	case ION_HEAP_TYPE_DMA:
		ion_cma_heap_destroy(heap);
		break;
#endif
	default:
		pr_err("%s: Invalid heap type %d\n", __func__,
		       heap->type);
	}
}
