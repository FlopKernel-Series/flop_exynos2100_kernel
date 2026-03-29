// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reclaim Samsung reserved-memory regions that are left unused when the
 * corresponding feature stack is disabled.
 */

#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/string.h>

struct sec_rmem_reclaim_region {
	const char *node_name;
	bool (*should_reclaim)(void);
};

static int sec_chosen_get_int(const char *key, int *val)
{
	struct device_node *node;
	const char *bootargs;
	char buf[16];
	const char *p;
	int len;
	int i;
	int ret;

	node = of_find_node_by_path("/chosen");
	if (!node)
		return -ENOENT;

	bootargs = of_get_property(node, "bootargs", &len);
	if (!bootargs) {
		ret = -ENOENT;
		goto out;
	}

	p = strstr(bootargs, key);
	if (!p) {
		ret = -ENOENT;
		goto out;
	}

	p += strlen(key);
	for (i = 0; i < ARRAY_SIZE(buf) - 1; i++) {
		if (p[i] == ' ' || p[i] == '\0')
			break;
		buf[i] = p[i];
	}
	buf[i] = '\0';

	ret = kstrtoint(buf, 0, val);

out:
	of_node_put(node);
	return ret;
}

static bool sec_force_upload_enabled(void)
{
	static const char * const keys[] = {
		"sec_debug_mode.force_upload=",
		"androidboot.force_upload=",
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(keys); i++) {
		int force_upload = 0;

		if (!sec_chosen_get_int(keys[i], &force_upload) && force_upload)
			return true;
	}

	return false;
}

static bool sec_should_reclaim_rdx_bootdev(void)
{
	if (IS_ENABLED(CONFIG_SEC_DEBUG))
		return false;

	if (sec_force_upload_enabled())
		return false;

	return true;
}

static const struct sec_rmem_reclaim_region sec_rmem_reclaim_regions[] = {
	{
		.node_name = "sec_rdx_bootdev",
		.should_reclaim = sec_should_reclaim_rdx_bootdev,
	},
};

static void __init sec_rmem_reclaim_one(
	const struct sec_rmem_reclaim_region *region)
{
	struct device_node *node;
	struct resource res;
	char path[64];
	phys_addr_t paddr;
	unsigned long size;
	int ret;

	if (!region->should_reclaim())
		return;

	snprintf(path, sizeof(path), "/reserved-memory/%s", region->node_name);
	node = of_find_node_by_path(path);
	if (!node)
		return;

	if (of_property_read_bool(node, "no-map")) {
		pr_info("%s: skip %s because it is no-map\n", __func__,
			region->node_name);
		goto out;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret) {
		pr_err("%s: failed to parse %s reg (%d)\n", __func__,
		       region->node_name, ret);
		goto out;
	}

	paddr = res.start;
	size = resource_size(&res);
	if (!paddr || !size)
		goto out;

	memset(phys_to_virt(paddr), 0, size);

	ret = memblock_free(paddr, size);
	if (ret) {
		pr_err("%s: memblock_free failed for %s (%d)\n", __func__,
		       region->node_name, ret);
		goto out;
	}

	free_reserved_area(phys_to_virt(paddr), phys_to_virt(paddr) + size, -1,
			   region->node_name);
	pr_info("%s: released %s (%lu KiB)\n", __func__, region->node_name,
		size >> 10);

out:
	of_node_put(node);
}

static int __init sec_rmem_reclaim_init(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(sec_rmem_reclaim_regions); i++)
		sec_rmem_reclaim_one(&sec_rmem_reclaim_regions[i]);

	return 0;
}
rootfs_initcall(sec_rmem_reclaim_init);

MODULE_DESCRIPTION("Samsung reserved-memory reclaim helper");
MODULE_LICENSE("GPL");
