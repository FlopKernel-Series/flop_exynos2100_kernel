// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/lz4k.h>
#include <linux/vmalloc.h>

#include "backend_lz4k.h"

struct lz4k_ctx {
	void *state;
};

static void lz4k_release_params(struct zcomp_params *params)
{
}

static int lz4k_setup_params(struct zcomp_params *params)
{
	return 0;
}

static int lz4k_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct lz4k_ctx *zctx;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;

	ctx->context = zctx;

	zctx->state = vzalloc(lz4k_encode_state_bytes_min());
	if (!zctx->state) {
		kfree(zctx);
		return -ENOMEM;
	}

	return 0;
}

static void lz4k_destroy(struct zcomp_ctx *ctx)
{
	struct lz4k_ctx *zctx = ctx->context;

	if (!zctx)
		return;

	vfree(zctx->state);
	kfree(zctx);
}

static int lz4k_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			 struct zcomp_req *req)
{
	struct lz4k_ctx *zctx = ctx->context;
	int ret;

	ret = lz4k_encode(zctx->state, req->src, req->dst, req->src_len,
			  req->dst_len, 0);
	if (ret <= 0)
		return -EINVAL;

	req->dst_len = ret;
	return 0;
}

static int lz4k_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	int ret;

	ret = lz4k_decode(req->src, req->dst, req->src_len, req->dst_len);
	if (ret <= 0)
		return -EINVAL;

	return 0;
}

const struct zcomp_ops backend_lz4k = {
	.compress	= lz4k_compress,
	.decompress	= lz4k_decompress,
	.create_ctx	= lz4k_create,
	.destroy_ctx	= lz4k_destroy,
	.setup_params	= lz4k_setup_params,
	.release_params	= lz4k_release_params,
	.name		= "lz4k",
};
