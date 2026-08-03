/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FLOPPYKERNEL_H
#define _FLOPPYKERNEL_H

#include <linux/types.h>

/*
 * Private feature-query interface for use with fkfeatctl.
 */
#define PR_GET_FK_FEATURE		0x46504b01

#define PR_FK_FEATURE_SUPPORTED		(1U << 0)
#define PR_FK_FEATURE_BY_INDEX		(1U << 1)

enum fk_feature_id {
	FK_FEATURE_UNAME_BPF_SPOOF = 1,
	FK_FEATURE_MASS_STORAGE_HACK = 2,
	FK_FEATURE_SELINUX_MODE = 3,
	FK_FEATURE_INIT_PROTECTION = 4,
	FK_FEATURE_AOSP_MODE = 5,
	FK_FEATURE_USB_SL_DISABLE = 6,
	FK_FEATURE_INIT_DEBUG = 7,
	FK_FEATURE_ENABLE_DMA_BUF = 8,
	FK_FEATURE_MALI_VERSION = 9,
};

enum fk_selinux_mode {
	FK_SELINUX_MODE_DEFAULT = 0,
	FK_SELINUX_MODE_ENFORCING = 1,
	FK_SELINUX_MODE_PERMISSIVE = 2,
};

struct prctl_fk_feature_state {
	u32 flags;
	u32 reserved;
	u64 value;
};

#define FK_FEATURE_NAME_LEN		32

struct prctl_fk_feature_info {
	u32 feature_id;
	u32 flags;
	u64 value;
	char name[FK_FEATURE_NAME_LEN];
};

bool is_mass_storage_hack_enabled(void);
int get_selinux_mode(void);

#endif /* _FLOPPYKERNEL_H */
