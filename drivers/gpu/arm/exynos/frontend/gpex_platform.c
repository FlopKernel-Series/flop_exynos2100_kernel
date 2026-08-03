/* SPDX-License-Identifier: GPL-2.0 */

/*
 * (C) COPYRIGHT 2021 Samsung Electronics Inc. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 */

#include <gpex_platform.h>
#include <gpex_utils.h>
#include <gpex_debug.h>
#include <gpex_pm.h>
#include <gpex_dvfs.h>
#include <gpex_qos.h>
#include <gpex_thermal.h>
#include <gpex_clock.h>
#include <gpex_ifpo.h>
#include <gpex_tsg.h>
#include <gpex_clboost.h>
#include <gpex_cmar_sched.h>
#include <gpexbe_devicetree.h>

#include <gpexbe_notifier.h>
#include <gpexbe_pm.h>
#include <gpexbe_clock.h>
#include <gpexbe_qos.h>
#include <gpexbe_bts.h>
#include <gpexbe_debug.h>
#include <gpexbe_utilization.h>
#include <gpexbe_llc_coherency.h>
#include <gpexbe_mem_usage.h>
#include <gpexbe_smc.h>
#include <gpex_gts.h>
#include <gpexwa_interactive_boost.h>

#include <runtime_test_runner.h>

#include <mali_redirect.h>

extern int gpu_dvfs_get_cur_clock(void);
extern int gpu_dvfs_get_clock(int level);
extern int gpu_dvfs_get_voltage(int clock);
extern int gpu_dvfs_get_step(void);
extern int gpu_dvfs_get_utilization(void);
extern int gpu_dvfs_get_max_freq(void);
extern int gpu_dvfs_get_min_freq(void);
extern int gpu_dvfs_get_max_cooling_freq(void);
extern int gpu_dvfs_get_max_locked_freq(void);
extern int gpu_dvfs_get_min_locked_freq(void);
extern int gpu_tmu_notifier(struct notifier_block *notifier, unsigned long event, void *v);
extern unsigned long exynos_stats_get_job_state_cnt(void);
extern uint32_t exynos_stats_get_gpu_table_size(void);
extern uint32_t *exynos_stats_get_gpu_freq_table(void);
extern ktime_t *exynos_stats_get_gpu_time_in_state(void);
extern int exynos_stats_get_gpu_max_lock(void);
extern int exynos_stats_get_gpu_min_lock(void);
extern ktime_t *exynos_stats_get_gpu_queued_job_time(void);
extern ktime_t exynos_stats_get_gpu_queued_last_updated(void);
extern int exynos_stats_get_gpu_polling_speed(void);
extern void exynos_stats_set_gpu_polling_speed(int polling_speed);
extern void exynos_migov_set_mode(int mode);
extern void exynos_migov_set_gpu_margin(int margin);
extern int register_frag_utils_change_notifier(struct notifier_block *nb);
extern int unregister_frag_utils_change_notifier(struct notifier_block *nb);
extern void gpu_register_out_data(void (*fn)(u64 *cnt));

static const struct mali_exports mali_exports_table = {
	.gpu_dvfs_get_cur_clock = gpu_dvfs_get_cur_clock,
	.gpu_dvfs_get_clock = gpu_dvfs_get_clock,
	.gpu_dvfs_get_voltage = gpu_dvfs_get_voltage,
	.gpu_dvfs_get_step = gpu_dvfs_get_step,
	.gpu_dvfs_get_utilization = gpu_dvfs_get_utilization,
	.gpu_dvfs_get_max_freq = gpu_dvfs_get_max_freq,
	.gpu_dvfs_get_min_freq = gpu_dvfs_get_min_freq,
	.gpu_dvfs_get_max_cooling_freq = gpu_dvfs_get_max_cooling_freq,
	.gpu_dvfs_get_max_locked_freq = gpu_dvfs_get_max_locked_freq,
	.gpu_dvfs_get_min_locked_freq = gpu_dvfs_get_min_locked_freq,
	.gpu_tmu_notifier = gpu_tmu_notifier,
	.exynos_stats_get_job_state_cnt = exynos_stats_get_job_state_cnt,
	.exynos_stats_get_gpu_table_size = exynos_stats_get_gpu_table_size,
	.exynos_stats_get_gpu_freq_table = exynos_stats_get_gpu_freq_table,
	.exynos_stats_get_gpu_time_in_state = exynos_stats_get_gpu_time_in_state,
	.exynos_stats_get_gpu_max_lock = exynos_stats_get_gpu_max_lock,
	.exynos_stats_get_gpu_min_lock = exynos_stats_get_gpu_min_lock,
	.exynos_stats_get_gpu_queued_job_time = exynos_stats_get_gpu_queued_job_time,
	.exynos_stats_get_gpu_queued_last_updated = exynos_stats_get_gpu_queued_last_updated,
	.exynos_stats_get_gpu_polling_speed = exynos_stats_get_gpu_polling_speed,
	.exynos_stats_set_gpu_polling_speed = exynos_stats_set_gpu_polling_speed,
	.exynos_migov_set_mode = exynos_migov_set_mode,
	.exynos_migov_set_gpu_margin = exynos_migov_set_gpu_margin,
	.register_frag_utils_change_notifier = register_frag_utils_change_notifier,
	.unregister_frag_utils_change_notifier = unregister_frag_utils_change_notifier,
	.gpu_register_out_data = gpu_register_out_data,
};

int gpex_platform_init(struct device **dev)
{
	/* TODO: check return value */
	/* TODO: becareful with order */
	gpexbe_devicetree_init(*dev);
	gpex_utils_init(dev);
	gpex_debug_init(dev);

	gpexbe_utilization_init(dev);
	gpex_clboost_init();

	gpex_gts_init(dev);

	gpexbe_debug_init();

	gpex_thermal_init();
	gpexbe_notifier_init();

	gpexbe_llc_coherency_init(dev);

	gpexbe_pm_init();
	gpexbe_clock_init();
	gpex_pm_init();
	gpex_clock_init(dev);

	gpexbe_qos_init();
	gpexbe_bts_init();
	gpex_qos_init();

	gpex_ifpo_init();
	gpex_dvfs_init(dev);
	gpexbe_smc_init();
	gpex_cmar_sched_init();
	gpex_tsg_init(dev);

	gpexbe_mem_usage_init();

	gpexwa_interactive_boost_init();

	runtime_test_runner_init();

	gpex_utils_sysfs_kobject_files_create();
	gpex_utils_sysfs_device_files_create();

	mali_register_exports(&mali_exports_table);

	return 0;
}

void gpex_platform_term(void)
{
	runtime_test_runner_term();

	gpexbe_mem_usage_term();

	gpexwa_interactive_boost_term();

	gpex_tsg_term();
	gpex_cmar_sched_term();
	gpexbe_smc_term();
	gpex_ifpo_term();

	gpex_pm_term();
	gpexbe_pm_term();

	gpex_qos_term();
	gpexbe_qos_term();
	gpexbe_bts_term();

	/* DVFS stuff */
	gpex_dvfs_term();

	gpex_clock_term();
	gpexbe_clock_term();

	gpexbe_llc_coherency_term();

	gpexbe_notifier_term();
	gpex_thermal_term();

	gpexbe_debug_term();
	gpex_gts_term();

	gpex_clboost_term();
	gpexbe_utilization_term();
	gpex_utils_term();
}
