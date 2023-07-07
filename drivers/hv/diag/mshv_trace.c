// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023, Microsoft Corporation.
 *
 * Functions used to read events from Microsoft Hypervisor's Local Diagnostics
 *
 * Author:
 *   Stanislav Kinsburskii <skinsburskii@linux.microsoft.com>
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/anon_inodes.h>
#include <linux/mshv.h>
#include <hyperv/hvtrapi.h>
#include <asm/mshyperv.h>

#include "mshv_diag.h"

struct mshv_trace_buffer {
};

struct mshv_trace_state {
	struct mshv_trace_config cfg;
	enum hv_eventlog_type type;
	struct mshv_trace_buffer *tbs;
	struct mshv_trace *trace;
};

struct mshv_trace {
};

static DEFINE_MUTEX(mshv_trace_state_lock);
static struct mshv_trace_state *mshv_trace_state;

static int hv_call_initialize_event_log_buffer_group(enum hv_eventlog_type type,
			     enum hv_eventlog_mode mode,
			     u32 max_buffers_count,
			     u32 pages_per_buffer,
			     u32 buffers_threshold,
			     enum hv_eventlog_entry_time_basis time_basis,
			     u64 system_time)
{
	struct hv_input_initialize_eventlog_buffer_group *input;
	unsigned long flags;
	u64 status;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);

	input->init.type = type;
	input->init.mode = mode;
	input->maximum_buffer_count = max_buffers_count;
	input->buffer_size_in_bytes = pages_per_buffer * PAGE_SIZE;
	input->threshold = buffers_threshold;
	input->time_basis = time_basis;
	input->system_time = system_time;

	status = hv_do_hypercall(HVCALL_INITIALIZE_EVENT_LOG_BUFFER_GROUP,
				 input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("%s: hypercall failed: %s\n",
		       __func__, hv_status_to_string(status));

	return hv_status_to_errno(status);
}

static int hv_call_finalize_event_log_buffer_group(enum hv_eventlog_type type)
{
	union hv_input_finalize_eventlog_buffer_group input;
	u64 status;

	input.type = type;

	status = hv_do_fast_hypercall8(HVCALL_FINALIZE_EVENT_LOG_BUFFER_GROUP,
				       input.as_uint64);

	if (!hv_result_success(status))
		pr_err("%s: hypercall failed: %s\n",
		       __func__, hv_status_to_string(status));

	return hv_status_to_errno(status);
}

static int mshv_trace_buffers_group_init(const struct mshv_trace_config *cfg,
					 enum hv_eventlog_type type,
					 struct mshv_trace_state **statep)
{
	struct mshv_trace_state *state;
	int err;

	state = kzalloc(sizeof(struct mshv_trace_state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	err = hv_call_initialize_event_log_buffer_group(type,
							cfg->mode,
							cfg->max_buffers_count,
							cfg->pages_per_buffer,
							cfg->buffers_threshold,
							cfg->time_basis,
							cfg->system_time);
	if (err)
		goto free_lb;

	state->type = type;
	memcpy(&state->cfg, cfg, sizeof(state->cfg));

	*statep = state;

	return 0;

free_lb:
	kfree(state);
	return err;
}

static int hv_call_delete_event_log_buffer(enum hv_eventlog_type type,
					   u32 buffer_index)
{
	union hv_input_delete_eventlog_buffer input;
	u64 status;

	input.type = type;
	input.buffer_index = buffer_index;

	status = hv_do_fast_hypercall8(HVCALL_DELETE_EVENT_LOG_BUFFER,
				       input.as_uint64);

	if (!hv_result_success(status))
		pr_err("%s: hypercall failed: %s\n",
		       __func__, hv_status_to_string(status));

	return hv_status_to_errno(status);
}

static int hv_call_create_event_log_buffer(enum hv_eventlog_type type,
					   u32 buffer_index)
{
	union hv_input_create_eventlog_buffer input;
	u64 status;

	input.type = type;
	input.buffer_index = buffer_index;
	/*
	 * There isn't logic in the hypervisor yet to prefer NUMA local buffers
	 * when allocating a free one, so probably proximity doesn't matter at
	 * the moment.
	 * Disable it for now.
	 */
	input.proximity_info =
		numa_node_to_proximity_domain_info(NUMA_NO_NODE);

	status = hv_do_fast_hypercall16(HVCALL_CREATE_EVENT_LOG_BUFFER,
					input.as_uint64[0], input.as_uint64[1]);

	if (!hv_result_success(status))
		pr_err("%s: hypercall failed: %s\n",
		       __func__, hv_status_to_string(status));

	return hv_status_to_errno(status);
}

static int mshv_trace_buffer_delete(const struct mshv_trace_state *state,
				    u32 buffer_index)
{
	int err;

	err = hv_call_delete_event_log_buffer(state->type, buffer_index);
	if (err)
		pr_err("%s: failed to delete trace buffer %u: %d\n",
		       __func__, buffer_index, err);
	return err;
}

static int mshv_trace_buffers_delete(struct mshv_trace_state *state)
{
	int i, err;

	for (i = 0; i < state->cfg.max_buffers_count; i++) {
		err = mshv_trace_buffer_delete(state, i);
		if (err)
			return err;
	}

	kfree(state->tbs);
	state->tbs = NULL;

	return 0;
}

static int mshv_trace_buffer_create(const struct mshv_trace_state *state,
				    u32 buffer_index)
{
	int err;

	err = hv_call_create_event_log_buffer(state->type, buffer_index);
	if (err)
		pr_err("%s: failed to create trace buffer %u: %d\n",
		       __func__, buffer_index, err);
	return err;
}

static int mshv_trace_buffers_create(struct mshv_trace_state *state)
{
	struct mshv_trace_buffer *tbs;
	int i, err;

	tbs = kcalloc(state->cfg.max_buffers_count,
		      sizeof(struct mshv_trace_buffer), GFP_KERNEL);
	if (!tbs)
		return -ENOMEM;

	for (i = 0; i < state->cfg.max_buffers_count; i++) {
		err = mshv_trace_buffer_create(state, i);
		if (err)
			goto free_buffers;
	}

	state->tbs = tbs;

	return 0;

free_buffers:
	for (i -= 1; i >= 0; i--)
		(void)mshv_trace_buffer_delete(state, i);
	kfree(tbs);
	return err;
}

static int mshv_trace_buffers_group_fini(struct mshv_trace_state *state)
{
	int err;

	err = hv_call_finalize_event_log_buffer_group(state->type);
	if (err) {
		pr_err("%s: failed to finalize trace buffer group\n",
		       __func__);
		return err;
	}

	kfree(state);

	return 0;
}

static int mshv_trace_state_create(struct mshv_trace_state **statep,
				   const struct mshv_trace_config *cfg)
{
	struct mshv_trace_state *state;
	enum hv_eventlog_type type = HV_EVENT_LOG_TYPE_LOCAL_DIAGNOSTICS;
	int err;

	err = mshv_trace_buffers_group_init(cfg, type, &state);
	if (err) {
		pr_err("%s: failed to initialize trace buffer group: %d\n",
		       __func__, err);
		return err;
	}

	err = mshv_trace_buffers_create(state);
	if (err) {
		pr_err("%s: failed to create trace buffers: %d\n",
		       __func__, err);
		goto finalize_state;
	}

	*statep = state;

	return 0;

finalize_state:
	(void)mshv_trace_buffers_group_fini(state);
	return err;
}

static int mshv_trace_state_destroy(struct mshv_trace_state **statep)
{
	int err;

	err = mshv_trace_buffers_delete(*statep);
	if (!err)
		err = mshv_trace_buffers_group_fini(*statep);
	if (err)
		return err;

	*statep = NULL;

	return 0;
}

static int mshv_trace_check_config(const struct mshv_trace_config *cfg)
{
	if (cfg->mode >= HV_EVENT_LOG_MODE_MAX) {
		pr_err("%s: unknown even log mode: %u\n", __func__, cfg->mode);
		return -EINVAL;
	}

	if (!cfg->max_buffers_count) {
		pr_err("%s: buffers count must be non-zero\n", __func__);
		return -EINVAL;
	}

	if (cfg->max_buffers_count > PAGE_SIZE / sizeof(u64)) {
		pr_err("%s: buffers count is too big: %u > %lu\n", __func__,
			cfg->max_buffers_count, PAGE_SIZE / sizeof(u64));
		return -EINVAL;
	}

	if (cfg->buffers_threshold > cfg->max_buffers_count) {
		pr_err("%s: buffers threshold is too big: %u > %u\n", __func__,
			cfg->buffers_threshold,	cfg->max_buffers_count);
		return -EINVAL;
	}

	return 0;
}

static int mshv_trace_create_state_ioctl(struct mshv_trace_state **statep,
					 const void __user *arg)
{
	struct mshv_trace_config config;

	if (*statep)
		return -EEXIST;

	if (copy_from_user(&config, arg, sizeof(config)))
		return -EFAULT;

	if (mshv_trace_check_config(&config))
		return -EINVAL;

	return mshv_trace_state_create(statep, &config);
}

static int mshv_trace_get_state_ioctl(const struct mshv_trace_state *state,
				      void __user *arg)
{
	if (!state)
		return -ENOENT;

	if (copy_to_user(arg, &state->cfg, sizeof(state->cfg)))
		return -EFAULT;

	return 0;
}

static int mshv_trace_destroy_state_ioctl(struct mshv_trace_state **statep)
{
	if (!*statep)
		return -ENOENT;

	if ((*statep)->trace)
		return -EBUSY;

	return mshv_trace_state_destroy(statep);
}

static int mshv_trace_attach_state_ioctl(struct mshv_trace *trace,
					 struct mshv_trace_state *state)
{
	if (!state)
		return -ENOENT;

	if (state->trace == trace)
		return -EALREADY;

	if (state->trace)
		return -EBUSY;

	state->trace = trace;

	return 0;
}

static int mshv_trace_detach_state_ioctl(const struct mshv_trace *trace,
					 struct mshv_trace_state *state)
{
	if (!state)
		return -ENOENT;

	if (state->trace != trace)
		return -EPERM;

	state->trace = NULL;

	return 0;
}

static long mshv_trace_ioctl(struct file *filp, unsigned int ioctl,
			     unsigned long arg)
{
	struct mshv_trace *trace = filp->private_data;
	int ret = -ENOTTY;

	/*
	 * This lock serializes global trace state creation, mutation and
	 * destruction.
	 */
	mutex_lock(&mshv_trace_state_lock);

	switch (ioctl) {
	case MSHV_TRACE_STATE_CREATE:
		ret = mshv_trace_create_state_ioctl(&mshv_trace_state,
						    (void __user *)arg);
		break;
	case MSHV_TRACE_STATE_INFO:
		ret = mshv_trace_get_state_ioctl(mshv_trace_state,
						 (void __user *)arg);
		break;
	case MSHV_TRACE_STATE_DESTROY:
		ret = mshv_trace_destroy_state_ioctl(&mshv_trace_state);
		break;
	case MSHV_TRACE_STATE_ATTACH:
		ret = mshv_trace_attach_state_ioctl(trace, mshv_trace_state);
		break;
	case MSHV_TRACE_STATE_DETACH:
		ret = mshv_trace_detach_state_ioctl(trace, mshv_trace_state);
		break;
	}

	mutex_unlock(&mshv_trace_state_lock);

	return ret;
}

static int mshv_trace_release(struct inode *inode, struct file *filp)
{
	struct mshv_trace *trace = filp->private_data;

	if (mshv_trace_state && mshv_trace_state->trace == trace)
		mshv_trace_state->trace = NULL;

	kfree(trace);

	return 0;
}

static const struct file_operations mshv_trace_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mshv_trace_ioctl,
	.release = mshv_trace_release,
};

static struct mshv_trace *mshv_trace_create(void)
{
	struct mshv_trace *trace;

	trace = kzalloc(sizeof(struct mshv_trace), GFP_KERNEL);
	if (!trace)
		return ERR_PTR(-ENOMEM);

	return trace;
}

int mshv_trace_get_fd(void)
{
	struct mshv_trace *trace;
	int fd;

	trace = mshv_trace_create();
	if (IS_ERR(trace))
		return PTR_ERR(trace);

	fd = anon_inode_getfd("mshv_trace",
			      &mshv_trace_fops, trace,
			      O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		kfree(trace);

	return fd;
}
