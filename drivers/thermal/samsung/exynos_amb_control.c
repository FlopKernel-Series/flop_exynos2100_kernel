/*
 * exynos_amb_control.c - Samsung ambient thermal control helpers
 *
 * Copyright (C) 2021 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/threads.h>
#include <soc/samsung/exynos-mcinfo.h>
#include <uapi/linux/sched/types.h>

#include "exynos_amb_control.h"
#include "exynos_tmu.h"

struct ambient_thermal_zone *amb_tz;
unsigned int hotplug_threshold = 75;
unsigned int emergency_control_temp = 75;
unsigned int emergency_control_threshold = 60;

int hotplug_out_big = 65;
int hotplug_in_big = 60;
int hotplug_out_mid = 65;
int hotplug_in_mid = 60;
int hotplug_out_lit = 75;
int hotplug_in_lit = 65;

#define EXYNOS_AMB_TEMP_MAX_MC	(250 * MCELSIUS)

static int exynos_amb_offset_to_mc(int base_temp, int offset)
{
	int temp = base_temp + offset;

	if (temp < 0)
		return 0;
	if (temp > EXYNOS_AMB_TEMP_MAX_MC)
		return EXYNOS_AMB_TEMP_MAX_MC;

	return temp;
}

static void exynos_amb_control_set_polling(struct ambient_thermal_zone *amb,
					   unsigned int delay_ms)
{
	if (!amb || !amb->thread || amb->in_suspend)
		return;

	kthread_mod_delayed_work(&amb->worker, &amb->dwork,
				 msecs_to_jiffies(delay_ms));
}

static void exynos_amb_control_lookup_zone(struct ambient_thermal_zone *amb)
{
	if (!amb || amb->amb_tzd)
		return;

	amb->amb_tzd = thermal_zone_get_zone_by_name("battery");
	if (PTR_ERR(amb->amb_tzd) == -ENODEV)
		amb->amb_tzd = NULL;
	else if (IS_ERR(amb->amb_tzd))
		amb->amb_tzd = NULL;
}

int get_ambient_temp(void)
{
	int temp;
	int ret;

	if (!amb_tz)
		return -ENODEV;

	if (!amb_tz->amb_tzd)
		return -ENODEV;

	ret = thermal_zone_get_temp(amb_tz->amb_tzd, &temp);
	if (ret) {
		if (ret != -EAGAIN)
			pr_warn("[%s] failed to read ambient thermal zone (%d)\n",
				__func__, ret);
		return -EINVAL;
	}

	return temp;
}

static void exynos_amb_control_apply(struct ambient_thermal_zone *amb, int temp)
{
	int i;
	struct cpumask mask;
	int status;

	for (i = 0; i < 3; i++) {
		if (amb->amb_data[i].hotplug_disabled)
			continue;

		if (amb->amb_data[i].is_cpu_hotplugged_out) {
			if (temp < amb->amb_data[i].hotplug_in_threshold) {
				exynos_cpuhp_request(amb->amb_data[i].cpuhp_name,
						     *cpu_possible_mask);
				amb->amb_data[i].is_cpu_hotplugged_out = false;
			}
		} else {
			if (temp >= amb->amb_data[i].hotplug_out_threshold) {
				amb->amb_data[i].is_cpu_hotplugged_out = true;
				cpumask_andnot(&mask, cpu_possible_mask,
					       &amb->amb_data[i].cpu_domain);
				exynos_cpuhp_request(amb->amb_data[i].cpuhp_name,
						     mask);
			}
		}
	}

	if (temp > hotplug_threshold * 1000) {
		if (!amb->mif_throttled) {
			exynos_pm_qos_update_request(&amb->mif_max_pm_qos, 2028000);
			amb->mif_throttled = true;
		}
		if (!amb->s2d_disabled) {
			adv_tracer_s2d_set_enable(0);
			amb->s2d_disabled = true;
		}
	} else {
		if (amb->s2d_disabled) {
			adv_tracer_s2d_set_enable(1);
			amb->s2d_disabled = false;
		}
		if (amb->mif_throttled) {
			exynos_pm_qos_update_request(&amb->mif_max_pm_qos,
						     PM_QOS_BUS_THROUGHPUT_MAX_DEFAULT_VALUE);
			amb->mif_throttled = false;
		}
	}

	if (temp > emergency_control_threshold * 1000) {
		for (i = 0; i < AMB_TZ_NUM; i++) {
			if (amb->amb_data[i].use_pi_thermal) {
				amb->amb_data[i].tzd->ops->set_trip_temp(
					amb->amb_data[i].tzd, 2,
					amb->amb_data[i].emg_control_temp);
			} else if (amb->amb_data[i].increase_trip_temp) {
				amb->amb_data[i].tzd->ops->set_trip_temp(
					amb->amb_data[i].tzd, 1,
					amb->amb_data[i].decrease_trip_temp);
			}
		}
	} else if (temp <= (emergency_control_threshold - 5) * 1000) {
		for (i = 0; i < AMB_TZ_NUM; i++) {
			if (amb->amb_data[i].use_pi_thermal) {
				amb->amb_data[i].tzd->ops->set_trip_temp(
					amb->amb_data[i].tzd, 2,
					amb->amb_data[i].normal_control_temp);
			} else if (amb->amb_data[i].increase_trip_temp) {
				amb->amb_data[i].tzd->ops->set_trip_temp(
					amb->amb_data[i].tzd, 1,
					amb->amb_data[i].increase_trip_temp);
			}
		}
	}

	status = amb->amb_data[0].is_cpu_hotplugged_out ||
		 amb->amb_data[1].is_cpu_hotplugged_out ||
		 amb->amb_data[2].is_cpu_hotplugged_out;

	if (temp > amb->increase_sampling_temp)
		amb->current_sampling_rate = amb->high_sampling_rate;
	else if (temp <= amb->decrease_sampling_temp)
		amb->current_sampling_rate = amb->default_sampling_rate;

	amb->status = status;
	amb->status_valid = true;
	amb->next_update_jiffies = jiffies +
		msecs_to_jiffies(amb->current_sampling_rate);

	update_ambient_status(status);
}

static void exynos_amb_control_work_fn(struct kthread_work *work)
{
	struct ambient_thermal_zone *amb =
		container_of(work, struct ambient_thermal_zone, dwork.work);
	int temp;
	unsigned int delay;

	mutex_lock(&amb->lock);

	if (amb->in_suspend)
		goto out;

	exynos_amb_control_lookup_zone(amb);
	temp = get_ambient_temp();

	if (temp < 0) {
		delay = amb->high_sampling_rate;
		amb->next_update_jiffies = jiffies + msecs_to_jiffies(delay);
		goto schedule;
	}

	if (!temp) {
		amb->current_sampling_rate = amb->high_sampling_rate;
		delay = amb->current_sampling_rate;
		amb->next_update_jiffies = jiffies + msecs_to_jiffies(delay);
		goto schedule;
	}

	exynos_amb_control_apply(amb, temp);
	delay = amb->current_sampling_rate;

schedule:
	if (!amb->in_suspend)
		exynos_amb_control_set_polling(amb, delay);
out:
	mutex_unlock(&amb->lock);
}

static int exynos_amb_control_pm_notify(struct notifier_block *nb,
					unsigned long mode, void *_unused)
{
	struct ambient_thermal_zone *amb =
		container_of(nb, struct ambient_thermal_zone, pm_nb);

	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
	case PM_SUSPEND_PREPARE:
		mutex_lock(&amb->lock);
		amb->in_suspend = true;
		mutex_unlock(&amb->lock);
		kthread_cancel_delayed_work_sync(&amb->dwork);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		mutex_lock(&amb->lock);
		amb->in_suspend = false;
		mutex_unlock(&amb->lock);
		exynos_amb_control_set_polling(amb, amb->current_sampling_rate);
		break;
	default:
		break;
	}

	return 0;
}

static bool exynos_amb_control_init(struct ambient_thermal_zone *amb)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };

	amb->amb_data[AMB_TZ_BIG].hotplug_in_threshold = hotplug_in_big * 1000;
	amb->amb_data[AMB_TZ_BIG].hotplug_out_threshold = hotplug_out_big * 1000;
	amb->amb_data[AMB_TZ_MID].hotplug_in_threshold = hotplug_in_mid * 1000;
	amb->amb_data[AMB_TZ_MID].hotplug_out_threshold = hotplug_out_mid * 1000;
	amb->amb_data[AMB_TZ_LIT].hotplug_in_threshold = hotplug_in_lit * 1000;
	amb->amb_data[AMB_TZ_LIT].hotplug_out_threshold = hotplug_out_lit * 1000;

	cpulist_parse("7", &amb->amb_data[AMB_TZ_BIG].cpu_domain);
	cpulist_parse("4-6", &amb->amb_data[AMB_TZ_MID].cpu_domain);
	cpulist_parse("2-3", &amb->amb_data[AMB_TZ_LIT].cpu_domain);

	snprintf(amb->amb_data[AMB_TZ_BIG].cpuhp_name, THERMAL_NAME_LENGTH,
		 "AMB_BIG");
	exynos_cpuhp_register(amb->amb_data[AMB_TZ_BIG].cpuhp_name,
			      *cpu_possible_mask);

	snprintf(amb->amb_data[AMB_TZ_MID].cpuhp_name, THERMAL_NAME_LENGTH,
		 "AMB_MID");
	exynos_cpuhp_register(amb->amb_data[AMB_TZ_MID].cpuhp_name,
			      *cpu_possible_mask);

	snprintf(amb->amb_data[AMB_TZ_LIT].cpuhp_name, THERMAL_NAME_LENGTH,
		 "AMB_LIT");
	exynos_cpuhp_register(amb->amb_data[AMB_TZ_LIT].cpuhp_name,
			      *cpu_possible_mask);

	mutex_init(&amb->lock);
	kthread_init_worker(&amb->worker);
	kthread_init_delayed_work(&amb->dwork, exynos_amb_control_work_fn);

	amb->thread = kthread_create(kthread_worker_fn, &amb->worker,
				     "thermal_amb");
	if (IS_ERR(amb->thread)) {
		pr_warn("thermal: failed to create ambient worker: %ld\n",
			PTR_ERR(amb->thread));
		amb->thread = NULL;
		return false;
	}

	kthread_bind(amb->thread, 0);
	if (sched_setscheduler_nocheck(amb->thread, SCHED_FIFO, &param))
		pr_warn("thermal: failed to set ambient worker priority\n");
	wake_up_process(amb->thread);

	amb->default_sampling_rate = AMB_DEFAULT_SAMPLING_RATE_MS;
	amb->high_sampling_rate = AMB_HIGH_SAMPLING_RATE_MS;
	amb->increase_sampling_temp = AMB_INCREASE_SAMPLING_TEMP_MC;
	amb->decrease_sampling_temp = AMB_DECREASE_SAMPLING_TEMP_MC;
	amb->current_sampling_rate = amb->default_sampling_rate;
	amb->pm_nb.notifier_call = exynos_amb_control_pm_notify;
	register_pm_notifier(&amb->pm_nb);

	exynos_pm_qos_add_request(&amb->mif_max_pm_qos, PM_QOS_BUS_THROUGHPUT_MAX,
				  PM_QOS_BUS_THROUGHPUT_MAX_DEFAULT_VALUE);
	exynos_pm_qos_add_request(&amb->mif_min_pm_qos, PM_QOS_BUS_THROUGHPUT,
				  PM_QOS_BUS_THROUGHPUT_DEFAULT_VALUE);

	return true;
}

void exynos_amb_control_register_tmu(struct exynos_tmu_data *data)
{
	int control_temp = 0;

	if (!data || data->id >= AMB_TZ_NUM)
		return;

	if (!amb_tz) {
		amb_tz = kzalloc(sizeof(*amb_tz), GFP_KERNEL);
		if (!amb_tz)
			return;
		if (!exynos_amb_control_init(amb_tz)) {
			kfree(amb_tz);
			amb_tz = NULL;
			return;
		}
	}

	mutex_lock(&amb_tz->lock);
	amb_tz->registered_mask |= BIT(data->id);

	if (data->use_pi_thermal && data->pi_param) {
		amb_tz->amb_data[data->id].use_pi_thermal = data->use_pi_thermal;
		amb_tz->amb_data[data->id].tzd = data->tzd;
		data->tzd->ops->get_trip_temp(data->tzd,
					      data->pi_param->trip_control_temp,
					      &control_temp);
		amb_tz->amb_data[data->id].normal_control_temp = control_temp;
		amb_tz->amb_data[data->id].emg_control_temp =
			exynos_amb_offset_to_mc(emergency_control_temp * MCELSIUS,
						data->trip_offset);
	} else if (data->id == AMB_TZ_ISP) {
		amb_tz->amb_data[data->id].tzd = data->tzd;
		data->tzd->ops->get_trip_temp(data->tzd, 1, &control_temp);
		amb_tz->amb_data[data->id].decrease_trip_temp = control_temp;
		data->tzd->ops->get_trip_temp(data->tzd, 2, &control_temp);
		amb_tz->amb_data[data->id].increase_trip_temp = control_temp;
	}
	mutex_unlock(&amb_tz->lock);

	exynos_amb_control_kick(0);
}

void exynos_amb_control_unregister_tmu(struct exynos_tmu_data *data)
{
	struct ambient_thermal_zone *amb = amb_tz;
	bool free_amb = false;

	if (!amb || !data || data->id >= AMB_TZ_NUM)
		return;

	mutex_lock(&amb->lock);
	amb->registered_mask &= ~BIT(data->id);
	memset(&amb->amb_data[data->id], 0, sizeof(amb->amb_data[data->id]));
	free_amb = !amb->registered_mask;
	mutex_unlock(&amb->lock);

	if (!free_amb)
		return;

	unregister_pm_notifier(&amb->pm_nb);
	kthread_cancel_delayed_work_sync(&amb->dwork);

	if (exynos_pm_qos_request_active(&amb->mif_max_pm_qos))
		exynos_pm_qos_remove_request(&amb->mif_max_pm_qos);
	if (exynos_pm_qos_request_active(&amb->mif_min_pm_qos))
		exynos_pm_qos_remove_request(&amb->mif_min_pm_qos);

	if (amb->thread)
		kthread_stop(amb->thread);

	amb_tz = NULL;
	kfree(amb);
}

unsigned int exynos_amb_control_get_status(void)
{
	if (!amb_tz || !amb_tz->status_valid)
		return 0;

	return READ_ONCE(amb_tz->status);
}

void exynos_amb_control_kick(unsigned int delay_ms)
{
	if (!amb_tz)
		return;

	exynos_amb_control_set_polling(amb_tz, delay_ms);
}
