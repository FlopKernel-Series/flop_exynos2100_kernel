/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mali KMD external symbol redirector
 *
 * Exports generic names that external kernel consumers (exynos-migov,
 * exynos-gpu-profiler, gpu_cooling) resolve against. The active Mali
 * version module registers its function table via mali_register_exports()
 * during probe; calls are forwarded to the versioned implementation
 * at runtime through the function pointer table.
 */
#ifndef _MALI_REDIRECT_H_
#define _MALI_REDIRECT_H_

#include <linux/types.h>
#include <linux/ktime.h>

/* Forward declarations for types used in the exports struct */
struct notifier_block;

/*
 * Mali export function table filled in at registration time.
 */
struct mali_exports {
	/* gpu_dvfs API */
	int (*gpu_dvfs_get_cur_clock)(void);
	int (*gpu_dvfs_get_clock)(int level);
	int (*gpu_dvfs_get_voltage)(int clock);
	int (*gpu_dvfs_get_step)(void);
	int (*gpu_dvfs_get_utilization)(void);
	int (*gpu_dvfs_get_max_freq)(void);
	int (*gpu_dvfs_get_min_freq)(void);
	int (*gpu_dvfs_get_max_cooling_freq)(void);
	int (*gpu_dvfs_get_max_locked_freq)(void);
	int (*gpu_dvfs_get_min_locked_freq)(void);

	/* GPU thermal/tmu */
	int (*gpu_tmu_notifier)(struct notifier_block *notifier,
				unsigned long event, void *v);

	/* TSG / GPU stats API */
	unsigned long (*exynos_stats_get_job_state_cnt)(void);
	uint32_t (*exynos_stats_get_gpu_table_size)(void);
	uint32_t *(*exynos_stats_get_gpu_freq_table)(void);
	ktime_t *(*exynos_stats_get_gpu_time_in_state)(void);
	int (*exynos_stats_get_gpu_max_lock)(void);
	int (*exynos_stats_get_gpu_min_lock)(void);
	ktime_t *(*exynos_stats_get_gpu_queued_job_time)(void);
	ktime_t (*exynos_stats_get_gpu_queued_last_updated)(void);
	int (*exynos_stats_get_gpu_polling_speed)(void);
	void (*exynos_stats_set_gpu_polling_speed)(int polling_speed);

	/* Migov helpers */
	void (*exynos_migov_set_mode)(int mode);
	void (*exynos_migov_set_gpu_margin)(int margin);

	/* Frag utils change notifier */
	int (*register_frag_utils_change_notifier)(struct notifier_block *nb);
	int (*unregister_frag_utils_change_notifier)(struct notifier_block *nb);

	/* GTS */
	void (*gpu_register_out_data)(void (*fn)(u64 *cnt));
};

/*
 * Register the active Mali module's export table.
 * Called by the Mali module after probe initialization succeeds.
 * Passing NULL unregisters.
 */
void mali_register_exports(const struct mali_exports *exp);

#endif /* _MALI_REDIRECT_H_ */