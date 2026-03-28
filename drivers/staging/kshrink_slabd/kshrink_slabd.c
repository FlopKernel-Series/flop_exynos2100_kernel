// SPDX-License-Identifier: GPL-2.0-only
/*
 * Non-GKI direct port of Xiaomi's asynchronous slab shrinking helper.
 */
#define pr_fmt(fmt) "kshrink_slabd: " fmt

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kshrink_slabd.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/wait.h>

#define KSHRINK_SLABD_NAME "kshrink_slabd"

extern unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 int priority);

struct kshrink_slabd_params {
	struct mem_cgroup *memcg;
	gfp_t gfp_mask;
	atomic_t runnable;
	int nid;
	int priority;
};

static struct task_struct *kshrink_slabd_tsk;
static bool kshrink_slabd_setup;
static wait_queue_head_t kshrink_slabd_wait;
static DEFINE_SPINLOCK(kshrink_slabd_lock);
static struct kshrink_slabd_params kshrink_slabd = {
	.runnable = ATOMIC_INIT(0),
};

static inline bool is_kshrink_slabd_task(struct task_struct *tsk)
{
	return kshrink_slabd_tsk && tsk->pid == kshrink_slabd_tsk->pid;
}

static bool wakeup_kshrink_slabd(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg, int priority)
{
	unsigned long flags;

	if (memcg && !mem_cgroup_is_root(memcg) &&
	    !css_tryget_online(&memcg->css))
		return false;

	spin_lock_irqsave(&kshrink_slabd_lock, flags);
	if (atomic_read(&kshrink_slabd.runnable) == 1) {
		spin_unlock_irqrestore(&kshrink_slabd_lock, flags);
		if (memcg && !mem_cgroup_is_root(memcg))
			mem_cgroup_put(memcg);
		return false;
	}

	kshrink_slabd.gfp_mask = gfp_mask;
	kshrink_slabd.nid = nid;
	kshrink_slabd.memcg = memcg;
	kshrink_slabd.priority = priority;
	atomic_set(&kshrink_slabd.runnable, 1);
	spin_unlock_irqrestore(&kshrink_slabd_lock, flags);

	wake_up_interruptible(&kshrink_slabd_wait);

	return true;
}

static void bind_kshrink_slabd_cpus(void)
{
	struct cpumask mask;
	struct cpumask max_policy_cpus;
	pg_data_t *pgdat = NODE_DATA(0);
	unsigned int cpu;
	unsigned int max_freq = 0;
	bool found_policy = false;
	static bool bound_once;

	if (bound_once)
		return;

	cpumask_clear(&max_policy_cpus);

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;

		if (!found_policy || policy->cpuinfo.max_freq >= max_freq) {
			max_freq = policy->cpuinfo.max_freq;
			cpumask_copy(&max_policy_cpus, policy->related_cpus);
			found_policy = true;
		}

		cpufreq_cpu_put(policy);
	}

	if (!found_policy)
		return;

	cpumask_copy(&mask, cpumask_of_node(pgdat->node_id));
	cpumask_andnot(&mask, &mask, &max_policy_cpus);

	if (!cpumask_empty(&mask)) {
		set_cpus_allowed_ptr(kshrink_slabd_tsk, &mask);
		bound_once = true;
	}
}

static int kshrink_slabd_thread(void *unused)
{
	struct mem_cgroup *memcg;
	gfp_t gfp_mask;
	int nid;
	int priority;
	unsigned long flags;

	current->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	while (!kthread_should_stop()) {
		wait_event_freezable(kshrink_slabd_wait,
				     kthread_should_stop() ||
				     atomic_read(&kshrink_slabd.runnable) == 1);

		if (kthread_should_stop())
			break;

		bind_kshrink_slabd_cpus();

		spin_lock_irqsave(&kshrink_slabd_lock, flags);
		nid = kshrink_slabd.nid;
		gfp_mask = kshrink_slabd.gfp_mask;
		priority = kshrink_slabd.priority;
		memcg = kshrink_slabd.memcg;
		atomic_set(&kshrink_slabd.runnable, 0);
		spin_unlock_irqrestore(&kshrink_slabd_lock, flags);

		shrink_slab(gfp_mask, nid, memcg, priority);
		if (memcg && !mem_cgroup_is_root(memcg))
			mem_cgroup_put(memcg);
	}

	current->flags &= ~(PF_MEMALLOC | PF_KSWAPD);

	return 0;
}

bool kshrink_slabd_bypass(gfp_t gfp_mask, int nid,
			  struct mem_cgroup *memcg, int priority)
{
	static unsigned long prev_jiffies;
	unsigned long curr_jiffies;
	unsigned long diff_jiffies;

	if (unlikely(!kshrink_slabd_setup))
		return false;

	curr_jiffies = jiffies;
	diff_jiffies = curr_jiffies - READ_ONCE(prev_jiffies);
	WRITE_ONCE(prev_jiffies, curr_jiffies);

	if (is_kshrink_slabd_task(current) || diff_jiffies < HZ)
		return false;

	return wakeup_kshrink_slabd(gfp_mask, nid, memcg, priority);
}

static int __init kshrink_slabd_init(void)
{
	init_waitqueue_head(&kshrink_slabd_wait);

	kshrink_slabd_tsk = kthread_run(kshrink_slabd_thread, NULL,
					KSHRINK_SLABD_NAME);
	if (IS_ERR(kshrink_slabd_tsk)) {
		int ret = PTR_ERR(kshrink_slabd_tsk);

		kshrink_slabd_tsk = NULL;
		pr_err("failed to start thread: %d\n", ret);
		return ret;
	}

	kshrink_slabd_setup = true;
	pr_info("enabled\n");

	return 0;
}

module_init(kshrink_slabd_init);

MODULE_LICENSE("GPL v2");
