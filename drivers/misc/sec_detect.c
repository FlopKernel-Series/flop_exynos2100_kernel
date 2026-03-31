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

#include <linux/init.h>
#ifdef CONFIG_JUMP_LABEL
#include <linux/jump_label.h>
#endif
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sec_detect.h>
#include <linux/string.h>
#ifdef CONFIG_SEC_DETECT_SYSFS
#include <linux/kobject.h>
#include <linux/sysfs.h>
#endif

static int g_sec_current_device = DEVICE_UNKNOWN;
static char g_sec_current_device_name[32] = "Unknown";
static bool sec_feat_flags[SEC_FEAT_COUNT] __read_mostly;
static bool mcd_feat_flags[MCD_FEAT_COUNT] __read_mostly;
static bool g_detection_complete;

#ifdef CONFIG_JUMP_LABEL
DEFINE_STATIC_KEY_FALSE(mcd_feat_usuv3_key);
DEFINE_STATIC_KEY_FALSE(sec_feat_support_hmd_key);
DEFINE_STATIC_KEY_FALSE(sec_feat_support_mask_layer_key);
EXPORT_SYMBOL_GPL(mcd_feat_usuv3_key);
EXPORT_SYMBOL_GPL(sec_feat_support_hmd_key);
EXPORT_SYMBOL_GPL(sec_feat_support_mask_layer_key);
#endif

enum SEC_devices sec_get_current_device(void)
{
	return g_sec_current_device;
}
EXPORT_SYMBOL_GPL(sec_get_current_device);

bool sec_get_feat(enum sec_feat feat)
{
	if (feat < 0 || feat >= SEC_FEAT_COUNT)
		return false;

	return sec_feat_flags[feat];
}
EXPORT_SYMBOL_GPL(sec_get_feat);

bool sec_get_mcd_feat(enum mcd_feat feat)
{
	if (feat < 0 || feat >= MCD_FEAT_COUNT)
		return false;

	return mcd_feat_flags[feat];
}
EXPORT_SYMBOL_GPL(sec_get_mcd_feat);

bool sec_is_detection_complete(void)
{
	return g_detection_complete;
}
EXPORT_SYMBOL_GPL(sec_is_detection_complete);

#ifdef CONFIG_SEC_DETECT_SYSFS
static ssize_t device_name_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 32, "%s\n", g_sec_current_device_name);
}

static ssize_t device_model_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	const char *model_name = "Unknown";

	if (g_sec_current_device >= 0 &&
	    g_sec_current_device < SEC_DEVICE_COUNT)
		model_name = device_names[g_sec_current_device];

	return snprintf(buf, 32, "%s\n", model_name);
}

static struct kobj_attribute device_name_attr =
	__ATTR(device_name, 0444, device_name_show, NULL);
static struct kobj_attribute device_model_attr =
	__ATTR(device_model, 0444, device_model_show, NULL);

static struct attribute *attrs[] = {
	&device_name_attr.attr,
	&device_model_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *device_kobj;
#endif

static inline void setup_camera_params(void)
{
	switch (g_sec_current_device) {
	case SEC_R9S:
		mcd_feat_flags[MCD_FEAT_TYPE_RSU] = true;
		break;
	case SEC_O1S:
		mcd_feat_flags[MCD_FEAT_TYPE_USU] = true;
		break;
	case SEC_P3S:
		mcd_feat_flags[MCD_FEAT_TYPE_USU] = true;
		mcd_feat_flags[MCD_FEAT_TYPE_USUV3] = true;
#ifdef CONFIG_JUMP_LABEL
		static_branch_enable(&mcd_feat_usuv3_key);
#endif
		break;
	case SEC_T2S:
		mcd_feat_flags[MCD_FEAT_TYPE_USU] = true;
		break;
	default:
		break;
	}
}

static inline void print_sec_variables(const char *machine_name)
{
	SEC_DETECT_LOG("Current machine name: %s\n", machine_name);
	SEC_DETECT_LOG("Detected device codename: %s\n",
		       g_sec_current_device_name);
	SEC_DETECT_LOG("sec_feat_uses_s2mpb02 = %s\n",
		       sec_get_feat(SEC_FEAT_USES_S2MPB02) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_uses_ktd2692 = %s\n",
		       sec_get_feat(SEC_FEAT_USES_KTD2692) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_uses_pd_charger_hv_disable = %s\n",
		       sec_get_feat(SEC_FEAT_USES_PD_CHARGER_HV_DISABLE) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_support_mask_layer = %s\n",
		       sec_get_feat(SEC_FEAT_SUPPORT_MASK_LAYER) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_support_tig = %s\n",
		       sec_get_feat(SEC_FEAT_SUPPORT_TIG) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_support_hmd = %s\n",
		       sec_get_feat(SEC_FEAT_SUPPORT_HMD) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_uses_ssp_unbound = %s\n",
		       sec_get_feat(SEC_FEAT_USES_SSP_UNBOUND) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_uses_ssp_r9s = %s\n",
		       sec_get_feat(SEC_FEAT_USES_SSP_R9S) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_uses_bcm4375 = %s\n",
		       sec_get_feat(SEC_FEAT_USES_BCM4375) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_uses_bcm4389 = %s\n",
		       sec_get_feat(SEC_FEAT_USES_BCM4389) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_support_ddi_flash = %s\n",
		       sec_get_feat(SEC_FEAT_SUPPORT_DDI_FLASH) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_support_gm2_flash = %s\n",
		       sec_get_feat(SEC_FEAT_SUPPORT_GM2_FLASH) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_support_poc_spi = %s\n",
		       sec_get_feat(SEC_FEAT_SUPPORT_POC_SPI) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_evasion_disp_det = %s\n",
		       sec_get_feat(SEC_FEAT_EVASION_DISP_DET) ? "true" : "false");
	SEC_DETECT_LOG("sec_feat_uses_rbin = %s\n",
		       sec_get_feat(SEC_FEAT_USES_RBIN) ? "true" : "false");
	SEC_DETECT_LOG("mcd_feat_type_rsu = %s\n",
		       sec_get_mcd_feat(MCD_FEAT_TYPE_RSU) ? "true" : "false");
	SEC_DETECT_LOG("mcd_feat_type_usu = %s\n",
		       sec_get_mcd_feat(MCD_FEAT_TYPE_USU) ? "true" : "false");
	SEC_DETECT_LOG("mcd_feat_type_usuv3 = %s\n",
		       sec_get_mcd_feat(MCD_FEAT_TYPE_USUV3) ? "true" : "false");
	SEC_DETECT_LOG("mcd_feat_type_usuv1 = %s\n",
		       sec_get_mcd_feat(MCD_FEAT_TYPE_USUV1) ? "true" : "false");
	SEC_DETECT_LOG("mcd_feat_type_usuv2 = %s\n",
		       sec_get_mcd_feat(MCD_FEAT_TYPE_USUV2) ? "true" : "false");
}

static int __init sec_detect_init(void)
{
	struct device_node *root;
	const char *machine_name;
	int retval = 0;
#ifdef CONFIG_SEC_DETECT_SYSFS
	int sysfs_ret = 0;
#endif

	root = of_find_node_by_path("/");
	if (!root) {
		SEC_DETECT_LOG("Failed to find device tree root\n");
		smp_wmb();
		g_detection_complete = true;
		retval = -ENOENT;
		goto exit_no_root;
	}

	machine_name = of_get_property(root, "model", NULL);
	if (!machine_name)
		machine_name = of_get_property(root, "compatible", NULL);

	if (!machine_name) {
		SEC_DETECT_LOG("Failed to find machine name\n");
		smp_wmb();
		g_detection_complete = true;
		retval = -ENOENT;
		goto exit_put_root;
	}

	if (strstr(machine_name, "R9S") != NULL) {
		g_sec_current_device = SEC_R9S;
		strscpy(g_sec_current_device_name, "r9s",
			sizeof(g_sec_current_device_name));
		sec_feat_flags[SEC_FEAT_USES_KTD2692] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_MASK_LAYER] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_TIG] = true;
		sec_feat_flags[SEC_FEAT_USES_SSP_R9S] = true;
		sec_feat_flags[SEC_FEAT_USES_BCM4375] = true;
		sec_feat_flags[SEC_FEAT_USES_RBIN] = true;
	} else if (strstr(machine_name, "O1S") != NULL) {
		g_sec_current_device = SEC_O1S;
		strscpy(g_sec_current_device_name, "o1s",
			sizeof(g_sec_current_device_name));
		sec_feat_flags[SEC_FEAT_USES_S2MPB02] = true;
		sec_feat_flags[SEC_FEAT_USES_PD_CHARGER_HV_DISABLE] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_HMD] = true;
		sec_feat_flags[SEC_FEAT_USES_SSP_UNBOUND] = true;
		sec_feat_flags[SEC_FEAT_USES_BCM4375] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_DDI_FLASH] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_GM2_FLASH] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_POC_SPI] = true;
		sec_feat_flags[SEC_FEAT_EVASION_DISP_DET] = true;
		sec_feat_flags[SEC_FEAT_USES_RBIN] = true;
	} else if (strstr(machine_name, "P3S") != NULL) {
		g_sec_current_device = SEC_P3S;
		strscpy(g_sec_current_device_name, "p3s",
			sizeof(g_sec_current_device_name));
		sec_feat_flags[SEC_FEAT_USES_S2MPB02] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_HMD] = true;
		sec_feat_flags[SEC_FEAT_USES_SSP_UNBOUND] = true;
		sec_feat_flags[SEC_FEAT_USES_BCM4389] = true;
	} else if (strstr(machine_name, "T2S") != NULL) {
		g_sec_current_device = SEC_T2S;
		strscpy(g_sec_current_device_name, "t2s",
			sizeof(g_sec_current_device_name));
		sec_feat_flags[SEC_FEAT_USES_S2MPB02] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_HMD] = true;
		sec_feat_flags[SEC_FEAT_USES_SSP_UNBOUND] = true;
		sec_feat_flags[SEC_FEAT_USES_BCM4375] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_DDI_FLASH] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_GM2_FLASH] = true;
		sec_feat_flags[SEC_FEAT_SUPPORT_POC_SPI] = true;
		sec_feat_flags[SEC_FEAT_EVASION_DISP_DET] = true;
		sec_feat_flags[SEC_FEAT_USES_RBIN] = true;
	}

	setup_camera_params();
#ifdef CONFIG_JUMP_LABEL
	if (sec_feat_flags[SEC_FEAT_SUPPORT_HMD])
		static_branch_enable(&sec_feat_support_hmd_key);
	if (sec_feat_flags[SEC_FEAT_SUPPORT_MASK_LAYER])
		static_branch_enable(&sec_feat_support_mask_layer_key);
#endif
	print_sec_variables(machine_name);

#ifdef CONFIG_SEC_DETECT_SYSFS
	device_kobj = kobject_create_and_add("sec_detect", kernel_kobj);
	if (!device_kobj) {
		SEC_DETECT_LOG("Failed to create sysfs kobject\n");
		retval = -ENOMEM;
		goto exit_put_root;
	}

	sysfs_ret = sysfs_create_group(device_kobj, &attr_group);
	if (sysfs_ret) {
		SEC_DETECT_LOG("Failed to create sysfs group, error %d\n",
			       sysfs_ret);
		kobject_put(device_kobj);
		device_kobj = NULL;
	}
#endif

exit_put_root:
	of_node_put(root);
exit_no_root:
	smp_wmb();
	g_detection_complete = true;
	if (!retval)
		SEC_DETECT_LOG("Initialization complete and ready.\n");

	return retval;
}

static void __exit sec_detect_exit(void)
{
#ifdef CONFIG_SEC_DETECT_SYSFS
	if (device_kobj) {
		sysfs_remove_group(device_kobj, &attr_group);
		kobject_put(device_kobj);
	}
#endif
}

rootfs_initcall(sec_detect_init);
module_exit(sec_detect_exit);

MODULE_AUTHOR("Flopster101");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Detects the Samsung device currently running this kernel.");
