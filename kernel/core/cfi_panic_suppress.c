// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - Panic Suppression Layer
 *
 * Prevents the kernel's default panic behavior for CPU faults that
 * CFI can handle via isolation. Instead of crashing the entire system,
 * we suppress the panic and let CFI isolate the faulting CPU.
 *
 * Suppresses:
 *   - softlockup_panic: kernel would panic on softlockup
 *   - hardlockup_panic: kernel would panic on hardlockup
 *   - MCE tolerant level: kernel would panic on fatal MCE
 *
 * Also registers a panic notifier as a last-resort logger in case
 * a panic still occurs (e.g., from a source we don't suppress).
 */

#define pr_fmt(fmt) CFI_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include "cfi_internal.h"

#define PROC_SOFTLOCKUP_PANIC	"/proc/sys/kernel/softlockup_panic"
#define PROC_HARDLOCKUP_PANIC	"/proc/sys/kernel/hardlockup_panic"
#define SYSFS_MCE_TOLERANT	"/sys/devices/system/machinecheck/machinecheck0/tolerant"

static int orig_softlockup_panic = -1;
static int orig_hardlockup_panic = -1;
static int orig_mce_tolerant = -1;

static int cfi_read_int_file(const char *path, int *value)
{
	struct file *f;
	char buf[32];
	loff_t pos = 0;
	ssize_t len;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	len = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);

	if (len <= 0)
		return (len < 0) ? (int)len : -EIO;

	buf[len] = '\0';
	return kstrtoint(strim(buf), 10, value);
}

static int cfi_write_int_file(const char *path, int value)
{
	struct file *f;
	char buf[32];
	loff_t pos = 0;
	int len;
	ssize_t ret;

	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	len = snprintf(buf, sizeof(buf), "%d\n", value);
	ret = kernel_write(f, buf, len, &pos);
	filp_close(f, NULL);

	return (ret == len) ? 0 : -EIO;
}

static int cfi_panic_notifier_fn(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	const char *reason = (const char *)data;

	pr_emerg("PANIC on cpu%d: %s\n", raw_smp_processor_id(),
		 reason ? reason : "(unknown)");
	pr_emerg("CFI was unable to prevent this panic. "
		 "Check if the fault source is covered by CFI.\n");

	return NOTIFY_DONE;
}

static struct notifier_block cfi_panic_nb = {
	.notifier_call	= cfi_panic_notifier_fn,
	.priority	= INT_MAX,
};

int cfi_suppress_init(void)
{
	int val, ret;

	/* Suppress softlockup panic */
	ret = cfi_read_int_file(PROC_SOFTLOCKUP_PANIC, &val);
	if (ret == 0) {
		orig_softlockup_panic = val;
		if (val != 0) {
			ret = cfi_write_int_file(PROC_SOFTLOCKUP_PANIC, 0);
			if (ret == 0)
				pr_info("suppressed softlockup_panic (was %d)\n", val);
			else
				pr_warn("failed to suppress softlockup_panic: %d\n", ret);
		}
	} else {
		pr_info("softlockup_panic not available (%d)\n", ret);
	}

	/* Suppress hardlockup panic */
	ret = cfi_read_int_file(PROC_HARDLOCKUP_PANIC, &val);
	if (ret == 0) {
		orig_hardlockup_panic = val;
		if (val != 0) {
			ret = cfi_write_int_file(PROC_HARDLOCKUP_PANIC, 0);
			if (ret == 0)
				pr_info("suppressed hardlockup_panic (was %d)\n", val);
			else
				pr_warn("failed to suppress hardlockup_panic: %d\n", ret);
		}
	} else {
		pr_info("hardlockup_panic not available (%d)\n", ret);
	}

	/* Raise MCE tolerant level to prevent MCE panic */
	ret = cfi_read_int_file(SYSFS_MCE_TOLERANT, &val);
	if (ret == 0) {
		orig_mce_tolerant = val;
		if (val < (int)cfi_mce_tolerant) {
			ret = cfi_write_int_file(SYSFS_MCE_TOLERANT,
						 cfi_mce_tolerant);
			if (ret == 0)
				pr_info("set MCE tolerant=%u (was %d)\n",
					cfi_mce_tolerant, val);
			else
				pr_warn("failed to set MCE tolerant: %d\n", ret);
		}
	} else {
		pr_info("MCE tolerant sysfs not available (%d)\n", ret);
	}

	atomic_notifier_chain_register(&panic_notifier_list, &cfi_panic_nb);

	pr_info("panic suppression active\n");
	return 0;
}

void cfi_suppress_exit(void)
{
	atomic_notifier_chain_unregister(&panic_notifier_list, &cfi_panic_nb);

	if (orig_mce_tolerant >= 0)
		cfi_write_int_file(SYSFS_MCE_TOLERANT, orig_mce_tolerant);
	if (orig_hardlockup_panic >= 0)
		cfi_write_int_file(PROC_HARDLOCKUP_PANIC, orig_hardlockup_panic);
	if (orig_softlockup_panic >= 0)
		cfi_write_int_file(PROC_SOFTLOCKUP_PANIC, orig_softlockup_panic);

	pr_info("restored original panic settings\n");
}
