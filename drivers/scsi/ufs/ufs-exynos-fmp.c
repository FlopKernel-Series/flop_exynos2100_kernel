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

#ifdef CONFIG_SCSI_UFS_EXYNOS_FMP
static const struct bio_crypt_ctx *
exynos_ufs_fmp_get_crypt_ctx(struct scsi_cmnd *cmd)
{
	struct request *rq = cmd->request;

	if (rq && rq->crypt_ctx)
		return rq->crypt_ctx;

	if (rq && rq->bio && bio_has_crypt_ctx(rq->bio))
		return rq->bio->bi_crypt_context;

	return NULL;
}

static int exynos_ufs_fmp_prepare_prdt(struct ufs_hba *hba,
				       struct ufshcd_lrb *lrbp)
{
	struct scsi_cmnd *cmd = lrbp->cmd;
	struct request *rq;
	struct request_queue *q;
	struct bio *bio;
	const struct bio_crypt_ctx *bc;
	struct fmp_crypto_info fmp_info = { };
	struct fmp_request req = { };
	struct ufshcd_sg_entry *prd;
	u64 iv = 0;
	int sg_segments;
	int ret;
	int idx;

	if (!cmd)
		return 0;

	rq = cmd->request;
	q = rq ? rq->q : NULL;
	bio = rq ? rq->bio : NULL;
	if (!bio || !q)
		return 0;

	sg_segments = scsi_sg_count(cmd);
	if (!sg_segments)
		return 0;

	prd = (struct ufshcd_sg_entry *)lrbp->ucd_prdt_ptr;
	req.table = prd;
	req.prdt_off = hba->sg_entry_size;
	req.prdt_cnt = sg_segments;
	req.cmdq_enabled = false;

	bc = exynos_ufs_fmp_get_crypt_ctx(cmd);
	if (!q->ksm || !bc) {
		ret = exynos_fmp_bypass(&req, bio);
		if (!ret)
			return 0;

		req.fips = true;
		goto encrypt;
	}

	if (bc->bc_key->crypto_cfg.crypto_mode != BLK_ENCRYPTION_MODE_AES_256_XTS ||
	    bc->bc_key->crypto_cfg.data_unit_size != FMP_SECTOR_SIZE ||
	    bc->bc_key->crypto_cfg.is_hw_wrapped ||
	    bc->bc_key->crypto_cfg.dun_bytes > sizeof(iv) ||
	    bc->bc_dun[1] || bc->bc_dun[2] || bc->bc_dun[3]) {
		dev_err(hba->dev, "%s: unsupported blk crypto config mode=%u dusize=%u dun=%u wrapped=%u\n",
			__func__, bc->bc_key->crypto_cfg.crypto_mode,
			bc->bc_key->crypto_cfg.data_unit_size,
			bc->bc_key->crypto_cfg.dun_bytes,
			bc->bc_key->crypto_cfg.is_hw_wrapped);
		return -EOPNOTSUPP;
	}

	fmp_info.enc_mode = EXYNOS_FMP_FILE_ENC;
	fmp_info.algo_mode = EXYNOS_FMP_ALGO_MODE_AES_XTS;

	ret = exynos_fmp_setkey(&fmp_info, (u8 *)bc->bc_key->raw,
				bc->bc_key->size, false);
	if (ret) {
		dev_err(hba->dev, "%s: failed to set FMP key (%d)\n",
			__func__, ret);
		return ret;
	}

encrypt:
	req.iv = &iv;
	req.ivsize = sizeof(iv);
	for (idx = 0; idx < sg_segments; idx++) {
		if (!req.fips)
			iv = bc->bc_dun[0] + idx;

		req.table = prd;
		ret = exynos_fmp_crypt(&fmp_info, &req);
		if (ret) {
			dev_err(hba->dev, "%s: failed to program FMP PRDT (%d)\n",
				__func__, ret);
			return ret;
		}

		prd = (void *)prd + hba->sg_entry_size;
	}

	return 0;
}

static void exynos_ufs_fmp_complete_prdt(struct ufs_hba *hba,
					 struct ufshcd_lrb *lrbp)
{
	struct scsi_cmnd *cmd = lrbp->cmd;
	struct request *rq;
	struct request_queue *q;
	struct bio *bio;
	const struct bio_crypt_ctx *bc;
	struct fmp_crypto_info fmp_info = { };
	struct fmp_request req = { };
	struct ufshcd_sg_entry *prd;
	int sg_segments;
	int idx;

	if (!cmd)
		return;

	rq = cmd->request;
	q = rq ? rq->q : NULL;
	bio = rq ? rq->bio : NULL;
	if (!bio || !q)
		return;

	bc = exynos_ufs_fmp_get_crypt_ctx(cmd);
	if ((q->ksm && bc) || exynos_fmp_fips(bio)) {
		sg_segments = scsi_sg_count(cmd);
		prd = (struct ufshcd_sg_entry *)lrbp->ucd_prdt_ptr;
		req.cmdq_enabled = false;
		req.fips = !bc;

		for (idx = 0; idx < sg_segments; idx++) {
			req.table = prd;
			if (exynos_fmp_clear(&fmp_info, &req))
				break;
			prd = (void *)prd + hba->sg_entry_size;
		}
	}
}

int exynos_ufs_fmp_fill_prdt(struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	return exynos_ufs_fmp_prepare_prdt(hba, lrbp);
}

void exynos_ufs_fmp_clear_prdt(struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	exynos_ufs_fmp_complete_prdt(hba, lrbp);
}

void exynos_ufs_fmp_config(struct ufs_hba *hba, bool init)
{
	if (init) {
		/*
		 * Keep the legacy Exynos FMP format compatible with older
		 * android11-5.4 userdata by preserving the PRDT-based crypto
		 * programming model behind the modern blk-crypto request flow.
		 */
		hba->caps |= UFSHCD_CAP_CRYPTO;
		hba->quirks |= UFSHCD_QUIRK_CUSTOM_KEYSLOT_MANAGER |
			       UFSHCD_QUIRK_KEYS_IN_PRDT;
		blk_ksm_init_passthrough(&hba->ksm);
		hba->ksm.max_dun_bytes_supported = sizeof(u64);
		hba->ksm.features = BLK_CRYPTO_FEATURE_STANDARD_KEYS;
		hba->ksm.crypto_modes_supported[BLK_ENCRYPTION_MODE_AES_256_XTS] =
			FMP_SECTOR_SIZE;
		hba->ksm.dev = hba->dev;
		hba->sg_entry_size = sizeof(struct fmp_table_setting);
	}
	exynos_fmp_sec_cfg(0, 0, init);
}
#else
int exynos_ufs_fmp_fill_prdt(struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	return 0;
}

void exynos_ufs_fmp_clear_prdt(struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
}

void exynos_ufs_fmp_config(struct ufs_hba *hba, bool init)
{
}
#endif
