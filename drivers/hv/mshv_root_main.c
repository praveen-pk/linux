// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Microsoft Corporation.
 *
 * The main part of the mshv_root module, providing APIs to create
 * and manage guest partitions.
 *
 * Authors: Microsoft Linux virtualization team
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/cpuhotplug.h>
#include <linux/random.h>
#include <asm/mshyperv.h>
#include <linux/hyperv.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/kexec.h>
#include <linux/page-flags.h>
#include <linux/crash_dump.h>
#include <linux/panic_notifier.h>
#include <linux/vmalloc.h>
#include <linux/hmm.h>

#include <trace/events/mshv.h>

#include "mshv_eventfd.h"
#include "mshv.h"
#include "mshv_root.h"

#define MSHV_MAP_FAULT_IN_PAGES			HPAGE_PMD_NR

MODULE_AUTHOR("Microsoft");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Microsoft Hyper-V root partition VMM interface /dev/mshv");

struct mshv_root mshv_root;

enum hv_scheduler_type hv_scheduler_type;

/* Once we implement the fast extended hypercall ABI they can go away. */
static void * __percpu *root_scheduler_input;
static void * __percpu *root_scheduler_output;

static long mshv_dev_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg);
static int mshv_dev_open(struct inode *inode, struct file *filp);
static int mshv_dev_release(struct inode *inode, struct file *filp);
static int mshv_vp_release(struct inode *inode, struct file *filp);
static long mshv_vp_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg);
static int mshv_partition_release(struct inode *inode, struct file *filp);
static long mshv_partition_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg);
static int mshv_vp_mmap(struct file *file, struct vm_area_struct *vma);
static vm_fault_t mshv_vp_fault(struct vm_fault *vmf);
static int mshv_init_async_handler(struct mshv_partition *partition);
static void mshv_async_hvcall_handler(void *data, u64 *status);
static long mshv_vp_set_explicit_suspend(const struct mshv_vp *vp);
static int mshv_vp_clear_explicit_suspend(struct mshv_vp *vp);
static struct mshv_mem_region
	*mshv_partition_region_by_gfn(struct mshv_partition *pt, u64 gfn);
static int mshv_region_remap_pages(struct mshv_mem_region *region,
				   u32 map_flags, u64 page_offset,
				   u64 page_count);

static const union hv_input_vtl input_vtl_zero;
static const union hv_input_vtl input_vtl_normal = {
	.target_vtl = HV_NORMAL_VTL,
	.use_target_vtl = 1,
};

static const struct vm_operations_struct mshv_vp_vm_ops = {
	.fault = mshv_vp_fault,
};

static const struct file_operations mshv_vp_fops = {
	.owner = THIS_MODULE,
	.release = mshv_vp_release,
	.unlocked_ioctl = mshv_vp_ioctl,
	.llseek = noop_llseek,
	.mmap = mshv_vp_mmap,
};

static const struct file_operations mshv_partition_fops = {
	.owner = THIS_MODULE,
	.release = mshv_partition_release,
	.unlocked_ioctl = mshv_partition_ioctl,
	.llseek = noop_llseek,
};

static const struct file_operations mshv_dev_fops = {
	.owner = THIS_MODULE,
	.open = mshv_dev_open,
	.release = mshv_dev_release,
	.unlocked_ioctl = mshv_dev_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice mshv_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mshv",
	.fops = &mshv_dev_fops,
	.mode = 0600,
};

/*
 * Only allow hypercalls that have a u64 partition id as the first member of
 * the input structure.
 * These are sorted by value.
 */
static u16 mshv_passthru_hvcalls[] = {
	HVCALL_GET_PARTITION_PROPERTY,
	HVCALL_SET_PARTITION_PROPERTY,
	HVCALL_INSTALL_INTERCEPT,
	HVCALL_GET_VP_REGISTERS,
	HVCALL_SET_VP_REGISTERS,
	HVCALL_TRANSLATE_VIRTUAL_ADDRESS,
	HVCALL_CLEAR_VIRTUAL_INTERRUPT,
	HVCALL_REGISTER_INTERCEPT_RESULT,
	HVCALL_ASSERT_VIRTUAL_INTERRUPT,
	HVCALL_GET_GPA_PAGES_ACCESS_STATES,
	HVCALL_SIGNAL_EVENT_DIRECT,
	HVCALL_POST_MESSAGE_DIRECT,
	HVCALL_GET_VP_CPUID_VALUES,
};

static bool mshv_hvcall_is_async(u16 code)
{
	switch (code) {
	case HVCALL_SET_PARTITION_PROPERTY:
		return true;
	default:
		break;
	}
	return false;
}

static inline u64 hv_do_rep_hypercall_ex(u16 code, u16 rep_count,
					 u16 varhead_size, u16 rep_start,
					 void *input, void *output)
{
	u64 control = code;
	u64 status;
	u16 rep_comp;

	control |= (u64)varhead_size << HV_HYPERCALL_VARHEAD_OFFSET;
	control |= (u64)rep_count << HV_HYPERCALL_REP_COMP_OFFSET;
	control |= (u64)rep_start << HV_HYPERCALL_REP_START_OFFSET;

	do {
		status = hv_do_hypercall(control, input, output);
		if (!hv_result_success(status))
			return status;

		rep_comp = hv_repcomp(status);

		control &= ~HV_HYPERCALL_REP_START_MASK;
		control |= (u64)rep_comp << HV_HYPERCALL_REP_START_OFFSET;
	} while (rep_comp < rep_count);

	return status;
}

static int mshv_ioctl_passthru_hvcall(struct mshv_partition *partition,
				      bool partition_locked,
				      void __user *user_args)
{
	u64 status;
	int ret = 0, i;
	bool is_async;
	struct mshv_root_hvcall args;
	struct page *page;
	unsigned int pages_order;
	void *input_pg = NULL;
	void *output_pg = NULL;
	u16 reps_completed;

	if (copy_from_user(&args, user_args, sizeof(args)))
		return -EFAULT;

	if (args.status || !args.in_ptr || args.in_sz < sizeof(u64) ||
	    mshv_field_nonzero(args, rsvd) || args.in_sz > HV_HYP_PAGE_SIZE)
		return -EINVAL;

	if (args.out_ptr && (!args.out_sz || args.out_sz > HV_HYP_PAGE_SIZE))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(mshv_passthru_hvcalls); ++i)
		if (args.code == mshv_passthru_hvcalls[i])
			break;

	if (i >= ARRAY_SIZE(mshv_passthru_hvcalls))
		return -EINVAL;

	is_async = mshv_hvcall_is_async(args.code);
	if (is_async) {
		/* async hypercalls can only be called from partition fd */
		if (!partition_locked)
			return -EINVAL;
		ret = mshv_init_async_handler(partition);
		if (ret)
			return ret;
	}

	pages_order = args.out_ptr ? 1 : 0;
	page = alloc_pages(GFP_KERNEL, pages_order);
	if (!page)
		return -ENOMEM;
	input_pg = page_address(page);

	if (args.out_ptr)
		output_pg = (char *)input_pg + PAGE_SIZE;
	else
		output_pg = NULL;

	if (copy_from_user(input_pg, (void __user *)args.in_ptr,
			   args.in_sz)) {
		ret = -EFAULT;
		goto free_pages_out;
	}

	/*
	 * NOTE: This only works because all the allowed hypercalls' input
	 * structs begin with a u64 partition_id field.
	 */
	*(u64 *)input_pg = partition->pt_id;

	reps_completed = 0;
	do {
		if (args.reps) {
			status = hv_do_rep_hypercall_ex(args.code, args.reps,
							0, reps_completed,
							input_pg, output_pg);
			reps_completed = hv_repcomp(status);
		} else {
			status = hv_do_hypercall(args.code, input_pg, output_pg);
		}

		if (hv_result(status) == HV_STATUS_CALL_PENDING) {
			if (is_async) {
				mshv_async_hvcall_handler(partition, &status);
			} else { /* Paranoia check. This shouldn't happen! */
				ret = -EBADFD;
				goto free_pages_out;
			}
		}

		if (hv_result_success(status))
			break;

		if (hv_result(status) != HV_STATUS_INSUFFICIENT_MEMORY)
			ret = hv_result_to_errno(status);
		else
			ret = hv_call_deposit_pages(NUMA_NO_NODE,
						    partition->pt_id, 1);
	} while (!ret);

	/*
	 * Always return the status and output data regardless of result.
	 * The VMM may need it to determine how to proceed. E.g. the status may
	 * contain the number of reps completed if a rep hypercall partially
	 * succeeded.
	 */
	args.status = hv_result(status);
	args.reps = reps_completed;
	if (copy_to_user(user_args, &args, sizeof(args)))
		ret = -EFAULT;

	if (!ret && output_pg &&
	    copy_to_user((void __user *)args.out_ptr, output_pg, args.out_sz))
		ret = -EFAULT;

free_pages_out:
	free_pages((unsigned long)input_pg, pages_order);

	return ret;
}

static inline bool is_ghcb_mapping_available(void)
{
#if IS_ENABLED(CONFIG_X86_64)
	return ms_hyperv.ext_features & HV_VP_GHCB_ROOT_MAPPING_AVAILABLE;
#else
	return 0;
#endif
}

static int mshv_get_vp_registers(u32 vp_index, u64 partition_id, u16 count,
				 struct hv_register_assoc *registers)
{
	return hv_call_get_vp_registers(vp_index, partition_id,
					count, input_vtl_zero, registers);
}

static int mshv_set_vp_registers(u32 vp_index, u64 partition_id, u16 count,
				 struct hv_register_assoc *registers)
{
	return hv_call_set_vp_registers(vp_index, partition_id,
					count, input_vtl_zero, registers);
}

#ifdef CONFIG_X86_64

#if defined(CONFIG_MMU_NOTIFIER)
/**
 * mshv_region_hmm_fault_and_lock - Handle HMM faults and lock the memory region
 * @region: Pointer to the memory region structure
 * @range: Pointer to the HMM range structure
 *
 * This function performs the following steps:
 * 1. Reads the notifier sequence for the HMM range.
 * 2. Acquires a read lock on the memory map.
 * 3. Handles HMM faults for the specified range.
 * 4. Releases the read lock on the memory map.
 * 5. If successful, locks the memory region mutex.
 * 6. Verifies if the notifier sequence has changed during the operation.
 *    If it has, releases the mutex and returns -EBUSY to match with
 *    hmm_range_fault() return code for repeating.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int mshv_region_hmm_fault_and_lock(struct mshv_mem_region *region,
					  struct hmm_range *range)
{
	int ret;

	range->notifier_seq = mmu_interval_read_begin(range->notifier);
	mmap_read_lock(region->memreg_mni.mm);
	ret = hmm_range_fault(range);
	mmap_read_unlock(region->memreg_mni.mm);
	if (ret)
		return ret;

	mutex_lock(&region->memreg_mutex);

	if (mmu_interval_read_retry(range->notifier, range->notifier_seq)) {
		mutex_unlock(&region->memreg_mutex);
		cond_resched();
		return -EBUSY;
	}

	return 0;
}

/**
 * mshv_region_range_fault - Handle memory range faults for a given region.
 * @region: Pointer to the memory region structure.
 * @page_offset: Offset of the page within the region.
 * @page_count: Number of pages to handle.
 *
 * This function resolves memory faults for a specified range of pages
 * within a memory region. It uses HMM (Heterogeneous Memory Management)
 * to fault in the required pages and updates the region's page array.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mshv_region_range_fault(struct mshv_mem_region *region,
				   u64 page_offset, u64 page_count)
{
	struct hmm_range range = {
		.notifier = &region->memreg_mni,
		.default_flags = HMM_PFN_REQ_FAULT | HMM_PFN_REQ_WRITE,
	};
	unsigned long *pfns;
	int ret;
	u64 i;

	pfns = kmalloc_array(page_count, sizeof(unsigned long), GFP_KERNEL);
	if (!pfns)
		return -ENOMEM;

	range.hmm_pfns = pfns;
	range.start = region->start_uaddr + page_offset * HV_HYP_PAGE_SIZE;
	range.end = range.start + page_count * HV_HYP_PAGE_SIZE;

	do {
		ret = mshv_region_hmm_fault_and_lock(region, &range);
	} while (ret == -EBUSY);

	if (ret)
		goto out;

	for (i = 0; i < page_count; i++)
		region->pages[page_offset + i] = hmm_pfn_to_page(pfns[i]);

	if (PageHuge(region->pages[page_offset]))
		region->flags.large_pages = true;

	ret = mshv_region_remap_pages(region, region->hv_map_flags,
				      page_offset, page_count);

	mutex_unlock(&region->memreg_mutex);
out:
	kfree(pfns);
	return ret;
}
#else /* CONFIG_MMU_NOTIFIER */
static int mshv_region_range_fault(struct mshv_mem_region *region,
				   u64 page_offset, u64 page_count)
{
	return -ENODEV;
}
#endif /* CONFIG_MMU_NOTIFIER */

static bool mshv_region_handle_gfn_fault(struct mshv_mem_region *region, u64 gfn)
{
	u64 page_offset, page_count;
	int ret;

	if (WARN_ON_ONCE(region->flags.range_pinned))
		return false;

	/* Align the page offset to the nearest MSHV_MAP_FAULT_IN_PAGES. */
	page_offset = ALIGN_DOWN(gfn - region->start_gfn,
				 MSHV_MAP_FAULT_IN_PAGES);

	/* Map more pages than requested to reduce the number of faults. */
	page_count = min(region->nr_pages - page_offset,
			 MSHV_MAP_FAULT_IN_PAGES);

	ret = mshv_region_range_fault(region, page_offset, page_count);

	WARN_ONCE(ret,
		  "p%llu: GPA intercept failed: region %#llx-%#llx, gfn %#llx, page_offset %llu, page_count %llu\n",
		  region->partition->pt_id, region->start_uaddr,
		  region->start_uaddr + (region->nr_pages << HV_HYP_PAGE_SHIFT),
		  gfn, page_offset, page_count);

	return !ret;
}

/**
 * mshv_handle_gpa_intercept - Handle GPA (Guest Physical Address) intercepts.
 * @vp: Pointer to the virtual processor structure.
 *
 * This function processes GPA intercepts by identifying the memory region
 * corresponding to the intercepted GPA, aligning the page offset, and
 * mapping the required pages. It ensures that the region is valid and
 * handles faults efficiently by mapping multiple pages at once.
 *
 * Return: true if the intercept was handled successfully, false otherwise.
 */
static bool mshv_handle_gpa_intercept(struct mshv_vp *vp)
{
	struct mshv_partition *p = vp->vp_partition;
	struct mshv_mem_region *region;
	struct hv_x64_memory_intercept_message *msg;
	u64 gfn;

	msg = (struct hv_x64_memory_intercept_message *)
		vp->vp_intercept_msg_page->u.payload;

	gfn = HVPFN_DOWN(msg->guest_physical_address);

	region = mshv_partition_region_by_gfn(p, gfn);
	if (!region)
		return false;

	if (WARN_ON_ONCE(!region->flags.memreg_isram))
		return false;

	if (WARN_ON_ONCE(region->flags.range_pinned))
		return false;

	return mshv_region_handle_gfn_fault(region, gfn);
}

#else	/* CONFIG_X86_64 */

static bool mshv_handle_gpa_intercept(struct mshv_vp *vp) { return false; }

#endif	/* CONFIG_X86_64 */

static long mshv_vp_clear_intercept_suspend(struct mshv_vp *vp)
{
	struct hv_register_assoc susp_reg = {
		.name = HV_REGISTER_INTERCEPT_SUSPEND,
		.value.intercept_suspend.suspended = 0,
	};
	long ret;

	ret = mshv_set_vp_registers(vp->vp_index, vp->vp_partition->pt_id,
				    1, &susp_reg);

	trace_mshv_root_sched_unsuspend_vp(ret, vp->vp_partition->pt_id,
					   vp->vp_index);
	if (ret)
		vp_err(vp, "Failed to unsuspend\n");

	vp->run.flags.intercept_suspended = 0;

	return ret;
}

static long mshv_vp_msg_pending(struct mshv_vp *vp, bool *msg_in_flight)
{
	struct hv_register_assoc susp_reg = {
		.name = HV_REGISTER_INTERCEPT_SUSPEND
	};
	long ret;

	ret = mshv_get_vp_registers(vp->vp_index, vp->vp_partition->pt_id,
				    1, &susp_reg);
	if (ret) {
		vp_err(vp, "Failed to get intercept suspend state\n");
		return ret;
	}

	*msg_in_flight = susp_reg.value.intercept_suspend.suspended;
	return 0;
}

/*
 * This function either runs or resumes a vcpu using core or classic scheduler
 * of the hypervisor. There are three ways to exit out of this function:
 *   1. The vcpu was intercept suspended, meaning hypervisor has a message for
 *	the vmm to process. The vcpu can not run again until intercept cleared.
 *   2. The thread got woken up via Unix signal.
 *   3. An error, eg failed hypercall, occured.
 *
 * Since the vp register page is flushed only in case of intercept suspend or
 * explicit suspend, we do explicit suspend in case of the Unix signal so the
 * register page is valid and vmm can examine it. Resuming then happens by
 * clearing either. Also, a new vp is created in explicit suspend state.
 */
static long mshv_run_vp_with_hyp_scheduler(struct mshv_vp *vp)
{
	long ret;

	/* Resume VP execution, or run it for the first time */
	if (vp->run.flags.intercept_suspended)
		ret = mshv_vp_clear_intercept_suspend(vp);
	else
		ret = mshv_vp_clear_explicit_suspend(vp);
	if (ret)
		return ret;

	ret = wait_event_interruptible(vp->run.vp_suspend_queue,
				       vp->run.kicked_by_hv == 1);
	if (ret) {
		bool message_in_flight;

		/* Got signal, stop the vp and keep hyp from running it again */
		ret = mshv_vp_set_explicit_suspend(vp);
		if (ret)
			return ret;

		/* Check after suspend if there was a message in transit */
		ret = mshv_vp_msg_pending(vp, &message_in_flight);
		if (ret)
			return ret;

		if (!message_in_flight)
			return -EINTR;

		/* Wait for the message in flight. */
		wait_event(vp->run.vp_suspend_queue, vp->run.kicked_by_hv == 1);

		/* cpu in intercept suspend state, we can clear explicit susp */
		ret = mshv_vp_clear_explicit_suspend(vp);
		if (ret)
			return ret;
	}

	vp->run.flags.intercept_suspended = 1;
	vp->run.kicked_by_hv = 0;

	return 0;
}

static int
mshv_vp_dispatch(struct mshv_vp *vp, u32 flags,
		 struct hv_output_dispatch_vp *res)
{
	struct hv_input_dispatch_vp *input;
	struct hv_output_dispatch_vp *output;
	u64 status;

	preempt_disable();
	input = *this_cpu_ptr(root_scheduler_input);
	output = *this_cpu_ptr(root_scheduler_output);

	memset(input, 0, sizeof(*input));
	input->partition_id = vp->vp_partition->pt_id;
	input->vp_index = vp->vp_index;
	input->time_slice = 0; /* Run forever until something happens */
	input->spec_ctrl = 0; /* TODO: set sensible flags */
	input->flags = flags;

	vp->run.flags.root_sched_dispatched = 1;
	status = hv_do_hypercall(HVCALL_DISPATCH_VP, input, output);
	vp->run.flags.root_sched_dispatched = 0;

	trace_mshv_hvcall_dispatch_vp(status, vp->vp_partition->pt_id,
				      vp->vp_index, flags,
				      output->dispatch_state,
				      output->dispatch_event);

	*res = *output;
	preempt_enable();

	if (!hv_result_success(status))
		vp_err(vp, "%s: status %s\n", __func__,
		       hv_result_to_string(status));

	return hv_result_to_errno(status);
}

static long mshv_vp_set_explicit_suspend(const struct mshv_vp *vp)
{
	struct hv_register_assoc susp_reg = {
		.name = HV_REGISTER_EXPLICIT_SUSPEND,
		.value.explicit_suspend.suspended = 1,
	};
	long ret;

	ret = mshv_set_vp_registers(vp->vp_index, vp->vp_partition->pt_id,
				    1, &susp_reg);
	if (ret) {
		vp_err(vp, "Failed to explicitly suspend vCPU\n");
		return ret;
	}

	return 0;
}

static int
mshv_vp_clear_explicit_suspend(struct mshv_vp *vp)
{
	struct hv_register_assoc explicit_suspend = {
		.name = HV_REGISTER_EXPLICIT_SUSPEND,
		.value.explicit_suspend.suspended = 0,
	};
	int ret;

	ret = mshv_set_vp_registers(vp->vp_index, vp->vp_partition->pt_id,
				    1, &explicit_suspend);

	trace_mshv_root_sched_unsuspend_vp(ret, vp->vp_partition->pt_id,
					   vp->vp_index);

	if (ret)
		vp_err(vp, "Failed to unsuspend\n");

	return ret;
}

#if IS_ENABLED(CONFIG_X86_64)
static u64 mshv_vp_interrupt_pending(struct mshv_vp *vp)
{
	if (!vp->vp_register_page)
		return 0;
	return vp->vp_register_page->interrupt_vectors.as_uint64;
}
#else
static u64 mshv_vp_interrupt_pending(struct mshv_vp *vp)
{
	return 0;
}
#endif

static bool mshv_vp_dispatch_thread_blocked(struct mshv_vp *vp)
{
	struct hv_stats_page **stats = vp->vp_stats_pages;
	u64 *self_vp_cntrs = stats[HV_STATS_AREA_SELF]->vp_cntrs;
	u64 *parent_vp_cntrs = stats[HV_STATS_AREA_PARENT]->vp_cntrs;

	if (self_vp_cntrs[VpRootDispatchThreadBlocked])
		return self_vp_cntrs[VpRootDispatchThreadBlocked];
	return parent_vp_cntrs[VpRootDispatchThreadBlocked];
}

/*
 * root scheduler only: when vp goes into blocked state, it just waits here.
 * Then 3 ways to wake up:
 *    1. kicked by hypervisor
 *    2. interrupt injection by vmm via irqfd
 *    3. unix signal
 */
static int mshv_vp_wait_for_event(struct mshv_vp *vp)
{
	int ret;

	ret = wait_event_interruptible(vp->run.vp_suspend_queue,
				       (vp->run.kicked_by_hv == 1 &&
					!mshv_vp_dispatch_thread_blocked(vp)) ||
				       mshv_vp_interrupt_pending(vp));
	if (ret)
		return -EINTR;

	vp->run.flags.root_sched_blocked = 0;
	vp->run.kicked_by_hv = 0;

	return 0;
}

/*
 * Before going into guest mode, check if this task has pending events, and
 * process them.
 */
static int mshv_chk_process_host_events(struct mshv_vp *vp)
{
	const ulong work_flags = _TIF_NOTIFY_SIGNAL | _TIF_SIGPENDING |
				 _TIF_NEED_RESCHED  | _TIF_NOTIFY_RESUME;
	ulong th_flags;

	th_flags = read_thread_flags();
	while (th_flags & work_flags) {
		int ret;

		/* nb: following will call schedule */
		ret = mshv_do_pre_guest_mode_work(th_flags);

		trace_mshv_root_sched_handle_work(ret, vp->vp_partition->pt_id,
						  vp->vp_index, th_flags);

		if (ret)
			return ret;

		th_flags = read_thread_flags();
	}

	return 0;
}

/* Must be called with interrupts enabled */
static long mshv_run_vp_with_root_scheduler(struct mshv_vp *vp)
{
	long ret;

	if (!vp->run.flags.intercept_suspended) {
		ret = mshv_vp_clear_explicit_suspend(vp);
		if (ret)
			return ret;
	}

	do {
		u32 flags = 0;
		struct hv_output_dispatch_vp output;

		/*
		 * If the dispatch state of this vp is blocked, wait for the
		 * hypervisor to clear it before dispatching the vp.
		 */
		if (vp->run.flags.root_sched_blocked) {
			ret = mshv_vp_wait_for_event(vp);
			if (ret)
				break;
		}

		ret = mshv_chk_process_host_events(vp);
		if (ret)
			break;

		if (vp->run.flags.intercept_suspended)
			flags |= HV_DISPATCH_VP_FLAG_CLEAR_INTERCEPT_SUSPEND;

		if (mshv_vp_interrupt_pending(vp))
			flags |= HV_DISPATCH_VP_FLAG_SCAN_INTERRUPT_INJECTION;

		ret = mshv_vp_dispatch(vp, flags, &output);
		if (ret)
			break;

		vp->run.flags.intercept_suspended = 0;

		if (output.dispatch_state == HV_VP_DISPATCH_STATE_BLOCKED) {
			if (output.dispatch_event ==
						HV_VP_DISPATCH_EVENT_SUSPEND) {
				/*
				 * TODO: remove the warning once VP canceling
				 *	 is supported
				 */
				WARN_ONCE(atomic64_read(&vp->run.vp_signaled_count),
					  "%s: vp#%d: unexpected explicit suspend\n",
					  __func__, vp->vp_index);
				/*
				 * Need to clear explicit suspend before
				 * dispatching.
				 * Since VP cancelling is not yet supported,
				 * simply clear it here.
				 */
				ret = mshv_vp_clear_explicit_suspend(vp);
				if (ret)
					break;

				ret = mshv_vp_wait_for_event(vp);
				if (ret)
					break;
			} else {
				vp->run.flags.root_sched_blocked = 1;
			}
		} else {
			/* HV_VP_DISPATCH_STATE_READY */
			if (output.dispatch_event ==
						HV_VP_DISPATCH_EVENT_INTERCEPT)
				vp->run.flags.intercept_suspended = 1;
		}
	} while (!vp->run.flags.intercept_suspended);

	if (!vp->run.flags.intercept_suspended) {
		long rc = mshv_vp_set_explicit_suspend(vp);

		/* don't override ret, it could be status from signal */
		if (rc)
			ret = rc;
	}

	return ret;
}

static_assert(sizeof(struct hv_message) <= MSHV_RUN_VP_BUF_SZ,
	      "sizeof(struct hv_message) must not exceed MSHV_RUN_VP_BUF_SZ");

static bool mshv_vp_handle_intercept(struct mshv_vp *vp)
{
	switch (vp->vp_intercept_msg_page->header.message_type) {
	case HVMSG_GPA_INTERCEPT:
		return mshv_handle_gpa_intercept(vp);
	}
	return false;
}

static long mshv_vp_ioctl_run_vp(struct mshv_vp *vp, void __user *ret_msg)
{
	char *schednm;
	long rc;

	schednm = hv_scheduler_type == HV_SCHEDULER_TYPE_ROOT ? "root" : "hv";
	trace_mshv_run_vp_entry(vp->vp_partition->pt_id, vp->vp_index, schednm);

	do {
		if (hv_scheduler_type == HV_SCHEDULER_TYPE_ROOT)
			rc = mshv_run_vp_with_root_scheduler(vp);
		else
			rc = mshv_run_vp_with_hyp_scheduler(vp);
	} while (rc == 0 && mshv_vp_handle_intercept(vp));

	if (rc)
		return rc;

	trace_mshv_run_vp_exit(rc, vp->vp_partition->pt_id, vp->vp_index,
			       vp->vp_intercept_msg_page->header.message_type);

	if (copy_to_user(ret_msg, vp->vp_intercept_msg_page,
			 sizeof(struct hv_message)))
		rc = -EFAULT;

	return rc;
}

static int
mshv_vp_ioctl_get_set_state_pfn(struct mshv_vp *vp,
				struct hv_vp_state_data state_data,
				unsigned long user_pfn, size_t page_count,
				bool is_set)
{
	int completed, ret = 0;
	unsigned long check;
	struct page **pages;

	if (page_count > INT_MAX)
		return -EINVAL;
	/*
	 * Check the arithmetic for wraparound/overflow.
	 * The last page address in the buffer is:
	 * (user_pfn + (page_count - 1)) * PAGE_SIZE
	 */
	if (check_add_overflow(user_pfn, (page_count - 1), &check))
		return -EOVERFLOW;
	if (check_mul_overflow(check, PAGE_SIZE, &check))
		return -EOVERFLOW;

	/* Pin user pages so hypervisor can copy directly to them */
	pages = kcalloc(page_count, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (completed = 0; completed < page_count; completed += ret) {
		unsigned long user_addr = (user_pfn + completed) * PAGE_SIZE;
		int remaining = page_count - completed;

		ret = pin_user_pages_fast(user_addr, remaining, FOLL_WRITE,
					  &pages[completed]);
		if (ret < 0) {
			vp_err(vp, "%s: Failed to pin user pages error %i\n",
			       __func__, ret);
			goto unpin_pages;
		}
	}

	if (is_set)
		ret = hv_call_set_vp_state(vp->vp_index,
					   vp->vp_partition->pt_id,
					   state_data, page_count, pages,
					   0, NULL);
	else
		ret = hv_call_get_vp_state(vp->vp_index,
					   vp->vp_partition->pt_id,
					   state_data, page_count, pages,
					   NULL);

unpin_pages:
	unpin_user_pages(pages, completed);
	kfree(pages);
	return ret;
}

static long
mshv_vp_ioctl_get_set_state(struct mshv_vp *vp,
			    struct mshv_get_set_vp_state __user *user_args,
			    bool is_set)
{
	struct mshv_get_set_vp_state args;
	long ret = 0;
	union hv_output_get_vp_state vp_state;
	u32 data_sz;
	struct hv_vp_state_data state_data = {};

	if (copy_from_user(&args, user_args, sizeof(args)))
		return -EFAULT;

	if (args.type >= MSHV_VP_STATE_COUNT || mshv_field_nonzero(args, rsvd) ||
	    !args.buf_sz || !PAGE_ALIGNED(args.buf_sz) ||
	    !PAGE_ALIGNED(args.buf_ptr))
		return -EINVAL;

	if (!access_ok((void __user *)args.buf_ptr, args.buf_sz))
		return -EFAULT;

	switch (args.type) {
	case MSHV_VP_STATE_LAPIC:
		state_data.type = HV_GET_SET_VP_STATE_LAPIC_STATE;
		data_sz = HV_HYP_PAGE_SIZE;
		break;
	case MSHV_VP_STATE_XSAVE:
	{
		u64 data_sz_64;

		ret = hv_call_get_partition_property(vp->vp_partition->pt_id,
						     HV_PARTITION_PROPERTY_XSAVE_STATES,
						     &state_data.xsave.states.as_uint64);
		if (ret)
			return ret;

		ret = hv_call_get_partition_property(vp->vp_partition->pt_id,
						     HV_PARTITION_PROPERTY_MAX_XSAVE_DATA_SIZE,
						     &data_sz_64);
		if (ret)
			return ret;

		data_sz = (u32)data_sz_64;
		state_data.xsave.flags = 0;
		/* Always request legacy states */
		state_data.xsave.states.legacy_x87 = 1;
		state_data.xsave.states.legacy_sse = 1;
		state_data.type = HV_GET_SET_VP_STATE_XSAVE;
		break;
	}
	case MSHV_VP_STATE_SIMP:
		state_data.type = HV_GET_SET_VP_STATE_SIM_PAGE;
		data_sz = HV_HYP_PAGE_SIZE;
		break;
	case MSHV_VP_STATE_SIEFP:
		state_data.type = HV_GET_SET_VP_STATE_SIEF_PAGE;
		data_sz = HV_HYP_PAGE_SIZE;
		break;
	case MSHV_VP_STATE_SYNTHETIC_TIMERS:
		state_data.type = HV_GET_SET_VP_STATE_SYNTHETIC_TIMERS;
		data_sz = sizeof(vp_state.synthetic_timers_state);
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user(&user_args->buf_sz, &data_sz, sizeof(user_args->buf_sz)))
		return -EFAULT;

	if (data_sz > args.buf_sz)
		return -EINVAL;

	/* If the data is transmitted via pfns, delegate to helper */
	if (state_data.type & HV_GET_SET_VP_STATE_TYPE_PFN) {
		unsigned long user_pfn = PFN_DOWN(args.buf_ptr);
		size_t page_count = PFN_DOWN(args.buf_sz);

		return mshv_vp_ioctl_get_set_state_pfn(vp, state_data, user_pfn,
						       page_count, is_set);
	}

	/* Paranoia check - this shouldn't happen! */
	if (data_sz > sizeof(vp_state)) {
		vp_err(vp, "Invalid vp state data size!\n");
		return -EINVAL;
	}

	if (is_set) {
		if (copy_from_user(&vp_state, (__user void *)args.buf_ptr, data_sz))
			return -EFAULT;

		return hv_call_set_vp_state(vp->vp_index,
					    vp->vp_partition->pt_id,
					    state_data, 0, NULL,
					    sizeof(vp_state), (u8 *)&vp_state);
	}

	ret = hv_call_get_vp_state(vp->vp_index, vp->vp_partition->pt_id,
				   state_data, 0, NULL, &vp_state);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)args.buf_ptr, &vp_state, data_sz))
		return -EFAULT;

	return 0;
}

static long
mshv_vp_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	struct mshv_vp *vp = filp->private_data;
	long r = -ENOTTY;

	if (mutex_lock_killable(&vp->vp_mutex))
		return -EINTR;

	switch (ioctl) {
	case MSHV_RUN_VP:
		r = mshv_vp_ioctl_run_vp(vp, (void __user *)arg);
		break;
	case MSHV_GET_VP_STATE:
		r = mshv_vp_ioctl_get_set_state(vp, (void __user *)arg, false);
		break;
	case MSHV_SET_VP_STATE:
		r = mshv_vp_ioctl_get_set_state(vp, (void __user *)arg, true);
		break;
	case MSHV_ROOT_HVCALL:
		r = mshv_ioctl_passthru_hvcall(vp->vp_partition, false,
					       (void __user *)arg);
		break;
	default:
		vp_warn(vp, "Invalid ioctl: %#x\n", ioctl);
		break;
	}
	mutex_unlock(&vp->vp_mutex);

	return r;
}

static vm_fault_t mshv_vp_fault(struct vm_fault *vmf)
{
	struct mshv_vp *vp = vmf->vma->vm_file->private_data;

	switch (vmf->vma->vm_pgoff) {
	case MSHV_VP_MMAP_OFFSET_REGISTERS:
		vmf->page = virt_to_page(vp->vp_register_page);
		break;
	case MSHV_VP_MMAP_OFFSET_INTERCEPT_MESSAGE:
		vmf->page = virt_to_page(vp->vp_intercept_msg_page);
		break;
	case MSHV_VP_MMAP_OFFSET_GHCB:
		vmf->page = virt_to_page(vp->vp_ghcb_page);
		break;
	default:
		return VM_FAULT_SIGBUS;
	}

	get_page(vmf->page);

	return 0;
}

static int mshv_vp_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mshv_vp *vp = file->private_data;

	switch (vma->vm_pgoff) {
	case MSHV_VP_MMAP_OFFSET_REGISTERS:
		if (!vp->vp_register_page)
			return -ENODEV;
		break;
	case MSHV_VP_MMAP_OFFSET_INTERCEPT_MESSAGE:
		if (!vp->vp_intercept_msg_page)
			return -ENODEV;
		break;
	case MSHV_VP_MMAP_OFFSET_GHCB:
		if (!vp->vp_ghcb_page)
			return -ENODEV;
		break;
	default:
		return -EINVAL;
	}

	vma->vm_ops = &mshv_vp_vm_ops;
	return 0;
}

static int
mshv_vp_release(struct inode *inode, struct file *filp)
{
	struct mshv_vp *vp = filp->private_data;

	trace_mshv_vp_release(vp->vp_partition->pt_id, vp->vp_index);

	/* Rest of VP cleanup happens in destroy_partition() */
	mshv_partition_put(vp->vp_partition);
	return 0;
}

static void mshv_vp_stats_unmap(u64 partition_id, u32 vp_index,
				void *stats_pages[])
{
	union hv_stats_object_identity identity = {
		.vp.partition_id = partition_id,
		.vp.vp_index = vp_index,
	};

	identity.vp.stats_area_type = HV_STATS_AREA_SELF;
	hv_unmap_stats_page(HV_STATS_OBJECT_VP, NULL, &identity);

	if (stats_pages[HV_STATS_AREA_PARENT] == stats_pages[HV_STATS_AREA_SELF])
		return;

	identity.vp.stats_area_type = HV_STATS_AREA_PARENT;
	hv_unmap_stats_page(HV_STATS_OBJECT_VP, NULL, &identity);
}

static int mshv_vp_stats_map(u64 partition_id, u32 vp_index,
			     void *stats_pages[])
{
	union hv_stats_object_identity identity = {
		.vp.partition_id = partition_id,
		.vp.vp_index = vp_index,
	};
	int err;

	identity.vp.stats_area_type = HV_STATS_AREA_SELF;
	err = hv_map_stats_page(HV_STATS_OBJECT_VP, &identity,
				&stats_pages[HV_STATS_AREA_SELF]);
	if (err)
		return err;

	identity.vp.stats_area_type = HV_STATS_AREA_PARENT;
	err = hv_map_stats_page(HV_STATS_OBJECT_VP, &identity,
				&stats_pages[HV_STATS_AREA_PARENT]);
	if (err)
		goto unmap_self;
	if (!stats_pages[HV_STATS_AREA_PARENT])
		stats_pages[HV_STATS_AREA_PARENT] = stats_pages[HV_STATS_AREA_SELF];

	return 0;

unmap_self:
	identity.vp.stats_area_type = HV_STATS_AREA_SELF;
	hv_unmap_stats_page(HV_STATS_OBJECT_VP, NULL, &identity);
	return err;
}

static long
mshv_partition_ioctl_create_vp(struct mshv_partition *partition,
			       void __user *arg)
{
	struct mshv_create_vp args;
	struct mshv_vp *vp;
	struct page *intercept_message_page = NULL, *register_page = NULL, *ghcb_page = NULL;
	void *stats_pages[2];
	long ret;

	if (copy_from_user(&args, arg, sizeof(args)))
		return -EFAULT;

	if (args.vp_index >= MSHV_MAX_VPS)
		return -EINVAL;

	if (partition->pt_vp_array[args.vp_index])
		return -EEXIST;

	ret = hv_call_create_vp(NUMA_NO_NODE, partition->pt_id, args.vp_index,
				0 /* Only valid for root partition VPs */);
	if (ret)
		return ret;

	ret = hv_map_vp_state_page(partition->pt_id, args.vp_index,
				   HV_VP_STATE_PAGE_INTERCEPT_MESSAGE,
				   input_vtl_zero, &intercept_message_page);
	if (ret)
		goto destroy_vp;

	if (!mshv_partition_encrypted(partition)) {
		ret = hv_map_vp_state_page(partition->pt_id, args.vp_index,
					   HV_VP_STATE_PAGE_REGISTERS,
					   input_vtl_zero, &register_page);
		if (ret)
			goto unmap_intercept_message_page;
	}

	if (mshv_partition_encrypted(partition) &&
	    is_ghcb_mapping_available()) {
		ret = hv_map_vp_state_page(partition->pt_id, args.vp_index,
					   HV_VP_STATE_PAGE_GHCB,
					   input_vtl_normal, &ghcb_page);
		if (ret)
			goto unmap_register_page;
	}

	/* L1VH partitions are not allowed to map the stats page. Yet. */
	if (!hv_l1vh_partition()) {
		ret = mshv_vp_stats_map(partition->pt_id, args.vp_index,
					stats_pages);
		if (ret)
			goto unmap_ghcb_page;
	}

	vp = kzalloc(sizeof(*vp), GFP_KERNEL);
	if (!vp)
		goto unmap_stats_pages;

	vp->vp_partition = mshv_partition_get(partition);
	if (!vp->vp_partition) {
		ret = -EBADF;
		goto free_vp;
	}

	mutex_init(&vp->vp_mutex);
	init_waitqueue_head(&vp->run.vp_suspend_queue);
	atomic64_set(&vp->run.vp_signaled_count, 0);

	vp->vp_index = args.vp_index;
	vp->vp_intercept_msg_page = page_to_virt(intercept_message_page);
	if (!mshv_partition_encrypted(partition))
		vp->vp_register_page = page_to_virt(register_page);

	if (mshv_partition_encrypted(partition) && is_ghcb_mapping_available())
		vp->vp_ghcb_page = page_to_virt(ghcb_page);

	if (hv_parent_partition())
		memcpy(vp->vp_stats_pages, stats_pages, sizeof(stats_pages));

	ret = mshv_debugfs_vp_create(vp);
	if (ret)
		goto put_partition;

	/*
	 * Keep anon_inode_getfd last: it installs fd in the file struct and
	 * thus makes the state accessible in user space.
	 */
	ret = anon_inode_getfd("mshv_vp", &mshv_vp_fops, vp,
			       O_RDWR | O_CLOEXEC);
	if (ret < 0)
		goto remove_debugfs_vp;

	/* already exclusive with the partition mutex for all ioctls */
	partition->pt_vp_count++;
	partition->pt_vp_array[args.vp_index] = vp;

	trace_mshv_create_vp(ret, partition->pt_id, vp->vp_index, ret);

	return ret;

remove_debugfs_vp:
	mshv_debugfs_vp_remove(vp);
put_partition:
	mshv_partition_put(partition);
free_vp:
	kfree(vp);
unmap_stats_pages:
	if (!hv_l1vh_partition())
		mshv_vp_stats_unmap(partition->pt_id, args.vp_index, stats_pages);
unmap_ghcb_page:
	if (mshv_partition_encrypted(partition) && is_ghcb_mapping_available())
		hv_unmap_vp_state_page(partition->pt_id, args.vp_index,
				       HV_VP_STATE_PAGE_GHCB, vp->vp_ghcb_page,
				       input_vtl_normal);
unmap_register_page:
	if (!mshv_partition_encrypted(partition))
		hv_unmap_vp_state_page(partition->pt_id, args.vp_index,
				       HV_VP_STATE_PAGE_REGISTERS,
				       vp->vp_register_page, input_vtl_zero);
unmap_intercept_message_page:
	hv_unmap_vp_state_page(partition->pt_id, args.vp_index,
			       HV_VP_STATE_PAGE_INTERCEPT_MESSAGE,
			       vp->vp_intercept_msg_page, input_vtl_zero);
destroy_vp:
	hv_call_delete_vp(partition->pt_id, args.vp_index);
	trace_mshv_create_vp(ret, partition->pt_id, args.vp_index, -1);

	return ret;
}

static int mshv_init_async_handler(struct mshv_partition *partition)
{
	if (completion_done(&partition->async_hypercall)) {
		pt_err(partition,
		       "Cannot issue async hypercall while another one in progress!\n");
		return -EPERM;
	}

	reinit_completion(&partition->async_hypercall);
	return 0;
}

static void mshv_async_hvcall_handler(void *data, u64 *status)
{
	struct mshv_partition *partition = data;

	wait_for_completion(&partition->async_hypercall);
	pt_dbg(partition, "Async hypercall completed!\n");

	*status = partition->async_hypercall_status;
}

static int
mshv_partition_region_share(struct mshv_mem_region *region)
{
	u32 flags = HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_SHARED;

	if (region->flags.large_pages)
		flags |= HV_MODIFY_SPA_PAGE_HOST_ACCESS_LARGE_PAGE;

	return hv_call_modify_spa_host_access(region->partition->pt_id,
			region->pages, region->nr_pages,
			HV_MAP_GPA_READABLE | HV_MAP_GPA_WRITABLE,
			flags, true);
}

static int
mshv_partition_region_unshare(struct mshv_mem_region *region)
{
	u32 flags = HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_EXCLUSIVE;

	if (region->flags.large_pages)
		flags |= HV_MODIFY_SPA_PAGE_HOST_ACCESS_LARGE_PAGE;

	return hv_call_modify_spa_host_access(region->partition->pt_id,
			region->pages, region->nr_pages,
			0,
			flags, false);
}

static int
mshv_region_remap_pages(struct mshv_mem_region *region, u32 map_flags,
			u64 page_offset, u64 page_count)
{
	if (page_offset + page_count > region->nr_pages)
		return -EINVAL;

	if (region->flags.large_pages)
		map_flags |= HV_MAP_GPA_LARGE_PAGE;

	/* ask the hypervisor to map guest ram */
	return hv_call_map_gpa_pages(region->partition->pt_id,
				     region->start_gfn + page_offset,
				     page_count, map_flags,
				     region->pages + page_offset);
}

static int
mshv_region_map(struct mshv_mem_region *region)
{
	u32 map_flags = region->hv_map_flags;

	return mshv_region_remap_pages(region, map_flags,
				       0, region->nr_pages);
}

static void
mshv_region_evict_pages(struct mshv_mem_region *region,
			u64 page_offset, u64 page_count)
{
	if (region->flags.range_pinned)
		unpin_user_pages(region->pages + page_offset, page_count);

	memset(region->pages + page_offset, 0,
	       page_count * sizeof(struct page *));
}

static void
mshv_region_evict(struct mshv_mem_region *region)
{
	mshv_region_evict_pages(region, 0, region->nr_pages);
}

static int
mshv_region_populate_pages(struct mshv_mem_region *region,
			   u64 page_offset, u64 page_count)
{
	u64 done_count, nr_pages;
	struct page **pages;
	__u64 userspace_addr;
	int ret;

	if (page_offset + page_count > region->nr_pages)
		return -EINVAL;

	for (done_count = 0; done_count < page_count; done_count += ret) {
		pages = region->pages + page_offset + done_count;
		userspace_addr = region->start_uaddr +
				(page_offset + done_count) *
				HV_HYP_PAGE_SIZE;
		nr_pages = min(page_count - done_count,
			       MSHV_PIN_PAGES_BATCH_SIZE);

		/*
		 * Pinning assuming 4k pages works for large pages too.
		 * All page structs within the large page are returned.
		 *
		 * Pin requests are batched because pin_user_pages_fast
		 * with the FOLL_LONGTERM flag does a large temporary
		 * allocation of contiguous memory.
		 */
		ret = pin_user_pages_fast(userspace_addr, nr_pages,
					  FOLL_WRITE | FOLL_LONGTERM,
					  pages);
		if (ret < 0)
			goto release_pages;
	}

	if (PageHuge(region->pages[page_offset]))
		region->flags.large_pages = true;

	return 0;

release_pages:
	mshv_region_evict_pages(region, page_offset, done_count);
	return ret;
}

static int
mshv_region_populate(struct mshv_mem_region *region)
{
	return mshv_region_populate_pages(region, 0, region->nr_pages);
}

static struct mshv_mem_region *
mshv_partition_region_by_gfn(struct mshv_partition *partition, u64 gfn)
{
	struct mshv_mem_region *region;

	hlist_for_each_entry(region, &partition->pt_mem_regions, hnode) {
		if (gfn >= region->start_gfn &&
		    gfn < region->start_gfn + region->nr_pages)
			return region;
	}

	return NULL;
}

static struct mshv_mem_region *
mshv_partition_region_by_uaddr(struct mshv_partition *partition, u64 uaddr)
{
	struct mshv_mem_region *region;

	hlist_for_each_entry(region, &partition->pt_mem_regions, hnode) {
		if (uaddr >= region->start_uaddr &&
		    uaddr < region->start_uaddr +
			    (region->nr_pages << HV_HYP_PAGE_SHIFT))
			return region;
	}

	return NULL;
}

#if defined(CONFIG_MMU_NOTIFIER)
static void mshv_region_movable_fini(struct mshv_mem_region *region)
{
	if (region->flags.range_pinned)
		return;

	mmu_interval_notifier_remove(&region->memreg_mni);
}

/**
 * mshv_region_invalidate - Invalidate a memory region
 * @mni: Pointer to the mmu_interval_notifier structure
 * @range: Pointer to the mmu_notifier_range structure
 * @cur_seq: Current sequence number for the interval notifier
 *
 * This function invalidates a memory region by remapping its pages with
 * no access permissions. It locks the region's mutex to ensure thread safety
 * and updates the sequence number for the interval notifier. If the range
 * is blockable, it uses a blocking lock; otherwise, it attempts a non-blocking
 * lock and returns false if unsuccessful.
 *
 * Return: true if the region was successfully invalidated, false otherwise.
 */
static bool mshv_region_invalidate(struct mmu_interval_notifier *mni,
				   const struct mmu_notifier_range *range,
				   unsigned long cur_seq)
{
	struct mshv_mem_region *region = container_of(mni,
						struct mshv_mem_region,
						memreg_mni);
	u64 page_offset, page_count;
	unsigned long mstart, mend;
	int ret;

	if (!mmget_not_zero(mni->mm))
		return true;

	if (mmu_notifier_range_blockable(range)) {
		mutex_lock(&region->memreg_mutex);
	} else if (!mutex_trylock(&region->memreg_mutex)) {
		mmput(mni->mm);
		return false;
	}

	mmu_interval_set_seq(mni, cur_seq);

	mstart = max(range->start, region->start_uaddr);
	mend = min(range->end, region->start_uaddr +
		   (region->nr_pages << HV_HYP_PAGE_SHIFT));

	page_offset = HVPFN_DOWN(mstart - region->start_uaddr);
	page_count = HVPFN_DOWN(mend - mstart);

	ret = mshv_region_remap_pages(region, HV_MAP_GPA_NO_ACCESS,
				      page_offset, page_count);

	WARN_ONCE(ret,
		  "Failed to invalidate region %#llx-%#llx (range %#lx-%#lx, event: %u, pages %#llx-%#llx, mm: %#llx): %d\n",
		  region->start_uaddr,
		  region->start_uaddr + (region->nr_pages << HV_HYP_PAGE_SHIFT),
		  range->start, range->end, range->event,
		  page_offset, page_offset + page_count - 1, (u64)range->mm, ret);

	memset(region->pages + page_offset, 0,
	       page_count * sizeof(struct page *));

	mutex_unlock(&region->memreg_mutex);
	mmput(mni->mm);

	return true;
}

static const struct mmu_interval_notifier_ops mshv_region_mni_ops = {
	.invalidate = mshv_region_invalidate,
};

static bool mshv_region_movable_init(struct mshv_mem_region *region)
{
	int ret;

	ret = mmu_interval_notifier_insert(&region->memreg_mni, current->mm,
					   region->start_uaddr,
					   region->nr_pages << HV_HYP_PAGE_SHIFT,
					   &mshv_region_mni_ops);
	if (ret)
		return false;

	mutex_init(&region->memreg_mutex);

	return true;
}
#else
static inline void mshv_region_movable_fini(struct mshv_mem_region *region)
{
}

static inline bool mshv_region_movable_init(struct mshv_mem_region *region)
{
	return false;
}
#endif

/*
 * NB: caller checks and makes sure mem->size is page aligned
 * Returns: 0 with regionpp updated on success, or -errno
 */
static int mshv_partition_create_region(struct mshv_partition *partition,
					struct mshv_user_mem_region *mem,
					struct mshv_mem_region **regionpp,
					bool is_mmio)
{
	struct mshv_mem_region *region;
	u64 nr_pages = HVPFN_DOWN(mem->size);

	/* Reject overlapping regions */
	if (mshv_partition_region_by_gfn(partition, mem->guest_pfn) ||
	    mshv_partition_region_by_gfn(partition, mem->guest_pfn + nr_pages - 1) ||
	    mshv_partition_region_by_uaddr(partition, mem->userspace_addr) ||
	    mshv_partition_region_by_uaddr(partition, mem->userspace_addr + mem->size - 1))
		return -EEXIST;

	region = vzalloc(sizeof(*region) + sizeof(struct page *) * nr_pages);
	if (!region)
		return -ENOMEM;

	region->nr_pages = nr_pages;
	region->start_gfn = mem->guest_pfn;
	region->start_uaddr = mem->userspace_addr;
	region->hv_map_flags = HV_MAP_GPA_READABLE | HV_MAP_GPA_ADJUSTABLE;
	if (mem->flags & BIT(MSHV_SET_MEM_BIT_WRITABLE))
		region->hv_map_flags |= HV_MAP_GPA_WRITABLE;
	if (mem->flags & BIT(MSHV_SET_MEM_BIT_EXECUTABLE))
		region->hv_map_flags |= HV_MAP_GPA_EXECUTABLE;

	/* Note: large_pages flag populated when pages are allocated. */
	if (!is_mmio) {
		region->flags.memreg_isram = true;

		if (mshv_partition_encrypted(partition) ||
		    !mshv_region_movable_init(region))
			region->flags.range_pinned = true;
	}

	region->partition = partition;

	*regionpp = region;

	return 0;
}

/**
 * mshv_handle_pinned_region - Handle pinned memory regions
 * @region: Pointer to the memory region structure
 *
 * This function processes memory regions that are explicitly marked as pinned.
 * Pinned regions are preallocated, mapped upfront, and do not rely on fault-based
 * population. The function ensures the region is properly populated, handles
 * encryption requirements for SNP partitions if applicable, maps the region,
 * and performs necessary sharing or eviction operations based on the mapping
 * result.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int mshv_handle_pinned_region(struct mshv_mem_region *region)
{
	struct mshv_partition *partition = region->partition;
	int ret;

	ret = mshv_region_populate(region);
	if (ret) {
		pt_err(partition, "Failed to populate memory region: %d\n",
		       ret);
		goto err_out;
	}

	/*
	 * For an SNP partition it is a requirement that for every memory region
	 * that we are going to map for this partition we should make sure that
	 * host access to that region is released. This is ensured by doing an
	 * additional hypercall which will update the SLAT to release host
	 * access to guest memory regions.
	 */
	if (mshv_partition_encrypted(partition)) {
		ret = mshv_partition_region_unshare(region);
		if (ret) {
			pt_err(partition,
			       "Failed to unshare memory region (guest_pfn: %llu): %d\n",
			       region->start_gfn, ret);
			goto evict_region;
		}
	}

	ret = mshv_region_map(region);
	if (ret && mshv_partition_encrypted(partition)) {
		int shrc;

		shrc = mshv_partition_region_share(region);
		if (!shrc)
			goto evict_region;

		pt_err(partition,
		       "Failed to share memory region (guest_pfn: %llu): %d\n",
		       region->start_gfn, shrc);
		/*
		 * Don't unpin if marking shared failed because pages are no
		 * longer mapped in the host, ie root, anymore.
		 */
		goto err_out;
	}

	return 0;

evict_region:
	mshv_region_evict(region);
err_out:
	return ret;
}

/*
 * This maps two things: guest RAM and for pci passthru mmio space.
 *
 * mmio:
 *  - vfio overloads vm_pgoff to store the mmio start pfn/spa.
 *  - Two things need to happen for mapping mmio range:
 *	1. mapped in the uaddr so VMM can access it.
 *	2. mapped in the hwpt (gfn <-> mmio phys addr) so guest can access it.
 *
 *   This function takes care of the second. The first one is managed by vfio,
 *   and hence is taken care of via vfio_pci_mmap_fault().
 */
static long
mshv_map_user_memory(struct mshv_partition *partition,
		     struct mshv_user_mem_region mem)
{
	struct mshv_mem_region *region;
	struct vm_area_struct *vma;
	bool is_mmio;
	ulong mmio_pfn;
	long ret;

	if (mem.flags & BIT(MSHV_SET_MEM_BIT_UNMAP) ||
	    !access_ok((const void *)mem.userspace_addr, mem.size))
		return -EINVAL;

	mmap_read_lock(current->mm);
	vma = vma_lookup(current->mm, mem.userspace_addr);
	is_mmio = vma ? !!(vma->vm_flags & (VM_IO | VM_PFNMAP)) : 0;
	mmio_pfn = is_mmio ? vma->vm_pgoff : 0;
	mmap_read_unlock(current->mm);

	if (!vma)
		return -EINVAL;

	ret = mshv_partition_create_region(partition, &mem, &region,
					   is_mmio);
	if (ret)
		return ret;

	if (is_mmio)
		ret = hv_call_map_mmio_pages(partition->pt_id, mem.guest_pfn,
					     mmio_pfn, HVPFN_DOWN(mem.size));
	else if (region->flags.range_pinned)
		ret = mshv_handle_pinned_region(region);
	else
		/*
		 * For non-pinned regions, remap with no access to let the
		 * hypervisor track dirty pages, enabling precopy live
		 * migration.
		 */
		ret = mshv_region_remap_pages(region, HV_MAP_GPA_NO_ACCESS,
					       0, region->nr_pages);
	if (ret)
		goto errout;

	/* Install the new region */
	hlist_add_head(&region->hnode, &partition->pt_mem_regions);

	trace_mshv_map_user_memory(partition->pt_id, region->start_uaddr,
				   region->start_gfn, region->nr_pages,
				   region->hv_map_flags, ret);

	return 0;

errout:
	vfree(region);
	return ret;
}

static void mshv_partition_destroy_region(struct mshv_mem_region *region)
{
	struct mshv_partition *partition = region->partition;
	u64 page_offset, page_count;
	u32 unmap_flags = 0;
	int ret;

	hlist_del(&region->hnode);

	if (region->flags.memreg_isram)
		mshv_region_movable_fini(region);

	if (mshv_partition_encrypted(partition)) {
		ret = mshv_partition_region_share(region);
		if (ret) {
			pt_err(partition,
			       "Failed to regain access to memory, unpinning user pages will fail and crash the host error: %d\n",
			       ret);
			return;
		}
	}

	if (region->flags.large_pages)
		unmap_flags |= HV_UNMAP_GPA_LARGE_PAGE;

	/*
	 * Unmap only the mapped pages to optimize performance,
	 * especially for large memory regions.
	 */
	for (page_offset = 0; page_offset < region->nr_pages; page_offset += page_count) {
		page_count = 1;
		if (!region->pages[page_offset])
			continue;

		for (; page_count < region->nr_pages - page_offset; page_count++) {
			if (!region->pages[page_offset + page_count])
				break;
		}

		/* ignore unmap failures and continue as process may be exiting */
		hv_call_unmap_gpa_pages(partition->pt_id,
					region->start_gfn + page_offset,
					page_count, unmap_flags);
	}

	mshv_region_evict(region);

	vfree(region);
}

/* Called for unmapping both the guest ram and the mmio space */
static long
mshv_unmap_user_memory(struct mshv_partition *partition,
		       struct mshv_user_mem_region mem)
{
	struct mshv_mem_region *region;

	if (!(mem.flags & BIT(MSHV_SET_MEM_BIT_UNMAP)))
		return -EINVAL;

	region = mshv_partition_region_by_gfn(partition, mem.guest_pfn);
	if (!region)
		return -EINVAL;

	/* Paranoia check */
	if (region->start_uaddr != mem.userspace_addr ||
	    region->start_gfn != mem.guest_pfn ||
	    region->nr_pages != HVPFN_DOWN(mem.size))
		return -EINVAL;

	mshv_partition_destroy_region(region);
	return 0;
}

static long
mshv_partition_ioctl_set_memory(struct mshv_partition *partition,
				struct mshv_user_mem_region __user *user_mem)
{
	struct mshv_user_mem_region mem;

	if (copy_from_user(&mem, user_mem, sizeof(mem)))
		return -EFAULT;

	if (!mem.size ||
	    !PAGE_ALIGNED(mem.size) ||
	    !PAGE_ALIGNED(mem.userspace_addr) ||
	    (mem.flags & ~MSHV_SET_MEM_FLAGS_MASK) ||
	    mshv_field_nonzero(mem, rsvd))
		return -EINVAL;

	if (mem.flags & BIT(MSHV_SET_MEM_BIT_UNMAP))
		return mshv_unmap_user_memory(partition, mem);

	return mshv_map_user_memory(partition, mem);
}

static long
mshv_partition_ioctl_ioeventfd(struct mshv_partition *partition,
			       void __user *user_args)
{
	struct mshv_user_ioeventfd args;

	if (copy_from_user(&args, user_args, sizeof(args)))
		return -EFAULT;

	return mshv_set_unset_ioeventfd(partition, &args);
}

static long
mshv_partition_ioctl_irqfd(struct mshv_partition *partition,
			   void __user *user_args)
{
	struct mshv_user_irqfd args;

	if (copy_from_user(&args, user_args, sizeof(args)))
		return -EFAULT;

	return mshv_set_unset_irqfd(partition, &args);
}

static long
mshv_partition_ioctl_get_gpap_access_bitmap(struct mshv_partition *partition,
					    void __user *user_args)
{
	struct mshv_gpap_access_bitmap args;
	union hv_gpa_page_access_state *states;
	long ret, i;
	union hv_gpa_page_access_state_flags hv_flags = {};
	u8 hv_type_mask;
	ulong bitmap_buf_sz, states_buf_sz;
	int written = 0;

	if (copy_from_user(&args, user_args, sizeof(args)))
		return -EFAULT;

	if (args.access_type >= MSHV_GPAP_ACCESS_TYPE_COUNT ||
	    args.access_op >= MSHV_GPAP_ACCESS_OP_COUNT ||
	    mshv_field_nonzero(args, rsvd) || !args.page_count ||
	    !args.bitmap_ptr)
		return -EINVAL;

	if (check_mul_overflow(args.page_count, sizeof(*states), &states_buf_sz))
		return -E2BIG;

	/* Num bytes needed to store bitmap; one bit per page rounded up */
	bitmap_buf_sz = DIV_ROUND_UP(args.page_count, 8);

	/* Sanity check */
	if (bitmap_buf_sz > states_buf_sz)
		return -EBADFD;

	switch (args.access_type) {
	case MSHV_GPAP_ACCESS_TYPE_ACCESSED:
		hv_type_mask = 1;
		if (args.access_op == MSHV_GPAP_ACCESS_OP_CLEAR) {
			hv_flags.clear_accessed = 1;
			/* not accessed implies not dirty */
			hv_flags.clear_dirty = 1;
		} else { /* MSHV_GPAP_ACCESS_OP_SET */
			hv_flags.set_accessed = 1;
		}
		break;
	case MSHV_GPAP_ACCESS_TYPE_DIRTY:
		hv_type_mask = 2;
		if (args.access_op == MSHV_GPAP_ACCESS_OP_CLEAR) {
			hv_flags.clear_dirty = 1;
		} else { /* MSHV_GPAP_ACCESS_OP_SET */
			hv_flags.set_dirty = 1;
			/* dirty implies accessed */
			hv_flags.set_accessed = 1;
		}
		break;
	}

	states = vzalloc(states_buf_sz);
	if (!states)
		return -ENOMEM;

	ret = hv_call_get_gpa_access_states(partition->pt_id, args.page_count,
					    args.gpap_base, hv_flags, &written,
					    states);
	if (ret)
		goto free_return;

	/*
	 * Overwrite states buffer with bitmap - the bits in hv_type_mask
	 * correspond to bitfields in hv_gpa_page_access_state
	 */
	for (i = 0; i < written; ++i)
		__assign_bit(i, (ulong *)states,
			     states[i].as_uint8 & hv_type_mask);

	/* zero the unused bits in the last byte(s) of the returned bitmap */
	for (i = written; i < bitmap_buf_sz * 8; ++i)
		__clear_bit(i, (ulong *)states);

	if (copy_to_user((void __user *)args.bitmap_ptr, states, bitmap_buf_sz))
		ret = -EFAULT;

free_return:
	vfree(states);
	return ret;
}

static long
mshv_partition_ioctl_set_msi_routing(struct mshv_partition *partition,
				     void __user *user_args)
{
	struct mshv_user_irq_entry *entries = NULL;
	struct mshv_user_irq_table args;
	long ret;

	if (copy_from_user(&args, user_args, sizeof(args)))
		return -EFAULT;

	if (args.nr > MSHV_MAX_GUEST_IRQS ||
	    mshv_field_nonzero(args, rsvd))
		return -EINVAL;

	if (args.nr) {
		struct mshv_user_irq_table __user *urouting = user_args;

		entries = vmemdup_user(urouting->entries,
				       array_size(sizeof(*entries),
						  args.nr));
		if (IS_ERR(entries))
			return PTR_ERR(entries);
	}
	ret = mshv_update_routing_table(partition, entries, args.nr);
	kvfree(entries);

	return ret;
}

static long
mshv_partition_ioctl_initialize(struct mshv_partition *partition)
{
	long ret;

	if (partition->pt_initialized)
		return 0;

	ret = hv_call_initialize_partition(partition->pt_id);
	if (ret)
		return ret;

	ret = mshv_debugfs_partition_create(partition);
	if (ret)
		goto finalize_partition;

	partition->pt_initialized = true;

	return 0;

finalize_partition:
	hv_call_finalize_partition(partition->pt_id);
	hv_call_withdraw_memory(U64_MAX, NUMA_NO_NODE, partition->pt_id);

	return ret;
}

static long
mshv_partition_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	struct mshv_partition *partition = filp->private_data;
	long ret;
	void __user *uarg = (void __user *)arg;

	if (mutex_lock_killable(&partition->pt_mutex))
		return -EINTR;

	switch (ioctl) {
	case MSHV_INITIALIZE_PARTITION:
		ret = mshv_partition_ioctl_initialize(partition);
		break;
	case MSHV_SET_GUEST_MEMORY:
		ret = mshv_partition_ioctl_set_memory(partition, uarg);
		break;
	case MSHV_CREATE_VP:
		ret = mshv_partition_ioctl_create_vp(partition, uarg);
		break;
	case MSHV_IRQFD:
		ret = mshv_partition_ioctl_irqfd(partition, uarg);
		break;
	case MSHV_IOEVENTFD:
		ret = mshv_partition_ioctl_ioeventfd(partition, uarg);
		break;
	case MSHV_SET_MSI_ROUTING:
		ret = mshv_partition_ioctl_set_msi_routing(partition, uarg);
		break;
	case MSHV_GET_GPAP_ACCESS_BITMAP:
		ret = mshv_partition_ioctl_get_gpap_access_bitmap(partition,
								  uarg);
		break;
	case MSHV_ROOT_HVCALL:
		ret = mshv_ioctl_passthru_hvcall(partition, true, uarg);
		break;
	default:
		ret = -ENOTTY;
	}

	mutex_unlock(&partition->pt_mutex);
	return ret;
}

static int
disable_vp_dispatch(struct mshv_vp *vp)
{
	int ret;
	struct hv_register_assoc dispatch_suspend = {
		.name = HV_REGISTER_DISPATCH_SUSPEND,
		.value.dispatch_suspend.suspended = 1,
	};

	ret = mshv_set_vp_registers(vp->vp_index, vp->vp_partition->pt_id,
				    1, &dispatch_suspend);
	if (ret)
		vp_err(vp, "failed to suspend\n");

	trace_mshv_disable_vp_dispatch(ret, vp->vp_partition->pt_id,
				       vp->vp_index);

	return ret;
}

static int
get_vp_signaled_count(struct mshv_vp *vp, u64 *count)
{
	int ret;
	struct hv_register_assoc root_signal_count = {
		.name = HV_REGISTER_VP_ROOT_SIGNAL_COUNT,
	};

	ret = mshv_get_vp_registers(vp->vp_index, vp->vp_partition->pt_id,
				    1, &root_signal_count);

	if (ret) {
		vp_err(vp, "Failed to get root signal count");
		*count = 0;
		return ret;
	}

	*count = root_signal_count.value.reg64;

	return ret;
}

static void
drain_vp_signals(struct mshv_vp *vp)
{
	u64 hv_signal_count;
	u64 vp_signal_count;

	get_vp_signaled_count(vp, &hv_signal_count);

	vp_signal_count = atomic64_read(&vp->run.vp_signaled_count);

	/*
	 * There should be at most 1 outstanding notification, but be extra
	 * careful anyway.
	 */
	while (hv_signal_count != vp_signal_count) {
		WARN_ON(hv_signal_count - vp_signal_count != 1);

		if (wait_event_interruptible(vp->run.vp_suspend_queue,
					     vp->run.kicked_by_hv == 1))
			break;
		vp->run.kicked_by_hv = 0;
		vp_signal_count = atomic64_read(&vp->run.vp_signaled_count);
	}

	trace_mshv_drain_vp_signals(vp->vp_partition->pt_id, vp->vp_index);
}

static void drain_all_vps(const struct mshv_partition *partition)
{
	int i;
	struct mshv_vp *vp;

	/*
	 * VPs are reachable from ISR. It is safe to not take the partition
	 * lock because nobody else can enter this function and drop the
	 * partition from the list.
	 */
	for (i = 0; i < MSHV_MAX_VPS; i++) {
		vp = partition->pt_vp_array[i];
		if (!vp)
			continue;
		/*
		 * Disable dispatching of the VP in the hypervisor. After this
		 * the hypervisor guarantees it won't generate any signals for
		 * the VP and the hypervisor's VP signal count won't change.
		 */
		disable_vp_dispatch(vp);
		drain_vp_signals(vp);
	}
}

static void
remove_partition(struct mshv_partition *partition)
{
	spin_lock(&mshv_root.pt_ht_lock);
	hlist_del_rcu(&partition->pt_hnode);
	spin_unlock(&mshv_root.pt_ht_lock);

	synchronize_rcu();
}

/*
 * Tear down a partition and remove it from the list.
 * Partition's refcount must be 0
 */
static void destroy_partition(struct mshv_partition *partition)
{
	if (refcount_read(&partition->pt_ref_count)) {
		pt_err(partition,
		       "Attempt to destroy partition but refcount > 0\n");
		return;
	}

	trace_mshv_destroy_partition(partition->pt_id);

	if (partition->pt_initialized) {
		struct mshv_mem_region *region;
		struct hlist_node *n;
		struct mshv_vp *vp;
		int i;

		/*
		 * We only need to drain signals for root scheduler. This should be
		 * done before removing the partition from the partition list.
		 */
		if (hv_scheduler_type == HV_SCHEDULER_TYPE_ROOT)
			drain_all_vps(partition);

		/* Remove vps */
		for (i = 0; i < MSHV_MAX_VPS; ++i) {
			vp = partition->pt_vp_array[i];
			if (!vp)
				continue;

			mshv_debugfs_vp_remove(vp);

			if (!hv_l1vh_partition())
				mshv_vp_stats_unmap(partition->pt_id, vp->vp_index,
						    (void **)vp->vp_stats_pages);

			if (vp->vp_register_page) {
				(void)hv_unmap_vp_state_page(partition->pt_id,
							     vp->vp_index,
							     HV_VP_STATE_PAGE_REGISTERS,
							     vp->vp_register_page,
							     input_vtl_zero);
				vp->vp_register_page = NULL;
			}

			(void)hv_unmap_vp_state_page(partition->pt_id,
						     vp->vp_index,
						     HV_VP_STATE_PAGE_INTERCEPT_MESSAGE,
						     vp->vp_intercept_msg_page,
						     input_vtl_zero);
			vp->vp_intercept_msg_page = NULL;

			if (vp->vp_ghcb_page) {
				(void)hv_unmap_vp_state_page(partition->pt_id,
							     vp->vp_index,
							     HV_VP_STATE_PAGE_GHCB,
							     vp->vp_ghcb_page,
							     input_vtl_normal);
				vp->vp_ghcb_page = NULL;
			}

			kfree(vp);

			partition->pt_vp_array[i] = NULL;
		}

		hlist_for_each_entry_safe(region, n, &partition->pt_mem_regions,
					  hnode)
			mshv_partition_destroy_region(region);

		mshv_debugfs_partition_remove(partition);

		/* Deallocates and unmaps everything including vcpus, GPA mappings etc */
		hv_call_finalize_partition(partition->pt_id);

		partition->pt_initialized = false;
	}

	remove_partition(partition);

	/* Withdraw and free all pages we deposited */
	hv_call_withdraw_memory(U64_MAX, NUMA_NO_NODE, partition->pt_id);
	hv_call_delete_partition(partition->pt_id);

	mshv_free_routing_table(partition);
	kfree(partition);
}

struct
mshv_partition *mshv_partition_get(struct mshv_partition *partition)
{
	if (refcount_inc_not_zero(&partition->pt_ref_count))
		return partition;
	return NULL;
}

struct
mshv_partition *mshv_partition_find(u64 partition_id)
	__must_hold(RCU)
{
	struct mshv_partition *p;

	hash_for_each_possible_rcu(mshv_root.pt_htable, p, pt_hnode,
				   partition_id)
		if (p->pt_id == partition_id)
			return p;

	return NULL;
}

void
mshv_partition_put(struct mshv_partition *partition)
{
	if (refcount_dec_and_test(&partition->pt_ref_count))
		destroy_partition(partition);
}

static int
mshv_partition_release(struct inode *inode, struct file *filp)
{
	struct mshv_partition *partition = filp->private_data;

	trace_mshv_partition_release(partition->pt_id);

	mshv_eventfd_release(partition);

	cleanup_srcu_struct(&partition->pt_irq_srcu);

	mshv_partition_put(partition);

	return 0;
}

static int
add_partition(struct mshv_partition *partition)
{
	spin_lock(&mshv_root.pt_ht_lock);

	hash_add_rcu(mshv_root.pt_htable, &partition->pt_hnode,
		     partition->pt_id);

	spin_unlock(&mshv_root.pt_ht_lock);

	return 0;
}

static_assert(MSHV_NUM_CPU_FEATURES_BANKS <=
	      HV_PARTITION_PROCESSOR_FEATURES_BANKS);

static long mshv_ioctl_process_pt_flags(void __user *user_arg, u64 *pt_flags,
			struct hv_partition_creation_properties *cr_props,
			union hv_partition_isolation_properties *isol_props)
{
	int i, len;
	struct mshv_create_partition_v2 args;
	union hv_partition_processor_features *disabled_procs;
	union hv_partition_processor_xsave_features *disabled_xsave;

	/* First, copy orig struct in case user is on previous versions */
	len = sizeof(struct mshv_create_partition);
	if (copy_from_user(&args, user_arg, len))
		return -EFAULT;

	if ((args.pt_flags & ~MSHV_PT_FLAGS_MASK) ||
	     args.pt_isolation >= MSHV_PT_ISOLATION_COUNT)
		return -EINVAL;

	disabled_procs = &cr_props->disabled_processor_features;

	/* Disable all processor features first */
	for (i = 0; i < HV_PARTITION_PROCESSOR_FEATURES_BANKS; i++)
		disabled_procs->as_uint64[i] = -1;

#if defined(__x86_64__)
	/* Enable default features that are known to be supported */
	disabled_procs->cet_ibt_support = 0;
	disabled_procs->cet_ss_support = 0;
	disabled_procs->smep_support = 0;
	disabled_procs->rdtscp_support = 0;
	disabled_procs->tsc_invariant_support = 0;
	disabled_procs->sse3_support = 0;
	disabled_procs->lahf_sahf_support = 0;
	disabled_procs->ssse3_support = 0;
	disabled_procs->sse4_1_support = 0;
	disabled_procs->sse4_2_support = 0;
	disabled_procs->sse4a_support = 0;
	disabled_procs->xop_support = 0;
	disabled_procs->pop_cnt_support = 0;
	disabled_procs->cmpxchg16b_support = 0;
	disabled_procs->altmovcr8_support = 0;
	disabled_procs->lzcnt_support = 0;
	disabled_procs->mis_align_sse_support = 0;
	disabled_procs->mmx_ext_support = 0;
	disabled_procs->amd3dnow_support = 0;
	disabled_procs->extended_amd3dnow_support = 0;
	disabled_procs->aes_support = 0;
	disabled_procs->pclmulqdq_support = 0;
	disabled_procs->pcid_support = 0;
	disabled_procs->fma4_support = 0;
	disabled_procs->f16c_support = 0;
	disabled_procs->rd_rand_support = 0;
	disabled_procs->rd_wr_fs_gs_support = 0;
	disabled_procs->enhanced_fast_string_support = 0;
	disabled_procs->bmi1_support = 0;
	disabled_procs->bmi2_support = 0;
	disabled_procs->hle_support_deprecated = 0;
	disabled_procs->rtm_support_deprecated = 0;
	disabled_procs->movbe_support = 0;
	disabled_procs->npiep1_support = 0;
	disabled_procs->dep_x87_fpu_save_support = 0;
	disabled_procs->rd_seed_support = 0;
	disabled_procs->adx_support = 0;
	disabled_procs->intel_prefetch_support = 0;
	disabled_procs->smap_support = 0;
	disabled_procs->hle_support = 0;
	disabled_procs->rtm_support = 0;
	disabled_procs->invpcid_support = 0;
	disabled_procs->ibrs_support = 0;
	disabled_procs->stibp_support = 0;
	disabled_procs->mdd_support = 0;
	disabled_procs->ibpb_support = 0;
	disabled_procs->l1dcache_flush_support = 0;
	disabled_procs->virt_spec_ctrl_support = 0;
	disabled_procs->mb_clear_support = 0;
	disabled_procs->tsx_ctrl_support = 0;
	disabled_procs->clflushopt_support = 0;
	disabled_procs->rdcl_no_support = 0;
	disabled_procs->ibrs_all_support = 0;
	disabled_procs->page_1gb_support = 0;
	disabled_procs->skip_l1df_support = 0;
	disabled_procs->ssb_no_support = 0;
	disabled_procs->mbs_no_support = 0;
	disabled_procs->taa_no_support = 0;
	disabled_procs->fb_clear_support = 0;
	disabled_procs->gds_no_support = 0;
	disabled_procs->bhi_no_support = 0;
	disabled_procs->bhi_dis_support = 0;
	disabled_procs->btc_no_support = 0;
	disabled_procs->mitigation_ctrl_support = 0;
	disabled_procs->rfds_no_support = 0;
	disabled_procs->rfds_clear_support = 0;
	disabled_procs->unrestricted_guest_support = 0;
	disabled_procs->fast_short_rep_mov_support = 0;
	disabled_procs->rsb_a_no_support = 0;
	disabled_procs->rd_pid_support = 0;
	disabled_procs->umip_support = 0;
	disabled_procs->vmx_exception_inject_support = 0;
	disabled_procs->rdpru_support = 0;
	disabled_procs->mbec_support = 0;
	disabled_procs->psfd_support = 0;

	/* Enable default XSave features that are known to be supported*/
	disabled_xsave = &cr_props->disabled_processor_xsave_features;
	disabled_xsave->as_uint64 = -1;
	disabled_xsave->xsave_support = 0;
	disabled_xsave->xsaveopt_support = 0;
	disabled_xsave->avx_support = 0;
	disabled_xsave->xsave_supervisor_support = 0;
	disabled_xsave->xsave_comp_support = 0;
#endif
	/* Check if user provided newer struct with feature fields */
	if (args.pt_flags & BIT(MSHV_PT_BIT_CPU_AND_XSAVE_FEATURES)) {
		if (copy_from_user(&args, user_arg, sizeof(args)))
			return -EFAULT;

		if (args.pt_num_cpu_fbanks > MSHV_NUM_CPU_FEATURES_BANKS ||
			mshv_field_nonzero(args, pt_rsvd) ||
			mshv_field_nonzero(args, pt_rsvd1))
			return -EINVAL;

		for (i = 0; i < args.pt_num_cpu_fbanks; i++)
			disabled_procs->as_uint64[i] = args.pt_cpu_fbanks[i];
#if defined(__x86_64__)
		disabled_xsave->as_uint64 = args.pt_disabled_xsave;
#endif
	}

	/* Only support EXO partitions */
	*pt_flags = HV_PARTITION_CREATION_FLAG_EXO_PARTITION |
		    HV_PARTITION_CREATION_FLAG_INTERCEPT_MESSAGE_PAGE_ENABLED;

	if (args.pt_flags & BIT(MSHV_PT_BIT_LAPIC))
		*pt_flags |= HV_PARTITION_CREATION_FLAG_LAPIC_ENABLED;
	if (args.pt_flags & BIT(MSHV_PT_BIT_X2APIC))
		*pt_flags |= HV_PARTITION_CREATION_FLAG_X2APIC_CAPABLE;
	if (args.pt_flags & BIT(MSHV_PT_BIT_GPA_SUPER_PAGES))
		*pt_flags |= HV_PARTITION_CREATION_FLAG_GPA_SUPER_PAGES_ENABLED;

	switch (args.pt_isolation) {
	case MSHV_PT_ISOLATION_NONE:
		isol_props->isolation_type = HV_PARTITION_ISOLATION_TYPE_NONE;
		break;
	case MSHV_PT_ISOLATION_SNP:
		isol_props->isolation_type = HV_PARTITION_ISOLATION_TYPE_SNP;
		break;
	}

	return 0;
}

static long mshv_ioctl_get_host_partition_property(void __user *user_args)
{
	struct mshv_partition_property args;
	long ret;

	if (copy_from_user(&args, user_args, sizeof(args)))
		return -EFAULT;

	ret = hv_call_get_partition_property(HV_PARTITION_ID_SELF,
					     args.property_code, &args.property_value);
	if (ret)
		return ret;

	if (copy_to_user(user_args, &args, sizeof(args)))
		return -EFAULT;

	return 0;
}

static long
mshv_ioctl_create_partition(void __user *user_arg, struct device *module_dev)
{
	u64 creation_flags;
	struct hv_partition_creation_properties creation_properties = {};
	union hv_partition_isolation_properties isolation_properties = {};
	struct mshv_partition *partition;
	struct file *file;
	int fd;
	long ret;

	ret = mshv_ioctl_process_pt_flags(user_arg, &creation_flags,
			&creation_properties, &isolation_properties);
	if (ret)
		return ret;

	partition = kzalloc(sizeof(*partition), GFP_KERNEL);
	if (!partition)
		return -ENOMEM;

	partition->pt_module_dev = module_dev;
	partition->isolation_type = isolation_properties.isolation_type;

	refcount_set(&partition->pt_ref_count, 1);

	mutex_init(&partition->pt_mutex);

	mutex_init(&partition->pt_irq_lock);

	init_completion(&partition->async_hypercall);

	INIT_HLIST_HEAD(&partition->irq_ack_notifier_list);

	INIT_HLIST_HEAD(&partition->pt_devices);

	INIT_HLIST_HEAD(&partition->pt_mem_regions);

	mshv_eventfd_init(partition);

	ret = init_srcu_struct(&partition->pt_irq_srcu);
	if (ret)
		goto free_partition;

	ret = hv_call_create_partition(creation_flags,
				       creation_properties,
				       isolation_properties,
				       &partition->pt_id);
	if (ret)
		goto cleanup_irq_srcu;

	ret = add_partition(partition);
	if (ret)
		goto delete_partition;

	ret = mshv_init_async_handler(partition);
	if (ret)
		goto remove_partition;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto remove_partition;
	}

	file = anon_inode_getfile("mshv_partition", &mshv_partition_fops,
				  partition, O_RDWR);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto put_fd;
	}

	fd_install(fd, file);

	trace_mshv_create_partition(ret, partition->pt_id, fd);

	return fd;

put_fd:
	put_unused_fd(fd);
remove_partition:
	remove_partition(partition);
delete_partition:
	hv_call_delete_partition(partition->pt_id);
cleanup_irq_srcu:
	cleanup_srcu_struct(&partition->pt_irq_srcu);
free_partition:
	kfree(partition);

	trace_mshv_create_partition(ret, 0, -1);

	return ret;
}

static long mshv_dev_ioctl(struct file *filp, unsigned int ioctl,
			   unsigned long arg)
{
	struct miscdevice *misc = filp->private_data;

	switch (ioctl) {
	case MSHV_CREATE_PARTITION:
		return mshv_ioctl_create_partition((void __user *)arg,
						misc->this_device);
	case MSHV_GET_HOST_PARTITION_PROPERTY:
		return mshv_ioctl_get_host_partition_property(
			(void __user *)arg);
	}

	return -ENOTTY;
}

static int
mshv_dev_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int
mshv_dev_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static int mshv_cpuhp_online;
static int mshv_root_sched_online;

static const char *scheduler_type_to_string(enum hv_scheduler_type type)
{
	switch (type) {
	case HV_SCHEDULER_TYPE_LP:
		return "classic scheduler without SMT";
	case HV_SCHEDULER_TYPE_LP_SMT:
		return "classic scheduler with SMT";
	case HV_SCHEDULER_TYPE_CORE_SMT:
		return "core scheduler";
	case HV_SCHEDULER_TYPE_ROOT:
		return "root scheduler";
	default:
		return "unknown scheduler";
	};
}

/* Retrieve and stash the supported scheduler type */
static int __init mshv_retrieve_scheduler_type(struct device *dev)
{
	int ret;

	ret = hv_retrieve_scheduler_type(&hv_scheduler_type);
	if (ret)
		return ret;

	dev_info(dev, "Hypervisor using %s\n",
		 scheduler_type_to_string(hv_scheduler_type));

	switch (hv_scheduler_type) {
	case HV_SCHEDULER_TYPE_CORE_SMT:
	case HV_SCHEDULER_TYPE_LP_SMT:
	case HV_SCHEDULER_TYPE_ROOT:
	case HV_SCHEDULER_TYPE_LP:
		/* Supported scheduler, nothing to do */
		break;
	default:
		dev_err(dev, "unsupported scheduler 0x%x, bailing.\n",
			hv_scheduler_type);
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mshv_root_scheduler_init(unsigned int cpu)
{
	void **inputarg, **outputarg, *p;

	inputarg = (void **)this_cpu_ptr(root_scheduler_input);
	outputarg = (void **)this_cpu_ptr(root_scheduler_output);

	/* Allocate two consecutive pages. One for input, one for output. */
	p = kmalloc(2 * HV_HYP_PAGE_SIZE, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	*inputarg = p;
	*outputarg = (char *)p + HV_HYP_PAGE_SIZE;

	return 0;
}

static int mshv_root_scheduler_cleanup(unsigned int cpu)
{
	void *p, **inputarg, **outputarg;

	inputarg = (void **)this_cpu_ptr(root_scheduler_input);
	outputarg = (void **)this_cpu_ptr(root_scheduler_output);

	p = *inputarg;

	*inputarg = NULL;
	*outputarg = NULL;

	kfree(p);

	return 0;
}

/* Must be called after retrieving the scheduler type */
static int
root_scheduler_init(struct device *dev)
{
	int ret;

	if (hv_scheduler_type != HV_SCHEDULER_TYPE_ROOT)
		return 0;

	root_scheduler_input = alloc_percpu(void *);
	root_scheduler_output = alloc_percpu(void *);

	if (!root_scheduler_input || !root_scheduler_output) {
		dev_err(dev, "Failed to allocate root scheduler buffers\n");
		ret = -ENOMEM;
		goto out;
	}

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mshv_root_sched",
				mshv_root_scheduler_init,
				mshv_root_scheduler_cleanup);

	if (ret < 0) {
		dev_err(dev, "Failed to setup root scheduler state: %i\n", ret);
		goto out;
	}

	mshv_root_sched_online = ret;

	return 0;

out:
	free_percpu(root_scheduler_input);
	free_percpu(root_scheduler_output);
	return ret;
}

static void
root_scheduler_deinit(void)
{
	if (hv_scheduler_type != HV_SCHEDULER_TYPE_ROOT)
		return;

	cpuhp_remove_state(mshv_root_sched_online);
	free_percpu(root_scheduler_input);
	free_percpu(root_scheduler_output);
}

static int mshv_reboot_notify(struct notifier_block *nb,
			      unsigned long code, void *unused)
{
	cpuhp_remove_state(mshv_cpuhp_online);
	return 0;
}

struct notifier_block mshv_reboot_nb = {
	.notifier_call = mshv_reboot_notify,
};

static int __init mshv_l1vh_partition_init(struct device *dev)
{
	hv_scheduler_type = HV_SCHEDULER_TYPE_CORE_SMT;
	dev_info(dev, "Hypervisor using %s\n",
		 scheduler_type_to_string(hv_scheduler_type));

	return 0;
}

static void mshv_root_partition_exit(void)
{
	unregister_reboot_notifier(&mshv_reboot_nb);
	root_scheduler_deinit();
}

static int __init mshv_root_partition_init(struct device *dev)
{
	int err;

	if (mshv_retrieve_scheduler_type(dev))
		return -ENODEV;

	err = root_scheduler_init(dev);
	if (err)
		return err;

	err = register_reboot_notifier(&mshv_reboot_nb);
	if (err)
		goto root_sched_deinit;

	return 0;

root_sched_deinit:
	root_scheduler_deinit();
	return err;
}

static int mshv_init_vmm_caps(struct device *dev)
{
	int ret;

	memset(&mshv_root.vmm_caps, 0, sizeof(mshv_root.vmm_caps));
	ret = hv_call_get_partition_property_ex(HV_PARTITION_ID_SELF,
						HV_PARTITION_PROPERTY_VMM_CAPABILITIES,
						0, &mshv_root.vmm_caps,
						sizeof(mshv_root.vmm_caps));

	/*
	 * HV_PARTITION_PROPERTY_VMM_CAPABILITIES is not supported in
	 * older hyperv. Ignore the -EIO error code.
	 */
	if (ret && ret != -EIO)
		return ret;

	dev_dbg(dev, "vmm_caps=0x%llx\n", mshv_root.vmm_caps.as_uint64[0]);

	return 0;
}

static int __init mshv_parent_partition_init(void)
{
	int ret;
	struct device *dev;
	union hv_hypervisor_version_info version_info;

	if (!hv_parent_partition() || is_kdump_kernel())
		return -ENODEV;

	if (hv_get_hypervisor_version(&version_info))
		return -ENODEV;

	ret = misc_register(&mshv_dev);
	if (ret)
		return ret;

	dev = mshv_dev.this_device;

	if (version_info.build_number < MSHV_HV_MIN_VERSION ||
	    version_info.build_number > MSHV_HV_MAX_VERSION) {
		dev_err(dev, "Running on unvalidated Hyper-V version\n");
		dev_err(dev, "Versions: current: %u  min: %u  max: %u\n",
			version_info.build_number, MSHV_HV_MIN_VERSION,
			MSHV_HV_MAX_VERSION);
	}

	mshv_root.synic_pages = alloc_percpu(struct hv_synic_pages);
	if (!mshv_root.synic_pages) {
		dev_err(dev, "Failed to allocate percpu synic page\n");
		ret = -ENOMEM;
		goto device_deregister;
	}

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mshv_synic",
				mshv_synic_init,
				mshv_synic_cleanup);
	if (ret < 0) {
		dev_err(dev, "Failed to setup cpu hotplug state: %i\n", ret);
		goto free_synic_pages;
	}

	mshv_cpuhp_online = ret;

	if (hv_root_partition())
		ret = mshv_root_partition_init(dev);
	else
		ret = mshv_l1vh_partition_init(dev);
	if (ret)
		goto remove_cpu_state;

	ret = mshv_init_vmm_caps(dev);
	if (ret) {
		dev_err(dev, "Failed to get VMM capabilities: %d\n", ret);
		goto exit_partition;
	}

	ret = mshv_debugfs_init();
	if (ret)
		goto exit_partition;

	ret = mshv_irqfd_wq_init();
	if (ret)
		goto exit_debugfs;

	spin_lock_init(&mshv_root.pt_ht_lock);
	hash_init(mshv_root.pt_htable);

	hv_setup_mshv_handler(mshv_isr);

	return 0;

destroy_irqds_wq:
	mshv_irqfd_wq_cleanup();
exit_debugfs:
	mshv_debugfs_exit();
exit_partition:
	if (hv_root_partition())
		mshv_root_partition_exit();
remove_cpu_state:
	cpuhp_remove_state(mshv_cpuhp_online);
free_synic_pages:
	free_percpu(mshv_root.synic_pages);
device_deregister:
	misc_deregister(&mshv_dev);
	return ret;
}

static void __exit mshv_parent_partition_exit(void)
{
	hv_setup_mshv_handler(NULL);
	mshv_port_table_fini();
	mshv_debugfs_exit();
	mshv_irqfd_wq_cleanup();
	if (hv_root_partition())
		mshv_root_partition_exit();
	cpuhp_remove_state(mshv_cpuhp_online);
	free_percpu(mshv_root.synic_pages);
}

module_init(mshv_parent_partition_init);
module_exit(mshv_parent_partition_exit);
