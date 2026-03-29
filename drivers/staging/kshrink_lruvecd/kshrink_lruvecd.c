// SPDX-License-Identifier: GPL-2.0-only
/*
 * Direct non-GKI port of Oplus/Xiaomi asynchronous lruvec reclaim.
 */
#define pr_fmt(fmt) "kshrink_lruvecd: " fmt

#include <linux/cpufreq.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/kshrink_lruvecd.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/page_ext.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/wait.h>

#define KSHRINK_LRUVECD_HIGH		0x1000
#define KSHRINK_LRUVECD_DELAY_BIT	8
#define KSHRINK_LRUVECD_SKIP_BIT	9

static LIST_HEAD(lru_inactive);
static struct task_struct *shrink_lruvec_tsk;
static bool kshrink_lruvecd_setup;
static atomic_t shrink_lruvec_runnable = ATOMIC_INIT(0);
static unsigned long shrink_lruvec_pages;
static unsigned long shrink_lruvec_pages_max;
static unsigned long shrink_lruvec_handle_pages;
static wait_queue_head_t shrink_lruvec_wait;
static spinlock_t lru_inactive_lock;

static bool need_kshrink_lruvecd_page_ext(void)
{
	return true;
}

struct page_ext_operations kshrink_lruvecd_page_ext_ops = {
	.need = need_kshrink_lruvecd_page_ext,
};

static inline bool process_is_shrink_lruvecd(struct task_struct *tsk)
{
	return shrink_lruvec_tsk && shrink_lruvec_tsk->pid == tsk->pid;
}

static inline struct page_ext *kshrink_lruvecd_lookup_page_ext(struct page *page)
{
	return lookup_page_ext(page);
}

static inline void kshrink_lruvecd_set_delay(struct page *page)
{
	struct page_ext *page_ext = kshrink_lruvecd_lookup_page_ext(page);

	if (unlikely(!page_ext))
		return;

	set_bit(KSHRINK_LRUVECD_DELAY_BIT, &page_ext->flags);
}

static inline void kshrink_lruvecd_clear_delay(struct page *page)
{
	struct page_ext *page_ext = kshrink_lruvecd_lookup_page_ext(page);

	if (unlikely(!page_ext))
		return;

	clear_bit(KSHRINK_LRUVECD_DELAY_BIT, &page_ext->flags);
}

static inline bool kshrink_lruvecd_test_clear_delay(struct page *page)
{
	struct page_ext *page_ext = kshrink_lruvecd_lookup_page_ext(page);

	if (unlikely(!page_ext))
		return false;

	return test_and_clear_bit(KSHRINK_LRUVECD_DELAY_BIT, &page_ext->flags);
}

static inline void kshrink_lruvecd_set_skipped(struct page *page)
{
	struct page_ext *page_ext = kshrink_lruvecd_lookup_page_ext(page);

	if (unlikely(!page_ext))
		return;

	set_bit(KSHRINK_LRUVECD_SKIP_BIT, &page_ext->flags);
}

static inline void kshrink_lruvecd_clear_skipped(struct page *page)
{
	struct page_ext *page_ext = kshrink_lruvecd_lookup_page_ext(page);

	if (unlikely(!page_ext))
		return;

	clear_bit(KSHRINK_LRUVECD_SKIP_BIT, &page_ext->flags);
}

static inline bool kshrink_lruvecd_test_clear_skipped(struct page *page)
{
	struct page_ext *page_ext = kshrink_lruvecd_lookup_page_ext(page);

	if (unlikely(!page_ext))
		return false;

	return test_and_clear_bit(KSHRINK_LRUVECD_SKIP_BIT, &page_ext->flags);
}

static inline bool kshrink_lruvecd_skipped(struct page *page)
{
	struct page_ext *page_ext = kshrink_lruvecd_lookup_page_ext(page);

	if (unlikely(!page_ext))
		return false;

	return test_bit(KSHRINK_LRUVECD_SKIP_BIT, &page_ext->flags);
}

static void add_to_lruvecd_inactive_list(struct page *page)
{
	list_move(&page->lru, &lru_inactive);
	shrink_lruvec_pages += hpage_nr_pages(page);
	if (shrink_lruvec_pages > shrink_lruvec_pages_max)
		shrink_lruvec_pages_max = shrink_lruvec_pages;
}

static void bind_kshrink_lruvecd_cpus(void)
{
	struct cpumask mask;
	struct cpumask max_policy_cpus;
	pg_data_t *pgdat = NODE_DATA(0);
	unsigned int cpu;
	unsigned int max_freq = 0;
	bool found_policy = false;
	static bool bound_once;

	if (bound_once || !kshrink_lruvecd_setup)
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
		set_cpus_allowed_ptr(shrink_lruvec_tsk, &mask);
		bound_once = true;
	}
}

static int shrink_lruvecd(void *unused)
{
	LIST_HEAD(tmp_lru_inactive);
	struct page *page, *next;

	current->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	set_freezable();

	while (!kthread_should_stop()) {
		wait_event_freezable(shrink_lruvec_wait,
				     kthread_should_stop() ||
				     atomic_read(&shrink_lruvec_runnable) == 1);

		if (kthread_should_stop())
			break;

		bind_kshrink_lruvecd_cpus();

retry_reclaim:
		spin_lock_irq(&lru_inactive_lock);
		if (list_empty(&lru_inactive)) {
			spin_unlock_irq(&lru_inactive_lock);
			atomic_set(&shrink_lruvec_runnable, 0);
			continue;
		}

		list_for_each_entry_safe(page, next, &lru_inactive, lru) {
			list_move(&page->lru, &tmp_lru_inactive);
			shrink_lruvec_pages -= hpage_nr_pages(page);
			shrink_lruvec_handle_pages += hpage_nr_pages(page);
		}
		spin_unlock_irq(&lru_inactive_lock);

		reclaim_pages(&tmp_lru_inactive);
		goto retry_reclaim;
	}

	current->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD);

	return 0;
}

void kshrink_lruvecd_page_trylock_set(struct page *page)
{
	if (unlikely(!kshrink_lruvecd_setup))
		return;

	kshrink_lruvecd_clear_skipped(page);

	if (unlikely(process_is_shrink_lruvecd(current))) {
		kshrink_lruvecd_clear_delay(page);
		return;
	}

	kshrink_lruvecd_set_delay(page);
}

void kshrink_lruvecd_page_trylock_clear(struct page *page)
{
	kshrink_lruvecd_clear_delay(page);
	kshrink_lruvecd_clear_skipped(page);
}

bool kshrink_lruvecd_do_page_trylock(struct page *page,
				     struct rw_semaphore *sem,
				     bool *got_lock)
{
	if (got_lock)
		*got_lock = false;

	if (unlikely(!kshrink_lruvecd_setup))
		return false;

	if (!kshrink_lruvecd_test_clear_delay(page))
		return false;

	if (!sem) {
		kshrink_lruvecd_set_skipped(page);
		return true;
	}

	if (down_read_trylock(sem)) {
		if (got_lock)
			*got_lock = true;
	} else {
		kshrink_lruvecd_set_skipped(page);
	}

	return true;
}

bool kshrink_lruvecd_page_trylock_get_result(struct page *page)
{
	bool trylock_fail = false;

	kshrink_lruvecd_clear_delay(page);

	if (unlikely(!kshrink_lruvecd_setup) ||
	    unlikely(process_is_shrink_lruvecd(current)))
		return false;

	if (kshrink_lruvecd_skipped(page))
		trylock_fail = true;

	return trylock_fail;
}

void kshrink_lruvecd_handle_failed_page_trylock(struct list_head *page_list)
{
	struct page *page, *next;
	bool pages_should_reclaim = false;
	bool shrink_lruvecd_is_full = false;
	LIST_HEAD(tmp_lru_inactive);

	if (unlikely(!kshrink_lruvecd_setup) || list_empty(page_list))
		return;

	if (unlikely(shrink_lruvec_pages > KSHRINK_LRUVECD_HIGH))
		shrink_lruvecd_is_full = true;

	list_for_each_entry_safe(page, next, page_list, lru) {
		kshrink_lruvecd_clear_delay(page);
		if (unlikely(kshrink_lruvecd_test_clear_skipped(page))) {
			ClearPageActive(page);
			if (!shrink_lruvecd_is_full)
				list_move(&page->lru, &tmp_lru_inactive);
		}
	}

	if (!list_empty(&tmp_lru_inactive)) {
		spin_lock_irq(&lru_inactive_lock);
		list_for_each_entry_safe(page, next, &tmp_lru_inactive, lru) {
			pages_should_reclaim = true;
			add_to_lruvecd_inactive_list(page);
		}
		spin_unlock_irq(&lru_inactive_lock);
	}

	if (shrink_lruvecd_is_full || !pages_should_reclaim)
		return;

	if (atomic_cmpxchg(&shrink_lruvec_runnable, 0, 1) == 0)
		wake_up_interruptible(&shrink_lruvec_wait);
}

static int kshrink_lruvecd_status_show(struct seq_file *m, void *unused)
{
	seq_printf(m,
		   "kshrink_lruvecd_setup: %s\n"
		   "shrink_lruvec_pages: %lu\n"
		   "shrink_lruvec_handle_pages: %lu\n"
		   "shrink_lruvec_pages_max: %lu\n\n",
		   kshrink_lruvecd_setup ? "enable" : "disable",
		   shrink_lruvec_pages,
		   shrink_lruvec_handle_pages,
		   shrink_lruvec_pages_max);
	return 0;
}

static int __init kshrink_lruvecd_init(void)
{
	init_waitqueue_head(&shrink_lruvec_wait);
	spin_lock_init(&lru_inactive_lock);

	shrink_lruvec_tsk = kthread_run(shrink_lruvecd, NULL, "kshrink_lruvecd");
	if (IS_ERR(shrink_lruvec_tsk)) {
		int ret = PTR_ERR(shrink_lruvec_tsk);

		shrink_lruvec_tsk = NULL;
		pr_err("failed to start thread: %d\n", ret);
		return ret;
	}

	proc_create_single("kshrink_lruvecd_status", 0, NULL,
			   kshrink_lruvecd_status_show);
	kshrink_lruvecd_setup = true;
	pr_info("enabled\n");

	return 0;
}

module_init(kshrink_lruvecd_init);

MODULE_LICENSE("GPL v2");
