// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: @Flopster101
 * Based on AkiraNoSushi's work for the Mi439 project.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#ifndef _LINUX_SEC_H
#define _LINUX_SEC_H

#include <linux/types.h>

#define SEC_DETECT_LOG(fmt, ...) printk(KERN_INFO "sec_detect: " fmt, ##__VA_ARGS__)

enum SEC_devices {
	DEVICE_UNKNOWN = -1,
	SEC_R9S,
	SEC_O1S,
	SEC_P3S,
	SEC_T2S,
	SEC_DEVICE_COUNT,
};

static const char *const device_names[] = {
	[SEC_R9S] = "Galaxy S21 FE",
	[SEC_O1S] = "Galaxy S21",
	[SEC_P3S] = "Galaxy S21 Ultra",
	[SEC_T2S] = "Galaxy S21+",
};

enum sec_feat {
	SEC_FEAT_USES_S2MPB02 = 0,
	SEC_FEAT_USES_KTD2692,
	SEC_FEAT_SUPPORT_MASK_LAYER,
	SEC_FEAT_SUPPORT_TIG,
	SEC_FEAT_SUPPORT_HMD,
	SEC_FEAT_USES_SSP_UNBOUND,
	SEC_FEAT_USES_SSP_R9S,
	SEC_FEAT_USES_BCM4375,
	SEC_FEAT_USES_BCM4389,
	SEC_FEAT_SUPPORT_DDI_FLASH,
	SEC_FEAT_SUPPORT_GM2_FLASH,
	SEC_FEAT_SUPPORT_POC_SPI,
	SEC_FEAT_EVASION_DISP_DET,
	SEC_FEAT_COUNT,
};

enum mcd_feat {
	MCD_FEAT_TYPE_RSU = 0,
	MCD_FEAT_TYPE_USU,
	MCD_FEAT_TYPE_USUV3,
	MCD_FEAT_TYPE_USUV1,
	MCD_FEAT_TYPE_USUV2,
	MCD_FEAT_COUNT,
};

enum SEC_devices sec_get_current_device(void);
bool sec_get_feat(enum sec_feat feat);
bool sec_get_mcd_feat(enum mcd_feat feat);
bool sec_is_detection_complete(void);

#endif /* _LINUX_SEC_H */
