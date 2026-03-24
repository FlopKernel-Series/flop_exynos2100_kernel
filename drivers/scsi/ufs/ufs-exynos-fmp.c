/*
 * Exynos FMP UFS crypto interface
 *
 * Copyright (C) 2020 Samsung Electronics Co., Ltd.
 * Authors: Boojin Kim <boojin.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/keyslot-manager.h>
#include <crypto/fmp.h>
#include "ufshcd.h"
#include "ufshcd-crypto.h"
#include "ufs-exynos.h"
#include <crypto/fmp.h>

#ifdef CONFIG_SCSI_UFS_EXYNOS_FMP
void exynos_ufs_fmp_config(struct ufs_hba *hba, bool init)
{
	if (init) {
		 /*
		 * The android12-5.4 UFS core no longer carries the older Exynos
		 * crypto_vops passthrough path. Keep the enlarged PRDT entries and
		 * secure monitor configuration required by Exynos FMP.
		 */
		hba->sg_entry_size = sizeof(struct fmp_table_setting);
	}
	exynos_fmp_sec_cfg(0, 0, init);
}
#else
void exynos_ufs_fmp_config(struct ufs_hba *hba, bool init)
{
}
#endif
