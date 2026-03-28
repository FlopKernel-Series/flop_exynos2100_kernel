/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSHRINK_SLABD_H
#define _LINUX_KSHRINK_SLABD_H

#include <linux/gfp.h>

struct mem_cgroup;

#ifdef CONFIG_KSHRINK_SLABD
bool kshrink_slabd_bypass(gfp_t gfp_mask, int nid,
			  struct mem_cgroup *memcg, int priority);
#else
static inline bool kshrink_slabd_bypass(gfp_t gfp_mask, int nid,
					struct mem_cgroup *memcg,
					int priority)
{
	return false;
}
#endif

#endif /* _LINUX_KSHRINK_SLABD_H */
