// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "backend_zstd.h"

struct zstd_ctx {
	void *cctx_mem;
	void *dctx_mem;
	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;
};

static void zstd_release_params(struct zcomp_params *params)
{
}

static int zstd_setup_params(struct zcomp_params *params)
{
	if (params->level == ZCOMP_PARAM_NO_LEVEL)
		params->level = 3; /* ZSTD_defaultCLevel */

	return 0;
}

static void zstd_destroy(struct zcomp_ctx *ctx)
{
	struct zstd_ctx *zctx = ctx->context;

	if (!zctx)
		return;

	vfree(zctx->cctx_mem);
	vfree(zctx->dctx_mem);
	kfree(zctx);
}

static int zstd_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct zstd_ctx *zctx;
	ZSTD_parameters prm;
	size_t sz;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;

	ctx->context = zctx;

	prm = ZSTD_getParams(params->level, PAGE_SIZE, params->dict_sz);

	sz = ZSTD_CCtxWorkspaceBound(prm.cParams);
	zctx->cctx_mem = vzalloc(sz);
	if (!zctx->cctx_mem)
		goto error;

	zctx->cctx = ZSTD_initCCtx(zctx->cctx_mem, sz);
	if (!zctx->cctx)
		goto error;

	sz = ZSTD_DCtxWorkspaceBound();
	zctx->dctx_mem = vzalloc(sz);
	if (!zctx->dctx_mem)
		goto error;

	zctx->dctx = ZSTD_initDCtx(zctx->dctx_mem, sz);
	if (!zctx->dctx)
		goto error;

	return 0;

error:
	zstd_destroy(ctx);
	return -ENOMEM;
}

static int zstd_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			 struct zcomp_req *req)
{
	struct zstd_ctx *zctx = ctx->context;
	ZSTD_parameters prm;
	size_t ret;

	prm = ZSTD_getParams(params->level, req->src_len, params->dict_sz);

	if (!params->dict_sz)
		ret = ZSTD_compressCCtx(zctx->cctx, req->dst, req->dst_len,
					req->src, req->src_len, prm);
	else
		ret = ZSTD_compress_usingDict(zctx->cctx, req->dst,
					      req->dst_len, req->src,
					      req->src_len, params->dict,
					      params->dict_sz, prm);
	if (ZSTD_isError(ret))
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int zstd_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	if (!params->dict_sz)
		ret = ZSTD_decompressDCtx(zctx->dctx, req->dst, req->dst_len,
					  req->src, req->src_len);
	else
		ret = ZSTD_decompress_usingDict(zctx->dctx, req->dst,
						req->dst_len, req->src,
						req->src_len, params->dict,
						params->dict_sz);
	if (ZSTD_isError(ret))
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_zstd = {
	.compress	= zstd_compress,
	.decompress	= zstd_decompress,
	.create_ctx	= zstd_create,
	.destroy_ctx	= zstd_destroy,
	.setup_params	= zstd_setup_params,
	.release_params	= zstd_release_params,
	.name		= "zstd",
};
