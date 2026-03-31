// SPDX-License-Identifier: GPL-2.0

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <asm/current.h>

#define FLOPPY_USB_RC_BASENAME "init.exynos2100.usb.rc"
#define FLOPPY_USB_RC_PATH "/vendor/etc/init/init.exynos2100.usb.rc"
#define FLOPPY_USB_RC_ALT_PATH "/system/vendor/etc/init/init.exynos2100.usb.rc"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#define FLOPPY_SYS_READ_SYMBOL "__arm64_sys_read"
#define FLOPPY_SYS_FSTAT_SYMBOL "__arm64_sys_newfstat"
#define FLOPPY_PT_REAL_REGS(regs) ((struct pt_regs *)(regs)->regs[0])
#else
#define FLOPPY_SYS_READ_SYMBOL "sys_read"
#define FLOPPY_SYS_FSTAT_SYMBOL "sys_newfstat"
#define FLOPPY_PT_REAL_REGS(regs) (regs)
#endif

static const char FLOPPY_MASS_STORAGE_RC[] =
	"\n"
	"on init\n"
	"    mkdir /config/usb_gadget/g1/functions/mass_storage.0\n"
	"    chown system radio /config/usb_gadget/g1/functions/mass_storage.0/lun.0/file\n"
	"    chmod 0660 /config/usb_gadget/g1/functions/mass_storage.0/lun.0/file\n"
	"    chown system radio /config/usb_gadget/g1/functions/mass_storage.0/lun.0/ro\n"
	"    chmod 0660 /config/usb_gadget/g1/functions/mass_storage.0/lun.0/ro\n"
	"    chown system radio /config/usb_gadget/g1/functions/mass_storage.0/lun.0/cdrom\n"
	"    chmod 0660 /config/usb_gadget/g1/functions/mass_storage.0/lun.0/cdrom\n"
	"    chown system radio /config/usb_gadget/g1/functions/mass_storage.0/lun.0/removable\n"
	"    chmod 0660 /config/usb_gadget/g1/functions/mass_storage.0/lun.0/removable\n"
	"    chown system radio /config/usb_gadget/g1/functions/mass_storage.0/lun.0/nofua\n"
	"    chmod 0660 /config/usb_gadget/g1/functions/mass_storage.0/lun.0/nofua\n"
	"\n"
	"on property:sys.boot_completed=1 && property:sys.usb.config=mass_storage\n"
	"    write /config/usb_gadget/g1/configs/b.1/strings/0x409/configuration \"msc\"\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f1\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f2\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f3\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f4\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f5\n"
	"    write /config/usb_gadget/g1/idVendor 0x04E8\n"
	"    write /config/usb_gadget/g1/idProduct 0x6860\n"
	"    symlink /config/usb_gadget/g1/functions/mass_storage.0 /config/usb_gadget/g1/configs/b.1/f1\n"
	"    write /config/usb_gadget/g1/UDC ${sys.usb.controller}\n"
	"    setprop sys.usb.state ${sys.usb.config}\n"
	"\n"
	"on property:sys.boot_completed=1 && property:sys.usb.config=mass_storage,adb\n"
	"    start adbd\n"
	"\n"
	"on property:sys.boot_completed=1 && property:sys.usb.ffs.ready=1 && property:sys.usb.config=mass_storage,adb\n"
	"    write /config/usb_gadget/g1/configs/b.1/strings/0x409/configuration \"adb_msc\"\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f1\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f2\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f3\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f4\n"
	"    rm /config/usb_gadget/g1/configs/b.1/f5\n"
	"    write /config/usb_gadget/g1/idVendor 0x04E8\n"
	"    write /config/usb_gadget/g1/idProduct 0x6860\n"
	"    symlink /config/usb_gadget/g1/functions/ffs.adb /config/usb_gadget/g1/configs/b.1/f1\n"
	"    symlink /config/usb_gadget/g1/functions/mass_storage.0 /config/usb_gadget/g1/configs/b.1/f2\n"
	"    write /config/usb_gadget/g1/UDC ${sys.usb.controller}\n"
	"    setprop sys.usb.state ${sys.usb.config}\n"
	"\n";

static ssize_t (*floppy_orig_read)(struct file *, char __user *, size_t,
				   loff_t *);
static ssize_t (*floppy_orig_read_iter)(struct kiocb *, struct iov_iter *);
static struct file_operations floppy_usb_fops_proxy;
static size_t floppy_usb_rc_pos;
static const size_t floppy_usb_rc_len = sizeof(FLOPPY_MASS_STORAGE_RC) - 1;
static bool floppy_usb_rc_hooked;
static struct work_struct floppy_stop_init_rc_hook_work;

static bool floppy_is_usb_rc(struct file *file)
{
	char path[256];
	char *dpath;

	if (strcmp(current->comm, "init"))
		return false;

	if (!d_is_reg(file->f_path.dentry))
		return false;

	if (strcmp(file->f_path.dentry->d_name.name, FLOPPY_USB_RC_BASENAME))
		return false;

	dpath = d_path(&file->f_path, path, sizeof(path));
	if (IS_ERR(dpath))
		return false;

	return !strcmp(dpath, FLOPPY_USB_RC_PATH) ||
	       !strcmp(dpath, FLOPPY_USB_RC_ALT_PATH);
}

static ssize_t floppy_usb_read_proxy(struct file *file, char __user *buf,
					 size_t count, loff_t *pos)
{
	ssize_t ret = 0;
	size_t append_count;

	if (floppy_usb_rc_pos && floppy_usb_rc_pos < floppy_usb_rc_len)
		goto append_rc;

	ret = floppy_orig_read(file, buf, count, pos);
	if (ret != 0 || floppy_usb_rc_pos >= floppy_usb_rc_len)
		return ret;

	pr_info("floppy_usb_rc: reached EOF, appending rc payload\n");

append_rc:
	append_count = floppy_usb_rc_len - floppy_usb_rc_pos;
	if (append_count > count - ret)
		append_count = count - ret;

	if (copy_to_user(buf + ret, FLOPPY_MASS_STORAGE_RC + floppy_usb_rc_pos,
			 append_count)) {
		pr_err("floppy_usb_rc: failed to append rc chunk at %zu\n",
		       floppy_usb_rc_pos);
		return ret;
	}

	floppy_usb_rc_pos += append_count;
	ret += append_count;

	if (floppy_usb_rc_pos == floppy_usb_rc_len)
		pr_info("floppy_usb_rc: rc payload append complete\n");

	return ret;
}

static ssize_t floppy_usb_read_iter_proxy(struct kiocb *iocb,
					      struct iov_iter *to)
{
	ssize_t ret = 0;
	size_t append_count;

	if (floppy_usb_rc_pos && floppy_usb_rc_pos < floppy_usb_rc_len)
		goto append_rc;

	ret = floppy_orig_read_iter(iocb, to);
	if (ret != 0 || floppy_usb_rc_pos >= floppy_usb_rc_len)
		return ret;

	pr_info("floppy_usb_rc: reached EOF via read_iter, appending rc payload\n");

append_rc:
	append_count = copy_to_iter(FLOPPY_MASS_STORAGE_RC + floppy_usb_rc_pos,
				    floppy_usb_rc_len - floppy_usb_rc_pos, to);
	if (!append_count) {
		pr_err("floppy_usb_rc: failed to append rc chunk at %zu\n",
		       floppy_usb_rc_pos);
		return ret;
	}

	floppy_usb_rc_pos += append_count;
	ret += append_count;

	if (floppy_usb_rc_pos == floppy_usb_rc_len)
		pr_info("floppy_usb_rc: rc payload append complete\n");

	return ret;
}

static void floppy_stop_init_rc_hook(void)
{
	schedule_work(&floppy_stop_init_rc_hook_work);
}

static void floppy_apply_usb_rc_proxy(struct file *file)
{
	if (floppy_usb_rc_hooked) {
		floppy_stop_init_rc_hook();
		return;
	}

	floppy_usb_rc_hooked = true;
	pr_info("floppy_usb_rc: hooking %s\n", FLOPPY_USB_RC_PATH);

	memcpy(&floppy_usb_fops_proxy, file->f_op,
	       sizeof(floppy_usb_fops_proxy));

	floppy_orig_read = file->f_op->read;
	if (floppy_orig_read)
		floppy_usb_fops_proxy.read = floppy_usb_read_proxy;

	floppy_orig_read_iter = file->f_op->read_iter;
	if (floppy_orig_read_iter)
		floppy_usb_fops_proxy.read_iter = floppy_usb_read_iter_proxy;

	file->f_op = &floppy_usb_fops_proxy;
}

static void floppy_handle_sys_read(unsigned int fd)
{
	struct file *file = fget(fd);

	if (!file)
		return;

	if (floppy_is_usb_rc(file))
		floppy_apply_usb_rc_proxy(file);

	fput(file);
}

static int floppy_sys_read_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pt_regs *real_regs = FLOPPY_PT_REAL_REGS(regs);
	unsigned int fd = real_regs->regs[0];

	(void)p;
	floppy_handle_sys_read(fd);
	return 0;
}

static int floppy_sys_fstat_handler_pre(struct kretprobe_instance *ri,
					    struct pt_regs *regs)
{
	struct pt_regs *real_regs = FLOPPY_PT_REAL_REGS(regs);
	unsigned int fd = real_regs->regs[0];
	void *statbuf = (void *)real_regs->regs[1];
	struct file *file;

	*(void **)&ri->data = NULL;

	file = fget(fd);
	if (!file)
		return 1;

	if (floppy_is_usb_rc(file)) {
		*(void **)&ri->data = statbuf;
		fput(file);
		return 0;
	}

	fput(file);
	return 1;
}

static int floppy_sys_fstat_handler_post(struct kretprobe_instance *ri,
					     struct pt_regs *regs)
{
	void __user *statbuf = *(void **)&ri->data;

	(void)regs;
	if (statbuf) {
		char __user *st_size_ptr =
			(char __user *)statbuf + offsetof(struct stat, st_size);
		long size;
		long new_size;

		if (!copy_from_user(&size, st_size_ptr, sizeof(size))) {
			new_size = size + floppy_usb_rc_len;
			if (copy_to_user(st_size_ptr, &new_size,
					 sizeof(new_size)))
				pr_err("floppy_usb_rc: failed to grow init rc size\n");
		} else {
			pr_err("floppy_usb_rc: failed to read init rc size\n");
		}
	}

	return 0;
}

static struct kprobe floppy_sys_read_kp = {
	.symbol_name = FLOPPY_SYS_READ_SYMBOL,
	.pre_handler = floppy_sys_read_handler_pre,
};

static struct kretprobe floppy_sys_fstat_kp = {
	.kp.symbol_name = FLOPPY_SYS_FSTAT_SYMBOL,
	.entry_handler = floppy_sys_fstat_handler_pre,
	.handler = floppy_sys_fstat_handler_post,
	.data_size = sizeof(void *),
};

static void floppy_do_stop_init_rc_hook(struct work_struct *work)
{
	(void)work;
	unregister_kprobe(&floppy_sys_read_kp);
	unregister_kretprobe(&floppy_sys_fstat_kp);
}

static int __init floppy_mass_storage_hack_init(void)
{
	int ret;

	INIT_WORK(&floppy_stop_init_rc_hook_work, floppy_do_stop_init_rc_hook);

	ret = register_kprobe(&floppy_sys_read_kp);
	if (ret) {
		pr_err("floppy_usb_rc: failed to register read kprobe: %d\n",
		       ret);
		return ret;
	}

	ret = register_kretprobe(&floppy_sys_fstat_kp);
	if (ret) {
		unregister_kprobe(&floppy_sys_read_kp);
		pr_err("floppy_usb_rc: failed to register fstat kretprobe: %d\n",
		       ret);
		return ret;
	}

	pr_info("floppy_usb_rc: enabled\n");
	return 0;
}
late_initcall(floppy_mass_storage_hack_init);
