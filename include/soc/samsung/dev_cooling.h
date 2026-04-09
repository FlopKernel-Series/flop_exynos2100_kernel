#ifndef __DEV_COOLING_H__
#define __DEV_COOLING_H__

#include <linux/of.h>
#include <linux/thermal.h>
#include <linux/platform_device.h>
#include <soc/samsung/exynos-devfreq.h>

#if IS_ENABLED(CONFIG_EXYNOS_THERMAL_V2)
extern struct thermal_cooling_device *exynos_dev_cooling_register(struct device_node *np, struct exynos_devfreq_data *data);
unsigned long exynos_dev_cooling_get_freq(struct thermal_cooling_device *cdev,
					  unsigned long state);
#else
static inline struct thermal_cooling_device *exynos_dev_cooling_register(struct device_node *np, struct exynos_devfreq_data *data)
{
	return NULL;
}

static inline unsigned long
exynos_dev_cooling_get_freq(struct thermal_cooling_device *cdev,
			    unsigned long state)
{
	return 0;
}
#endif
#endif
