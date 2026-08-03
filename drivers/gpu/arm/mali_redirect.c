// SPDX-License-Identifier: GPL-2.0
/*
 * Mali KMD external symbol redirector
 *
 * Exports symbols that external kernel modules resolve against,
 * forwarding calls to whichever Mali KMD module is active at runtime.
 * The active Mali module registers its exports struct during probe.
 */
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/notifier.h>
#include <linux/types.h>
#include "mali_redirect.h"

static const struct mali_exports *mali_exp;

void mali_register_exports(const struct mali_exports *exp)
{
	mali_exp = exp;
}
EXPORT_SYMBOL(mali_register_exports);

/*
 * gpu_dvfs API
 */
int gpu_dvfs_get_cur_clock(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_cur_clock)
		return mali_exp->gpu_dvfs_get_cur_clock();
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_cur_clock);

int gpu_dvfs_get_clock(int level)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_clock)
		return mali_exp->gpu_dvfs_get_clock(level);
	return -1;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_clock);

int gpu_dvfs_get_voltage(int clock)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_voltage)
		return mali_exp->gpu_dvfs_get_voltage(clock);
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_voltage);

int gpu_dvfs_get_step(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_step)
		return mali_exp->gpu_dvfs_get_step();
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_step);

int gpu_dvfs_get_utilization(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_utilization)
		return mali_exp->gpu_dvfs_get_utilization();
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_utilization);

int gpu_dvfs_get_max_freq(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_max_freq)
		return mali_exp->gpu_dvfs_get_max_freq();
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_max_freq);

int gpu_dvfs_get_min_freq(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_min_freq)
		return mali_exp->gpu_dvfs_get_min_freq();
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_min_freq);

int gpu_dvfs_get_max_cooling_freq(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_max_cooling_freq)
		return mali_exp->gpu_dvfs_get_max_cooling_freq();
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_dvfs_get_max_cooling_freq);

int gpu_dvfs_get_max_locked_freq(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_max_locked_freq)
		return mali_exp->gpu_dvfs_get_max_locked_freq();
	return 0;
}
EXPORT_SYMBOL(gpu_dvfs_get_max_locked_freq);

int gpu_dvfs_get_min_locked_freq(void)
{
	if (mali_exp && mali_exp->gpu_dvfs_get_min_locked_freq)
		return mali_exp->gpu_dvfs_get_min_locked_freq();
	return 0;
}
EXPORT_SYMBOL(gpu_dvfs_get_min_locked_freq);

/*
 * GPU thermal/tmu API
 */
int gpu_tmu_notifier(struct notifier_block *notifier, unsigned long event, void *v)
{
	if (mali_exp && mali_exp->gpu_tmu_notifier)
		return mali_exp->gpu_tmu_notifier(notifier, event, v);
	return 0;
}
EXPORT_SYMBOL_GPL(gpu_tmu_notifier);

/*
 * TSG / GPU stats API
 */
unsigned long exynos_stats_get_job_state_cnt(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_job_state_cnt)
		return mali_exp->exynos_stats_get_job_state_cnt();
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_job_state_cnt);

uint32_t exynos_stats_get_gpu_table_size(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_table_size)
		return mali_exp->exynos_stats_get_gpu_table_size();
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_table_size);

uint32_t *exynos_stats_get_gpu_freq_table(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_freq_table)
		return mali_exp->exynos_stats_get_gpu_freq_table();
	return NULL;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_freq_table);

ktime_t *exynos_stats_get_gpu_time_in_state(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_time_in_state)
		return mali_exp->exynos_stats_get_gpu_time_in_state();
	return NULL;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_time_in_state);

int exynos_stats_get_gpu_max_lock(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_max_lock)
		return mali_exp->exynos_stats_get_gpu_max_lock();
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_max_lock);

int exynos_stats_get_gpu_min_lock(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_min_lock)
		return mali_exp->exynos_stats_get_gpu_min_lock();
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_min_lock);

ktime_t *exynos_stats_get_gpu_queued_job_time(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_queued_job_time)
		return mali_exp->exynos_stats_get_gpu_queued_job_time();
	return NULL;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_queued_job_time);

ktime_t exynos_stats_get_gpu_queued_last_updated(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_queued_last_updated)
		return mali_exp->exynos_stats_get_gpu_queued_last_updated();
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_queued_last_updated);

int exynos_stats_get_gpu_polling_speed(void)
{
	if (mali_exp && mali_exp->exynos_stats_get_gpu_polling_speed)
		return mali_exp->exynos_stats_get_gpu_polling_speed();
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_stats_get_gpu_polling_speed);

void exynos_stats_set_gpu_polling_speed(int polling_speed)
{
	if (mali_exp && mali_exp->exynos_stats_set_gpu_polling_speed)
		mali_exp->exynos_stats_set_gpu_polling_speed(polling_speed);
}
EXPORT_SYMBOL_GPL(exynos_stats_set_gpu_polling_speed);

/*
 * Migov helpers
 */
void exynos_migov_set_mode(int mode)
{
	if (mali_exp && mali_exp->exynos_migov_set_mode)
		mali_exp->exynos_migov_set_mode(mode);
}
EXPORT_SYMBOL_GPL(exynos_migov_set_mode);

void exynos_migov_set_gpu_margin(int margin)
{
	if (mali_exp && mali_exp->exynos_migov_set_gpu_margin)
		mali_exp->exynos_migov_set_gpu_margin(margin);
}
EXPORT_SYMBOL_GPL(exynos_migov_set_gpu_margin);

/*
 * Frag utils change notifier
 */
int register_frag_utils_change_notifier(struct notifier_block *nb)
{
	if (mali_exp && mali_exp->register_frag_utils_change_notifier)
		return mali_exp->register_frag_utils_change_notifier(nb);
	return 0;
}
EXPORT_SYMBOL_GPL(register_frag_utils_change_notifier);

int unregister_frag_utils_change_notifier(struct notifier_block *nb)
{
	if (mali_exp && mali_exp->unregister_frag_utils_change_notifier)
		return mali_exp->unregister_frag_utils_change_notifier(nb);
	return 0;
}
EXPORT_SYMBOL_GPL(unregister_frag_utils_change_notifier);

/*
 * GTS
 */
void gpu_register_out_data(void (*fn)(u64 *cnt))
{
	if (mali_exp && mali_exp->gpu_register_out_data)
		mali_exp->gpu_register_out_data(fn);
}
EXPORT_SYMBOL_GPL(gpu_register_out_data);