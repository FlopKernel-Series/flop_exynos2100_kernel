/*
 * exynos_amb_control.h - Samsung ambient thermal control helpers
 *
 * Copyright (C) 2021 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _EXYNOS_AMB_CONTROL_H
#define _EXYNOS_AMB_CONTROL_H

#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/thermal.h>
#include <soc/samsung/exynos_pm_qos.h>

struct exynos_tmu_data;
struct task_struct;

#define AMB_TZ_NUM	(5)
#define AMB_DEFAULT_SAMPLING_RATE_MS	1000
#define AMB_HIGH_SAMPLING_RATE_MS	100
#define AMB_INCREASE_SAMPLING_TEMP_MC	55000
#define AMB_DECREASE_SAMPLING_TEMP_MC	50000

enum tz_id {
	AMB_TZ_BIG,
	AMB_TZ_MID,
	AMB_TZ_LIT,
	AMB_TZ_GPU,
	AMB_TZ_ISP,
};

struct ambient_thermal_zone_data {
	int hotplug_in_threshold;
	int hotplug_out_threshold;
	bool is_cpu_hotplugged_out;
	char cpuhp_name[THERMAL_NAME_LENGTH + 1];
	int hotplug_disabled;
	struct cpumask cpu_domain;
	struct thermal_zone_device *tzd;
	unsigned int emg_control_temp;
	unsigned int normal_control_temp;
	unsigned int use_pi_thermal;
	unsigned int increase_trip_temp;
	unsigned int decrease_trip_temp;
};

struct ambient_thermal_zone {
	struct thermal_zone_device *amb_tzd;

	struct ambient_thermal_zone_data amb_data[AMB_TZ_NUM];
	struct exynos_pm_qos_request mif_max_pm_qos;
	struct exynos_pm_qos_request mif_min_pm_qos;
	struct mutex lock;
	struct kthread_delayed_work dwork;
	struct kthread_worker worker;
	struct notifier_block pm_nb;
	struct task_struct *thread;
	unsigned int registered_mask;
	unsigned int default_sampling_rate;
	unsigned int high_sampling_rate;
	unsigned int increase_sampling_temp;
	unsigned int decrease_sampling_temp;
	unsigned int current_sampling_rate;
	unsigned int status;
	unsigned long next_update_jiffies;
	bool mif_throttled;
	bool s2d_disabled;
	bool status_valid;
	bool in_suspend;
};

extern struct ambient_thermal_zone *amb_tz;
extern unsigned int hotplug_threshold;
extern unsigned int emergency_control_temp;
extern unsigned int emergency_control_threshold;
extern int hotplug_out_big;
extern int hotplug_in_big;
extern int hotplug_out_mid;
extern int hotplug_in_mid;
extern int hotplug_out_lit;
extern int hotplug_in_lit;

int get_ambient_temp(void);
unsigned int exynos_amb_control_get_status(void);
void exynos_amb_control_kick(unsigned int delay_ms);
void exynos_amb_control_register_tmu(struct exynos_tmu_data *data);
void exynos_amb_control_unregister_tmu(struct exynos_tmu_data *data);

#endif /* _EXYNOS_AMB_CONTROL_H */
