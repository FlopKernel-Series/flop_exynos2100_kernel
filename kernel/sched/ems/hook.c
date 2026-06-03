/*
 * Add hook to trace point
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include "../sched.h"

#include <linux/ems.h>
#include "ems.h"

#include <trace/events/sched.h>
#include <trace/events/ems_debug.h>
#include <trace/hooks/sched.h>

/******************************************************************************
 * Android vendor hook handlers                                               *
 ******************************************************************************/
static void ems_hook_select_task_rq_fair(void *data,
			struct task_struct *p, int prev_cpu,
			int sd_flag, int wake_flags, int *new_cpu)
{
	int cpu;

	if ((sd_flag == SD_BALANCE_WAKE) || (sd_flag == SD_BALANCE_FORK))
		ems_last_waked(p) = ktime_get_ns();

	cpu = ems_select_task_rq_fair(p, prev_cpu, sd_flag, wake_flags);

	if (cpu >= 0 && !is_dst_allowed(p, cpu))
		cpu = -1;

	*new_cpu = cpu;
	TASK_AVD1_1(p) = cpu;
}

static void ems_hook_scheduler_tick(void *data, struct rq *rq)
{
	ems_tick(rq);
}

static void ems_hook_newidle_balance(void *data,
			struct rq *this_rq, struct rq_flags *rf,
			int *pulled_task, int *done)
{
	lb_newidle_balance(this_rq, rf, pulled_task, done);
}

static void ems_hook_enqueue_task(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	update_cpu_active_ratio(rq, p, EMS_PART_ENQUEUE);
}

static void ems_hook_dequeue_task(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	update_cpu_active_ratio(rq, p, EMS_PART_DEQUEUE);
}

static void ems_hook_can_migrate_task(void *data,
			struct task_struct *p, int dst_cpu, int *can_migrate)
{
	*can_migrate = ems_can_migrate_task(p, dst_cpu);
}

static void ems_hook_find_lowest_rq(void *data, struct task_struct *p,
			struct cpumask *local_cpu_mask, int *lowest_cpu)
{
#ifdef CONFIG_SCHED_USE_FLUID_RT
	*lowest_cpu = frt_find_lowest_rq(p);
#endif
}

static void ems_hook_wake_up_new_task(void *data, struct task_struct *p)
{
	update_cpu_active_ratio(task_rq(p), p, EMS_PART_WAKEUP_NEW);
}

static void ems_hook_cpu_cgroup_can_attach(void *data, struct cgroup_taskset *tset, int *retval)
{
	*retval = freqboost_can_attach(tset);
}

static void ems_hook_enqueue_task_fair(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	freqboost_enqueue_task(p, cpu_of(rq), flags);
	prio_pinning_enqueue_task(p, cpu_of(rq));
}

static void ems_hook_dequeue_task_fair(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	freqboost_dequeue_task(p, cpu_of(rq), flags);
	prio_pinning_dequeue_task(p, cpu_of(rq));
}

static void ems_hook_enqueue_task_rt(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	freqboost_enqueue_task(p, cpu_of(rq), flags);
}

static void ems_hook_dequeue_task_rt(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	freqboost_dequeue_task(p, cpu_of(rq), flags);
}

static void ems_hook_sysbusy_on_somac(void *data, int *done)
{
	*done = sysbusy_on_somac();
}

static void ems_hook_ecs_cpu_available(void *data, int cpu, struct task_struct *p, int *available)
{
	*available = ecs_cpu_available(cpu, p);
}

/******************************************************************************
 * hook for ftrace                                                            *
 ******************************************************************************/
static void ftrace_pelt_cfs_tp(void *data, struct cfs_rq *cfs_rq)
{
	trace_sched_load_cfs_rq(cfs_rq);
}

static void ftrace_pelt_rt_tp(void *data, struct rq *rq)
{
	trace_sched_load_rt_rq(rq);
}

static void ftrace_pelt_dl_tp(void *data, struct rq *rq)
{
	trace_sched_load_dl_rq(rq);
}

static void ftrace_pelt_irq_tp(void *data, struct rq *rq)
{
#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	trace_sched_load_irq(rq);
#endif
}

static void ftrace_pelt_se_tp(void *data, struct sched_entity *se)
{
	trace_sched_load_se(se);
}

static void ftrace_sched_overutilized_tp(void *data,
			struct root_domain *rd, bool overutilized)
{
	trace_sched_overutilized(overutilized);
}

int hook_init(void)
{
	int ret = 0;

	ret = register_trace_android_rvh_select_task_rq_fair(ems_hook_select_task_rq_fair, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_vh_scheduler_tick(ems_hook_scheduler_tick, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_sched_newidle_balance(ems_hook_newidle_balance, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_can_migrate_task(ems_hook_can_migrate_task, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_enqueue_task(ems_hook_enqueue_task, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_dequeue_task(ems_hook_dequeue_task, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_find_lowest_rq(ems_hook_find_lowest_rq, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_wake_up_new_task(ems_hook_wake_up_new_task, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_cpu_cgroup_can_attach(ems_hook_cpu_cgroup_can_attach, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_enqueue_task_fair(ems_hook_enqueue_task_fair, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_dequeue_task_fair(ems_hook_dequeue_task_fair, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_enqueue_task_rt(ems_hook_enqueue_task_rt, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_dequeue_task_rt(ems_hook_dequeue_task_rt, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_sysbusy_on_somac(ems_hook_sysbusy_on_somac, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_rvh_ecs_cpu_available(ems_hook_ecs_cpu_available, NULL);
	if (ret)
		return ret;

	ret = register_trace_pelt_cfs_tp(ftrace_pelt_cfs_tp, NULL);
	WARN_ON(ret);
	ret = register_trace_pelt_rt_tp(ftrace_pelt_rt_tp, NULL);
	WARN_ON(ret);
	ret = register_trace_pelt_dl_tp(ftrace_pelt_dl_tp, NULL);
	WARN_ON(ret);
	ret = register_trace_pelt_irq_tp(ftrace_pelt_irq_tp, NULL);
	WARN_ON(ret);
	ret = register_trace_pelt_se_tp(ftrace_pelt_se_tp, NULL);
	WARN_ON(ret);
	ret = register_trace_sched_overutilized_tp(ftrace_sched_overutilized_tp, NULL);
	WARN_ON(ret);

	return ret;
}

void hook_exit(void)
{
	/* Android Restricted Vendor Hooks cannot be unregistered */
}
