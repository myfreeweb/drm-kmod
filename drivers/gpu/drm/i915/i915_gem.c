/*
 * Copyright © 2008-2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <drm/drm_vma_manager.h>
#include <drm/i915_drm.h>
#include <linux/dma-fence-array.h>
#include <linux/kthread.h>
#include <linux/dma-resv.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/swap.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/mman.h>

#include "display/intel_display.h"
#include "display/intel_frontbuffer.h"

#include "gem/i915_gem_clflush.h"
#include "gem/i915_gem_context.h"
#include "gem/i915_gem_ioctls.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_pm.h"
#include "gt/intel_context.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt_requests.h"
#include "gt/intel_mocs.h"
#include "gt/intel_reset.h"
#include "gt/intel_renderstate.h"
#include "gt/intel_rps.h"
#include "gt/intel_workarounds.h"

#include "i915_drv.h"
#include "i915_scatterlist.h"
#include "i915_trace.h"
#include "i915_vgpu.h"

#include "intel_pm.h"

static int
insert_mappable_node(struct i915_ggtt *ggtt, struct drm_mm_node *node, u32 size)
{
	int err;

	err = mutex_lock_interruptible(&ggtt->vm.mutex);
	if (err)
		return err;

	memset(node, 0, sizeof(*node));
	err = drm_mm_insert_node_in_range(&ggtt->vm.mm, node,
					  size, 0, I915_COLOR_UNEVICTABLE,
					  0, ggtt->mappable_end,
					  DRM_MM_INSERT_LOW);

	mutex_unlock(&ggtt->vm.mutex);

	return err;
}

static void
remove_mappable_node(struct i915_ggtt *ggtt, struct drm_mm_node *node)
{
	mutex_lock(&ggtt->vm.mutex);
	drm_mm_remove_node(node);
	mutex_unlock(&ggtt->vm.mutex);
}

int
i915_gem_get_aperture_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct i915_ggtt *ggtt = &to_i915(dev)->ggtt;
	struct drm_i915_gem_get_aperture *args = data;
	struct i915_vma *vma;
	u64 pinned;

	if (mutex_lock_interruptible(&ggtt->vm.mutex))
		return -EINTR;

	pinned = ggtt->vm.reserved;
	list_for_each_entry(vma, &ggtt->vm.bound_list, vm_link)
		if (i915_vma_is_pinned(vma))
			pinned += vma->node.size;

	mutex_unlock(&ggtt->vm.mutex);

	args->aper_size = ggtt->vm.total;
	args->aper_available_size = args->aper_size - pinned;

	return 0;
}

int i915_gem_object_unbind(struct drm_i915_gem_object *obj,
			   unsigned long flags)
{
	struct intel_runtime_pm *rpm = &to_i915(obj->base.dev)->runtime_pm;
	LIST_HEAD(still_in_list);
	intel_wakeref_t wakeref;
	struct i915_vma *vma;
	int ret;

	if (!atomic_read(&obj->bind_count))
		return 0;

	/*
	 * As some machines use ACPI to handle runtime-resume callbacks, and
	 * ACPI is quite kmalloc happy, we cannot resume beneath the vm->mutex
	 * as they are required by the shrinker. Ergo, we wake the device up
	 * first just in case.
	 */
	wakeref = intel_runtime_pm_get(rpm);

try_again:
	ret = 0;
	spin_lock(&obj->vma.lock);
	while (!ret && (vma = list_first_entry_or_null(&obj->vma.list,
						       struct i915_vma,
						       obj_link))) {
		struct i915_address_space *vm = vma->vm;
		bool awake = false;

		ret = -EAGAIN;
		if (!i915_vm_tryopen(vm))
			break;

		/* Prevent vma being freed by i915_vma_parked as we unbind */
		if (intel_gt_pm_get_if_awake(vm->gt)) {
			awake = true;
		} else {
			if (i915_vma_is_closed(vma)) {
				spin_unlock(&obj->vma.lock);
				i915_vma_parked(vm->gt);
				goto err_vm;
			}
		}

		list_move_tail(&vma->obj_link, &still_in_list);
		spin_unlock(&obj->vma.lock);

		ret = -EBUSY;
		if (flags & I915_GEM_OBJECT_UNBIND_ACTIVE ||
		    !i915_vma_is_active(vma))
			ret = i915_vma_unbind(vma);

		if (awake)
			intel_gt_pm_put(vm->gt);
err_vm:
		i915_vm_close(vm);
		spin_lock(&obj->vma.lock);
	}
	list_splice_init(&still_in_list, &obj->vma.list);
	spin_unlock(&obj->vma.lock);

	if (ret == -EAGAIN && flags & I915_GEM_OBJECT_UNBIND_ACTIVE) {
		rcu_barrier(); /* flush the i915_vm_release() */
		goto try_again;
	}

	intel_runtime_pm_put(rpm, wakeref);

	return ret;
}

static int
i915_gem_phys_pwrite(struct drm_i915_gem_object *obj,
		     struct drm_i915_gem_pwrite *args,
		     struct drm_file *file)
{
	void *vaddr = obj->phys_handle->vaddr + args->offset;
	char __user *user_data = u64_to_user_ptr(args->data_ptr);

	/*
	 * We manually control the domain here and pretend that it
	 * remains coherent i.e. in the GTT domain, like shmem_pwrite.
	 */
	i915_gem_object_invalidate_frontbuffer(obj, ORIGIN_CPU);

	if (copy_from_user(vaddr, user_data, args->size))
		return -EFAULT;

	drm_clflush_virt_range(vaddr, args->size);
	intel_gt_chipset_flush(&to_i915(obj->base.dev)->gt);

	i915_gem_object_flush_frontbuffer(obj, ORIGIN_CPU);
	return 0;
}

static int
i915_gem_create(struct drm_file *file,
		struct drm_i915_private *dev_priv,
		u64 *size_p,
		u32 *handle_p)
{
	struct drm_i915_gem_object *obj;
	u32 handle;
	u64 size;
	int ret;

	size = round_up(*size_p, PAGE_SIZE);
	if (size == 0)
		return -EINVAL;

	/* Allocate the new object */
	obj = i915_gem_object_create_shmem(dev_priv, size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_handle_create(file, &obj->base, &handle);
	/* drop reference from allocate - handle holds it now */
	i915_gem_object_put(obj);
	if (ret)
		return ret;

	*handle_p = handle;
	*size_p = size;
	return 0;
}

int
i915_gem_dumb_create(struct drm_file *file,
		     struct drm_device *dev,
		     struct drm_mode_create_dumb *args)
{
	int cpp = DIV_ROUND_UP(args->bpp, 8);
	u32 format;

	switch (cpp) {
	case 1:
		format = DRM_FORMAT_C8;
		break;
	case 2:
		format = DRM_FORMAT_RGB565;
		break;
	case 4:
		format = DRM_FORMAT_XRGB8888;
		break;
	default:
		return -EINVAL;
	}

	/* have to work out size/pitch and return them */
	args->pitch = ALIGN(args->width * cpp, 64);

	/* align stride to page size so that we can remap */
	if (args->pitch > intel_plane_fb_max_stride(to_i915(dev), format,
						    DRM_FORMAT_MOD_LINEAR))
		args->pitch = ALIGN(args->pitch, 4096);

	args->size = args->pitch * args->height;
	return i915_gem_create(file, to_i915(dev),
			       &args->size, &args->handle);
}

/**
 * Creates a new mm object and returns a handle to it.
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 */
int
i915_gem_create_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_create *args = data;

	i915_gem_flush_free_objects(dev_priv);

	return i915_gem_create(file, dev_priv,
			       &args->size, &args->handle);
}

static int
shmem_pread(struct page *page, int offset, int len, char __user *user_data,
	    bool needs_clflush)
{
	char *vaddr;
	int ret;

	vaddr = kmap(page);

	if (needs_clflush)
		drm_clflush_virt_range(vaddr + offset, len);

	ret = __copy_to_user(user_data, vaddr + offset, len);

	kunmap(page);

	return ret ? -EFAULT : 0;
}

static int
i915_gem_shmem_pread(struct drm_i915_gem_object *obj,
		     struct drm_i915_gem_pread *args)
{
	unsigned int needs_clflush;
	unsigned int idx, offset;
	struct dma_fence *fence;
	char __user *user_data;
	u64 remain;
	int ret;

	ret = i915_gem_object_prepare_read(obj, &needs_clflush);
	if (ret)
		return ret;

	fence = i915_gem_object_lock_fence(obj);
	i915_gem_object_finish_access(obj);
	if (!fence)
		return -ENOMEM;

	remain = args->size;
	user_data = u64_to_user_ptr(args->data_ptr);
	offset = offset_in_page(args->offset);
	for (idx = args->offset >> PAGE_SHIFT; remain; idx++) {
		struct page *page = i915_gem_object_get_page(obj, idx);
		unsigned int length = min_t(u64, remain, PAGE_SIZE - offset);

		ret = shmem_pread(page, offset, length, user_data,
				  needs_clflush);
		if (ret)
			break;

		remain -= length;
		user_data += length;
		offset = 0;
	}

	i915_gem_object_unlock_fence(obj, fence);
	return ret;
}

static inline bool
gtt_user_read(struct io_mapping *mapping,
	      loff_t base, int offset,
	      char __user *user_data, int length)
{
	void __iomem *vaddr;
	unsigned long unwritten;

	/* We can use the cpu mem copy function because this is X86. */
	vaddr = io_mapping_map_atomic_wc(mapping, base);
	unwritten = __copy_to_user_inatomic(user_data,
					    (void __force *)vaddr + offset,
					    length);
	io_mapping_unmap_atomic(vaddr);
	if (unwritten) {
		vaddr = io_mapping_map_wc(mapping, base, PAGE_SIZE);
		unwritten = copy_to_user(user_data,
					 (void __force *)vaddr + offset,
					 length);
		io_mapping_unmap(vaddr);
	}
	return unwritten;
}

static int
i915_gem_gtt_pread(struct drm_i915_gem_object *obj,
		   const struct drm_i915_gem_pread *args)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_ggtt *ggtt = &i915->ggtt;
	intel_wakeref_t wakeref;
	struct drm_mm_node node;
	struct dma_fence *fence;
	void __user *user_data;
	struct i915_vma *vma;
	u64 remain, offset;
	int ret;

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);
	vma = ERR_PTR(-ENODEV);
	if (!i915_gem_object_is_tiled(obj))
		vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 0,
					       PIN_MAPPABLE |
					       PIN_NONBLOCK /* NOWARN */ |
					       PIN_NOEVICT);
	if (!IS_ERR(vma)) {
		node.start = i915_ggtt_offset(vma);
		node.flags = 0;
	} else {
		ret = insert_mappable_node(ggtt, &node, PAGE_SIZE);
		if (ret)
			goto out_rpm;
		GEM_BUG_ON(!drm_mm_node_allocated(&node));
	}

	ret = i915_gem_object_lock_interruptible(obj);
	if (ret)
		goto out_unpin;

	ret = i915_gem_object_set_to_gtt_domain(obj, false);
	if (ret) {
		i915_gem_object_unlock(obj);
		goto out_unpin;
	}

	fence = i915_gem_object_lock_fence(obj);
	i915_gem_object_unlock(obj);
	if (!fence) {
		ret = -ENOMEM;
		goto out_unpin;
	}

	user_data = u64_to_user_ptr(args->data_ptr);
	remain = args->size;
	offset = args->offset;

	while (remain > 0) {
		/* Operation in this page
		 *
		 * page_base = page offset within aperture
		 * page_offset = offset within page
		 * page_length = bytes to copy for this page
		 */
		u32 page_base = node.start;
		unsigned page_offset = offset_in_page(offset);
		unsigned page_length = PAGE_SIZE - page_offset;
		page_length = remain < page_length ? remain : page_length;
		if (drm_mm_node_allocated(&node)) {
			ggtt->vm.insert_page(&ggtt->vm,
					     i915_gem_object_get_dma_address(obj, offset >> PAGE_SHIFT),
					     node.start, I915_CACHE_NONE, 0);
		} else {
			page_base += offset & PAGE_MASK;
		}

		if (gtt_user_read(&ggtt->iomap, page_base, page_offset,
				  user_data, page_length)) {
			ret = -EFAULT;
			break;
		}

		remain -= page_length;
		user_data += page_length;
		offset += page_length;
	}

	i915_gem_object_unlock_fence(obj, fence);
out_unpin:
	if (drm_mm_node_allocated(&node)) {
		ggtt->vm.clear_range(&ggtt->vm, node.start, node.size);
		remove_mappable_node(ggtt, &node);
	} else {
		i915_vma_unpin(vma);
	}
out_rpm:
	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
	return ret;
}

/**
 * Reads data from the object referenced by handle.
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 *
 * On error, the contents of *data are undefined.
 */
int
i915_gem_pread_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file)
{
	struct drm_i915_gem_pread *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	if (args->size == 0)
		return 0;

	if (!access_ok(u64_to_user_ptr(args->data_ptr),
		       args->size))
		return -EFAULT;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/* Bounds check source.  */
	if (range_overflows_t(u64, args->offset, args->size, obj->base.size)) {
		ret = -EINVAL;
		goto out;
	}

	trace_i915_gem_object_pread(obj, args->offset, args->size);

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		goto out;

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		goto out;

	ret = i915_gem_shmem_pread(obj, args);
	if (ret == -EFAULT || ret == -ENODEV)
		ret = i915_gem_gtt_pread(obj, args);

	i915_gem_object_unpin_pages(obj);
out:
	i915_gem_object_put(obj);
	return ret;
}

/* This is the fast write path which cannot handle
 * page faults in the source data
 */

static inline bool
ggtt_write(struct io_mapping *mapping,
	   loff_t base, int offset,
	   char __user *user_data, int length)
{
	void __iomem *vaddr;
	unsigned long unwritten;

	/* We can use the cpu mem copy function because this is X86. */
	vaddr = io_mapping_map_atomic_wc(mapping, base);
	unwritten = __copy_from_user_inatomic_nocache((void __force *)vaddr + offset,
						      user_data, length);
	io_mapping_unmap_atomic(vaddr);
	if (unwritten) {
		vaddr = io_mapping_map_wc(mapping, base, PAGE_SIZE);
		unwritten = copy_from_user((void __force *)vaddr + offset,
					   user_data, length);
		io_mapping_unmap(vaddr);
	}

	return unwritten;
}

/**
 * This is the fast pwrite path, where we copy the data directly from the
 * user into the GTT, uncached.
 * @obj: i915 GEM object
 * @args: pwrite arguments structure
 */
static int
i915_gem_gtt_pwrite_fast(struct drm_i915_gem_object *obj,
			 const struct drm_i915_gem_pwrite *args)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_ggtt *ggtt = &i915->ggtt;
	struct intel_runtime_pm *rpm = &i915->runtime_pm;
	intel_wakeref_t wakeref;
	struct drm_mm_node node;
	struct dma_fence *fence;
	struct i915_vma *vma;
	u64 remain, offset;
	void __user *user_data;
	int ret;

	if (i915_gem_object_has_struct_page(obj)) {
		/*
		 * Avoid waking the device up if we can fallback, as
		 * waking/resuming is very slow (worst-case 10-100 ms
		 * depending on PCI sleeps and our own resume time).
		 * This easily dwarfs any performance advantage from
		 * using the cache bypass of indirect GGTT access.
		 */
		wakeref = intel_runtime_pm_get_if_in_use(rpm);
		if (!wakeref)
			return -EFAULT;
	} else {
		/* No backing pages, no fallback, we must force GGTT access */
		wakeref = intel_runtime_pm_get(rpm);
	}

	vma = ERR_PTR(-ENODEV);
	if (!i915_gem_object_is_tiled(obj))
		vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 0,
					       PIN_MAPPABLE |
					       PIN_NONBLOCK /* NOWARN */ |
					       PIN_NOEVICT);
	if (!IS_ERR(vma)) {
		node.start = i915_ggtt_offset(vma);
		node.flags = 0;
	} else {
		ret = insert_mappable_node(ggtt, &node, PAGE_SIZE);
		if (ret)
			goto out_rpm;
		GEM_BUG_ON(!drm_mm_node_allocated(&node));
	}

	ret = i915_gem_object_lock_interruptible(obj);
	if (ret)
		goto out_unpin;

	ret = i915_gem_object_set_to_gtt_domain(obj, true);
	if (ret) {
		i915_gem_object_unlock(obj);
		goto out_unpin;
	}

	fence = i915_gem_object_lock_fence(obj);
	i915_gem_object_unlock(obj);
	if (!fence) {
		ret = -ENOMEM;
		goto out_unpin;
	}

	i915_gem_object_invalidate_frontbuffer(obj, ORIGIN_CPU);

	user_data = u64_to_user_ptr(args->data_ptr);
	offset = args->offset;
	remain = args->size;
	while (remain) {
		/* Operation in this page
		 *
		 * page_base = page offset within aperture
		 * page_offset = offset within page
		 * page_length = bytes to copy for this page
		 */
		u32 page_base = node.start;
		unsigned int page_offset = offset_in_page(offset);
		unsigned int page_length = PAGE_SIZE - page_offset;
		page_length = remain < page_length ? remain : page_length;
		if (drm_mm_node_allocated(&node)) {
			/* flush the write before we modify the GGTT */
			intel_gt_flush_ggtt_writes(ggtt->vm.gt);
			ggtt->vm.insert_page(&ggtt->vm,
					     i915_gem_object_get_dma_address(obj, offset >> PAGE_SHIFT),
					     node.start, I915_CACHE_NONE, 0);
			wmb(); /* flush modifications to the GGTT (insert_page) */
		} else {
			page_base += offset & PAGE_MASK;
		}
		/* If we get a fault while copying data, then (presumably) our
		 * source page isn't available.  Return the error and we'll
		 * retry in the slow path.
		 * If the object is non-shmem backed, we retry again with the
		 * path that handles page fault.
		 */
		if (ggtt_write(&ggtt->iomap, page_base, page_offset,
			       user_data, page_length)) {
			ret = -EFAULT;
			break;
		}

		remain -= page_length;
		user_data += page_length;
		offset += page_length;
	}

	intel_gt_flush_ggtt_writes(ggtt->vm.gt);
	intel_frontbuffer_flush(obj->frontbuffer, ORIGIN_CPU);

	i915_gem_object_unlock_fence(obj, fence);
out_unpin:
	if (drm_mm_node_allocated(&node)) {
		ggtt->vm.clear_range(&ggtt->vm, node.start, node.size);
		remove_mappable_node(ggtt, &node);
	} else {
		i915_vma_unpin(vma);
	}
out_rpm:
	intel_runtime_pm_put(rpm, wakeref);
	return ret;
}

/* Per-page copy function for the shmem pwrite fastpath.
 * Flushes invalid cachelines before writing to the target if
 * needs_clflush_before is set and flushes out any written cachelines after
 * writing if needs_clflush is set.
 */
static int
shmem_pwrite(struct page *page, int offset, int len, char __user *user_data,
	     bool needs_clflush_before,
	     bool needs_clflush_after)
{
	char *vaddr;
	int ret;

	vaddr = kmap(page);

	if (needs_clflush_before)
		drm_clflush_virt_range(vaddr + offset, len);

	ret = __copy_from_user(vaddr + offset, user_data, len);
	if (!ret && needs_clflush_after)
		drm_clflush_virt_range(vaddr + offset, len);

	kunmap(page);

	return ret ? -EFAULT : 0;
}

static int
i915_gem_shmem_pwrite(struct drm_i915_gem_object *obj,
		      const struct drm_i915_gem_pwrite *args)
{
	unsigned int partial_cacheline_write;
	unsigned int needs_clflush;
	unsigned int offset, idx;
	struct dma_fence *fence;
	void __user *user_data;
	u64 remain;
	int ret;

	ret = i915_gem_object_prepare_write(obj, &needs_clflush);
	if (ret)
		return ret;

	fence = i915_gem_object_lock_fence(obj);
	i915_gem_object_finish_access(obj);
	if (!fence)
		return -ENOMEM;

	/* If we don't overwrite a cacheline completely we need to be
	 * careful to have up-to-date data by first clflushing. Don't
	 * overcomplicate things and flush the entire patch.
	 */
	partial_cacheline_write = 0;
	if (needs_clflush & CLFLUSH_BEFORE)
		partial_cacheline_write = boot_cpu_data.x86_clflush_size - 1;

	user_data = u64_to_user_ptr(args->data_ptr);
	remain = args->size;
	offset = offset_in_page(args->offset);
	for (idx = args->offset >> PAGE_SHIFT; remain; idx++) {
		struct page *page = i915_gem_object_get_page(obj, idx);
		unsigned int length = min_t(u64, remain, PAGE_SIZE - offset);

		ret = shmem_pwrite(page, offset, length, user_data,
				   (offset | length) & partial_cacheline_write,
				   needs_clflush & CLFLUSH_AFTER);
		if (ret)
			break;

		remain -= length;
		user_data += length;
		offset = 0;
	}

	i915_gem_object_flush_frontbuffer(obj, ORIGIN_CPU);
	i915_gem_object_unlock_fence(obj, fence);

	return ret;
}

/**
 * Writes data to the object referenced by handle.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 *
 * On error, the contents of the buffer that were to be modified are undefined.
 */
int
i915_gem_pwrite_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_gem_pwrite *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	if (args->size == 0)
		return 0;

	if (!access_ok(u64_to_user_ptr(args->data_ptr), args->size))
		return -EFAULT;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/* Bounds check destination. */
	if (range_overflows_t(u64, args->offset, args->size, obj->base.size)) {
		ret = -EINVAL;
		goto err;
	}

	/* Writes not allowed into this read-only object */
	if (i915_gem_object_is_readonly(obj)) {
		ret = -EINVAL;
		goto err;
	}

	trace_i915_gem_object_pwrite(obj, args->offset, args->size);

	ret = -ENODEV;
	if (obj->ops->pwrite)
		ret = obj->ops->pwrite(obj, args);
	if (ret != -ENODEV)
		goto err;

	ret = i915_gem_object_wait(obj,
				   I915_WAIT_INTERRUPTIBLE |
				   I915_WAIT_ALL,
				   MAX_SCHEDULE_TIMEOUT);
	if (ret)
		goto err;

	ret = i915_gem_object_pin_pages(obj);
	if (ret)
		goto err;

	ret = -EFAULT;
	/* We can only do the GTT pwrite on untiled buffers, as otherwise
	 * it would end up going through the fenced access, and we'll get
	 * different detiling behavior between reading and writing.
	 * pread/pwrite currently are reading and writing from the CPU
	 * perspective, requiring manual detiling by the client.
	 */
	if (!i915_gem_object_has_struct_page(obj) ||
	    cpu_write_needs_clflush(obj))
		/* Note that the gtt paths might fail with non-page-backed user
		 * pointers (e.g. gtt mappings when moving data between
		 * textures). Fallback to the shmem path in that case.
		 */
		ret = i915_gem_gtt_pwrite_fast(obj, args);

	if (ret == -EFAULT || ret == -ENOSPC) {
		if (obj->phys_handle)
			ret = i915_gem_phys_pwrite(obj, args, file);
		else
			ret = i915_gem_shmem_pwrite(obj, args);
	}

	i915_gem_object_unpin_pages(obj);
err:
	i915_gem_object_put(obj);
	return ret;
}

/**
 * Called when user space has done writes to this buffer
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 */
int
i915_gem_sw_finish_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct drm_i915_gem_sw_finish *args = data;
	struct drm_i915_gem_object *obj;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/*
	 * Proxy objects are barred from CPU access, so there is no
	 * need to ban sw_finish as it is a nop.
	 */

	/* Pinned buffers may be scanout, so flush the cache */
	i915_gem_object_flush_if_display(obj);
	i915_gem_object_put(obj);

	return 0;
}

void i915_gem_runtime_suspend(struct drm_i915_private *i915)
{
	struct drm_i915_gem_object *obj, *on;
	int i;

	/*
	 * Only called during RPM suspend. All users of the userfault_list
	 * must be holding an RPM wakeref to ensure that this can not
	 * run concurrently with themselves (and use the struct_mutex for
	 * protection between themselves).
	 */

	list_for_each_entry_safe(obj, on,
				 &i915->ggtt.userfault_list, userfault_link)
		__i915_gem_object_release_mmap_gtt(obj);

	/*
	 * The fence will be lost when the device powers down. If any were
	 * in use by hardware (i.e. they are pinned), we should not be powering
	 * down! All other fences will be reacquired by the user upon waking.
	 */
	for (i = 0; i < i915->ggtt.num_fences; i++) {
		struct i915_fence_reg *reg = &i915->ggtt.fence_regs[i];

		/*
		 * Ideally we want to assert that the fence register is not
		 * live at this point (i.e. that no piece of code will be
		 * trying to write through fence + GTT, as that both violates
		 * our tracking of activity and associated locking/barriers,
		 * but also is illegal given that the hw is powered down).
		 *
		 * Previously we used reg->pin_count as a "liveness" indicator.
		 * That is not sufficient, and we need a more fine-grained
		 * tool if we want to have a sanity check here.
		 */

		if (!reg->vma)
			continue;

		GEM_BUG_ON(i915_vma_has_userfault(reg->vma));
		reg->dirty = true;
	}
}

struct i915_vma *
i915_gem_object_ggtt_pin(struct drm_i915_gem_object *obj,
			 const struct i915_ggtt_view *view,
			 u64 size,
			 u64 alignment,
			 u64 flags)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_ggtt *ggtt = &i915->ggtt;
	struct i915_vma *vma;
	int ret;

	if (i915_gem_object_never_bind_ggtt(obj))
		return ERR_PTR(-ENODEV);

	if (flags & PIN_MAPPABLE &&
	    (!view || view->type == I915_GGTT_VIEW_NORMAL)) {
		/*
		 * If the required space is larger than the available
		 * aperture, we will not able to find a slot for the
		 * object and unbinding the object now will be in
		 * vain. Worse, doing so may cause us to ping-pong
		 * the object in and out of the Global GTT and
		 * waste a lot of cycles under the mutex.
		 */
		if (obj->base.size > ggtt->mappable_end)
			return ERR_PTR(-E2BIG);

		/*
		 * If NONBLOCK is set the caller is optimistically
		 * trying to cache the full object within the mappable
		 * aperture, and *must* have a fallback in place for
		 * situations where we cannot bind the object. We
		 * can be a little more lax here and use the fallback
		 * more often to avoid costly migrations of ourselves
		 * and other objects within the aperture.
		 *
		 * Half-the-aperture is used as a simple heuristic.
		 * More interesting would to do search for a free
		 * block prior to making the commitment to unbind.
		 * That caters for the self-harm case, and with a
		 * little more heuristics (e.g. NOFAULT, NOEVICT)
		 * we could try to minimise harm to others.
		 */
		if (flags & PIN_NONBLOCK &&
		    obj->base.size > ggtt->mappable_end / 2)
			return ERR_PTR(-ENOSPC);
	}

	vma = i915_vma_instance(obj, &ggtt->vm, view);
	if (IS_ERR(vma))
		return vma;

	if (i915_vma_misplaced(vma, size, alignment, flags)) {
		if (flags & PIN_NONBLOCK) {
			if (i915_vma_is_pinned(vma) || i915_vma_is_active(vma))
				return ERR_PTR(-ENOSPC);

			if (flags & PIN_MAPPABLE &&
			    vma->fence_size > ggtt->mappable_end / 2)
				return ERR_PTR(-ENOSPC);
		}

		ret = i915_vma_unbind(vma);
		if (ret)
			return ERR_PTR(ret);
	}

	if (vma->fence && !i915_gem_object_is_tiled(obj)) {
		mutex_lock(&ggtt->vm.mutex);
		ret = i915_vma_revoke_fence(vma);
		mutex_unlock(&ggtt->vm.mutex);
		if (ret)
			return ERR_PTR(ret);
	}

	ret = i915_vma_pin(vma, size, alignment, flags | PIN_GLOBAL);
	if (ret)
		return ERR_PTR(ret);

	return vma;
}

int
i915_gem_madvise_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct drm_i915_gem_madvise *args = data;
	struct drm_i915_gem_object *obj;
	int err;

	switch (args->madv) {
	case I915_MADV_DONTNEED:
	case I915_MADV_WILLNEED:
	    break;
	default:
	    return -EINVAL;
	}

	obj = i915_gem_object_lookup(file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	err = mutex_lock_interruptible(&obj->mm.lock);
	if (err)
		goto out;

	if (i915_gem_object_has_pages(obj) &&
	    i915_gem_object_is_tiled(obj) &&
	    i915->quirks & QUIRK_PIN_SWIZZLED_PAGES) {
		if (obj->mm.madv == I915_MADV_WILLNEED) {
			GEM_BUG_ON(!obj->mm.quirked);
			__i915_gem_object_unpin_pages(obj);
			obj->mm.quirked = false;
		}
		if (args->madv == I915_MADV_WILLNEED) {
			GEM_BUG_ON(obj->mm.quirked);
			__i915_gem_object_pin_pages(obj);
			obj->mm.quirked = true;
		}
	}

	if (obj->mm.madv != __I915_MADV_PURGED)
		obj->mm.madv = args->madv;

	if (i915_gem_object_has_pages(obj)) {
		struct list_head *list;

		if (i915_gem_object_is_shrinkable(obj)) {
			unsigned long flags;

			spin_lock_irqsave(&i915->mm.obj_lock, flags);

			if (obj->mm.madv != I915_MADV_WILLNEED)
				list = &i915->mm.purge_list;
			else
				list = &i915->mm.shrink_list;
			list_move_tail(&obj->mm.link, list);

			spin_unlock_irqrestore(&i915->mm.obj_lock, flags);
		}
	}

	/* if the object is no longer attached, discard its backing storage */
	if (obj->mm.madv == I915_MADV_DONTNEED &&
	    !i915_gem_object_has_pages(obj))
		i915_gem_object_truncate(obj);

	args->retained = obj->mm.madv != __I915_MADV_PURGED;
	mutex_unlock(&obj->mm.lock);

out:
	i915_gem_object_put(obj);
	return err;
}

static int __intel_context_flush_retire(struct intel_context *ce)
{
	struct intel_timeline *tl;

	tl = intel_context_timeline_lock(ce);
	if (IS_ERR(tl))
		return PTR_ERR(tl);

	intel_context_timeline_unlock(tl);
	return 0;
}

static int __intel_engines_record_defaults(struct intel_gt *gt)
{
	struct i915_request *requests[I915_NUM_ENGINES] = {};
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err = 0;

	/*
	 * As we reset the gpu during very early sanitisation, the current
	 * register state on the GPU should reflect its defaults values.
	 * We load a context onto the hw (with restore-inhibit), then switch
	 * over to a second context to save that default register state. We
	 * can then prime every new context with that state so they all start
	 * from the same default HW values.
	 */

	for_each_engine(engine, gt, id) {
		struct intel_renderstate so;
		struct intel_context *ce;
		struct i915_request *rq;

		err = intel_renderstate_init(&so, engine);
		if (err)
			goto out;

		/* We must be able to switch to something! */
		GEM_BUG_ON(!engine->kernel_context);
		engine->serial++; /* force the kernel context switch */

		ce = intel_context_create(engine->kernel_context->gem_context,
					  engine);
		if (IS_ERR(ce)) {
			err = PTR_ERR(ce);
			goto out;
		}

		rq = intel_context_create_request(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			intel_context_put(ce);
			goto out;
		}

		err = intel_engine_emit_ctx_wa(rq);
		if (err)
			goto err_rq;

		err = intel_renderstate_emit(&so, rq);
		if (err)
			goto err_rq;

err_rq:
		requests[id] = i915_request_get(rq);
		i915_request_add(rq);
		intel_renderstate_fini(&so);
		if (err)
			goto out;
	}

	/* Flush the default context image to memory, and enable powersaving. */
	if (intel_gt_wait_for_idle(gt, I915_GEM_IDLE_TIMEOUT) == -ETIME) {
		err = -EIO;
		goto out;
	}

	for (id = 0; id < ARRAY_SIZE(requests); id++) {
		struct i915_request *rq;
		struct i915_vma *state;
		void *vaddr;

		rq = requests[id];
		if (!rq)
			continue;

		GEM_BUG_ON(!test_bit(CONTEXT_ALLOC_BIT,
				     &rq->hw_context->flags));
		state = rq->hw_context->state;
		if (!state)
			continue;

		/* Serialise with retirement on another CPU */
		err = __intel_context_flush_retire(rq->hw_context);
		if (err)
			goto out;

		/* We want to be able to unbind the state from the GGTT */
		GEM_BUG_ON(intel_context_is_pinned(rq->hw_context));

		/*
		 * As we will hold a reference to the logical state, it will
		 * not be torn down with the context, and importantly the
		 * object will hold onto its vma (making it possible for a
		 * stray GTT write to corrupt our defaults). Unmap the vma
		 * from the GTT to prevent such accidents and reclaim the
		 * space.
		 */
		err = i915_vma_unbind(state);
		if (err)
			goto out;

		i915_gem_object_lock(state->obj);
		err = i915_gem_object_set_to_cpu_domain(state->obj, false);
		i915_gem_object_unlock(state->obj);
		if (err)
			goto out;

		i915_gem_object_set_cache_coherency(state->obj, I915_CACHE_LLC);

		/* Check we can acquire the image of the context state */
		vaddr = i915_gem_object_pin_map(state->obj, I915_MAP_FORCE_WB);
		if (IS_ERR(vaddr)) {
			err = PTR_ERR(vaddr);
			goto out;
		}

		rq->engine->default_state = i915_gem_object_get(state->obj);
		i915_gem_object_unpin_map(state->obj);
	}

out:
	/*
	 * If we have to abandon now, we expect the engines to be idle
	 * and ready to be torn-down. The quickest way we can accomplish
	 * this is by declaring ourselves wedged.
	 */
	if (err)
		intel_gt_set_wedged(gt);

	for (id = 0; id < ARRAY_SIZE(requests); id++) {
		struct intel_context *ce;
		struct i915_request *rq;

		rq = requests[id];
		if (!rq)
			continue;

		ce = rq->hw_context;
		i915_request_put(rq);
		intel_context_put(ce);
	}
	return err;
}

static int intel_engines_verify_workarounds(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err = 0;

	if (!IS_ENABLED(CONFIG_DRM_I915_DEBUG_GEM))
		return 0;

	for_each_engine(engine, gt, id) {
		if (intel_engine_verify_workarounds(engine, "load"))
			err = -EIO;
	}

	return err;
}

int i915_gem_init(struct drm_i915_private *dev_priv)
{
	int ret;

	/* We need to fallback to 4K pages if host doesn't support huge gtt. */
	if (intel_vgpu_active(dev_priv) && !intel_vgpu_has_huge_gtt(dev_priv))
		mkwrite_device_info(dev_priv)->page_sizes =
			I915_GTT_PAGE_SIZE_4K;

	ret = i915_gem_init_userptr(dev_priv);
	if (ret)
		return ret;

	intel_uc_fetch_firmwares(&dev_priv->gt.uc);
	intel_wopcm_init(&dev_priv->wopcm);

	/* This is just a security blanket to placate dragons.
	 * On some systems, we very sporadically observe that the first TLBs
	 * used by the CS may be stale, despite us poking the TLB reset. If
	 * we hold the forcewake during initialisation these problems
	 * just magically go away.
	 */
	intel_uncore_forcewake_get(&dev_priv->uncore, FORCEWAKE_ALL);

	ret = i915_init_ggtt(dev_priv);
	if (ret) {
		GEM_BUG_ON(ret == -EIO);
		goto err_unlock;
	}

	intel_gt_init(&dev_priv->gt);

	ret = intel_engines_setup(&dev_priv->gt);
	if (ret) {
		GEM_BUG_ON(ret == -EIO);
		goto err_unlock;
	}

	ret = i915_gem_init_contexts(dev_priv);
	if (ret) {
		GEM_BUG_ON(ret == -EIO);
		goto err_scratch;
	}

	ret = intel_engines_init(&dev_priv->gt);
	if (ret) {
		GEM_BUG_ON(ret == -EIO);
		goto err_context;
	}

	intel_uc_init(&dev_priv->gt.uc);

	ret = intel_gt_init_hw(&dev_priv->gt);
	if (ret)
		goto err_uc_init;

	/* Only when the HW is re-initialised, can we replay the requests */
	ret = intel_gt_resume(&dev_priv->gt);
	if (ret)
		goto err_init_hw;

	/*
	 * Despite its name intel_init_clock_gating applies both display
	 * clock gating workarounds; GT mmio workarounds and the occasional
	 * GT power context workaround. Worse, sometimes it includes a context
	 * register workaround which we need to apply before we record the
	 * default HW state for all contexts.
	 *
	 * FIXME: break up the workarounds and apply them at the right time!
	 */
	intel_init_clock_gating(dev_priv);

	ret = intel_engines_verify_workarounds(&dev_priv->gt);
	if (ret)
		goto err_gt;

	ret = __intel_engines_record_defaults(&dev_priv->gt);
	if (ret)
		goto err_gt;

	ret = i915_inject_probe_error(dev_priv, -ENODEV);
	if (ret)
		goto err_gt;

	ret = i915_inject_probe_error(dev_priv, -EIO);
	if (ret)
		goto err_gt;

	intel_uncore_forcewake_put(&dev_priv->uncore, FORCEWAKE_ALL);

	return 0;

	/*
	 * Unwinding is complicated by that we want to handle -EIO to mean
	 * disable GPU submission but keep KMS alive. We want to mark the
	 * HW as irrevisibly wedged, but keep enough state around that the
	 * driver doesn't explode during runtime.
	 */
err_gt:
	intel_gt_set_wedged_on_init(&dev_priv->gt);
	i915_gem_suspend(dev_priv);
	i915_gem_suspend_late(dev_priv);

	i915_gem_drain_workqueue(dev_priv);
err_init_hw:
	intel_uc_fini_hw(&dev_priv->gt.uc);
err_uc_init:
	if (ret != -EIO) {
		intel_uc_fini(&dev_priv->gt.uc);
		intel_engines_cleanup(&dev_priv->gt);
	}
err_context:
	if (ret != -EIO)
		i915_gem_driver_release__contexts(dev_priv);
err_scratch:
	intel_gt_driver_release(&dev_priv->gt);
err_unlock:
	intel_uncore_forcewake_put(&dev_priv->uncore, FORCEWAKE_ALL);

	if (ret != -EIO) {
		intel_uc_cleanup_firmwares(&dev_priv->gt.uc);
		i915_gem_cleanup_userptr(dev_priv);
	}

	if (ret == -EIO) {
		/*
		 * Allow engines or uC initialisation to fail by marking the GPU
		 * as wedged. But we only want to do this when the GPU is angry,
		 * for all other failure, such as an allocation failure, bail.
		 */
		if (!intel_gt_is_wedged(&dev_priv->gt)) {
			i915_probe_error(dev_priv,
					 "Failed to initialize GPU, declaring it wedged!\n");
			intel_gt_set_wedged(&dev_priv->gt);
		}

		/* Minimal basic recovery for KMS */
		ret = i915_ggtt_enable_hw(dev_priv);
		i915_gem_restore_gtt_mappings(dev_priv);
		i915_gem_restore_fences(&dev_priv->ggtt);
		intel_init_clock_gating(dev_priv);
	}

	i915_gem_drain_freed_objects(dev_priv);
	return ret;
}

void i915_gem_driver_register(struct drm_i915_private *i915)
{
	i915_gem_driver_register__shrinker(i915);

	intel_engines_driver_register(i915);
}

void i915_gem_driver_unregister(struct drm_i915_private *i915)
{
	i915_gem_driver_unregister__shrinker(i915);
}

void i915_gem_driver_remove(struct drm_i915_private *dev_priv)
{
	intel_wakeref_auto_fini(&dev_priv->ggtt.userfault_wakeref);

	i915_gem_suspend_late(dev_priv);
	intel_gt_driver_remove(&dev_priv->gt);

	/* Flush any outstanding unpin_work. */
	i915_gem_drain_workqueue(dev_priv);

	intel_uc_fini_hw(&dev_priv->gt.uc);
	intel_uc_fini(&dev_priv->gt.uc);

	i915_gem_drain_freed_objects(dev_priv);
}

void i915_gem_driver_release(struct drm_i915_private *dev_priv)
{
	intel_engines_cleanup(&dev_priv->gt);
	i915_gem_driver_release__contexts(dev_priv);
	intel_gt_driver_release(&dev_priv->gt);

	intel_wa_list_free(&dev_priv->gt_wa_list);

	intel_uc_cleanup_firmwares(&dev_priv->gt.uc);
	i915_gem_cleanup_userptr(dev_priv);

	i915_gem_drain_freed_objects(dev_priv);

	WARN_ON(!list_empty(&dev_priv->gem.contexts.list));
}

static void i915_gem_init__mm(struct drm_i915_private *i915)
{
	spin_lock_init(&i915->mm.obj_lock);

	init_llist_head(&i915->mm.free_list);

	INIT_LIST_HEAD(&i915->mm.purge_list);
	INIT_LIST_HEAD(&i915->mm.shrink_list);

	i915_gem_init__objects(i915);
}

void i915_gem_init_early(struct drm_i915_private *dev_priv)
{
	i915_gem_init__mm(dev_priv);

	spin_lock_init(&dev_priv->fb_tracking.lock);
}

void i915_gem_cleanup_early(struct drm_i915_private *dev_priv)
{
	i915_gem_drain_freed_objects(dev_priv);
	GEM_BUG_ON(!llist_empty(&dev_priv->mm.free_list));
	GEM_BUG_ON(atomic_read(&dev_priv->mm.free_count));
	WARN_ON(dev_priv->mm.shrink_count);
}

int i915_gem_freeze(struct drm_i915_private *dev_priv)
{
	/* Discard all purgeable objects, let userspace recover those as
	 * required after resuming.
	 */
	i915_gem_shrink_all(dev_priv);

	return 0;
}

int i915_gem_freeze_late(struct drm_i915_private *i915)
{
	struct drm_i915_gem_object *obj;
	intel_wakeref_t wakeref;

	/*
	 * Called just before we write the hibernation image.
	 *
	 * We need to update the domain tracking to reflect that the CPU
	 * will be accessing all the pages to create and restore from the
	 * hibernation, and so upon restoration those pages will be in the
	 * CPU domain.
	 *
	 * To make sure the hibernation image contains the latest state,
	 * we update that state just before writing out the image.
	 *
	 * To try and reduce the hibernation image, we manually shrink
	 * the objects as well, see i915_gem_freeze()
	 */

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	i915_gem_shrink(i915, -1UL, NULL, ~0);
	i915_gem_drain_freed_objects(i915);

	list_for_each_entry(obj, &i915->mm.shrink_list, mm.link) {
		i915_gem_object_lock(obj);
		WARN_ON(i915_gem_object_set_to_cpu_domain(obj, true));
		i915_gem_object_unlock(obj);
	}

	intel_runtime_pm_put(&i915->runtime_pm, wakeref);

	return 0;
}

void i915_gem_release(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct i915_request *request;

	/* Clean up our request list when the client is going away, so that
	 * later retire_requests won't dereference our soon-to-be-gone
	 * file_priv.
	 */
	spin_lock(&file_priv->mm.lock);
	list_for_each_entry(request, &file_priv->mm.request_list, client_link)
		request->file_priv = NULL;
	spin_unlock(&file_priv->mm.lock);
}

int i915_gem_open(struct drm_i915_private *i915, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv;
	int ret;

	DRM_DEBUG("\n");

	file_priv = kzalloc(sizeof(*file_priv), GFP_KERNEL);
	if (!file_priv)
		return -ENOMEM;

	file->driver_priv = file_priv;
	file_priv->dev_priv = i915;
	file_priv->file = file;

	spin_lock_init(&file_priv->mm.lock);
	INIT_LIST_HEAD(&file_priv->mm.request_list);

	file_priv->bsd_engine = -1;
	file_priv->hang_timestamp = jiffies;

	ret = i915_gem_context_open(i915, file);
	if (ret)
		kfree(file_priv);

	return ret;
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/mock_gem_device.c"
#include "selftests/i915_gem.c"
#endif
