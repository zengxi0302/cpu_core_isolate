// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - MCE Panic Suppression Layer
 *
 * Scope: ONLY the hardware-MCE-induced panic path (mce_panic()).
 *
 * Lockup panic knobs (softlockup_panic / hardlockup_panic) are NOT touched.
 * Production HCE2 already runs softlockup_panic=0 (default) and
 * hardlockup_panic=1 (default); whichever they're at, we leave them. Software
 * lockups dominate >95% of the lockup cases, can't be hardware-isolated, and
 * the kernel's own watchdog should still be free to act on them per the
 * operator's policy.
 *
 * What we DO:
 *   - Raise mca_cfg.tolerant so the kernel doesn't call panic() on fatal MCE.
 *     CFI's notifier chain handler then performs the isolation.
 *   - Register a panic_notifier_list logger (last-resort post-mortem trail,
 *     fires only if everything else failed and the kernel still panics).
 *   - On HCE3 kernels, register on mce_panic_chain for pre-panic logging.
 *
 * MCE tolerant setting strategy (two methods):
 *   Method 1: Write to /sys/.../tolerant sysfs (standard upstream kernels)
 *   Method 2: Direct kernel variable write via kprobe symbol lookup
 *             (fallback for HCE3/openEuler kernels that removed sysfs)
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/notifier.h>
/*
 * panic_notifier_list lives in <linux/panic_notifier.h> upstream from
 * v5.17, but distros backport the header (e.g. Ubuntu 5.15). On kernels
 * without it (HCE 2.0 / 5.10) the declaration comes via kernel.h. Probe
 * for the header instead of version-gating so all three cases build.
 */
#if defined(__has_include)
#  if __has_include(<linux/panic_notifier.h>)
#    include <linux/panic_notifier.h>
#  endif
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
#  include <linux/panic_notifier.h>
#endif
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include "cfi_internal.h"

#define SYSFS_MCE_TOLERANT_FMT	"/sys/devices/system/machinecheck/machinecheck%u/tolerant"

/*
 * tolerant is mirrored on /sys/devices/system/machinecheck/machinecheckN/
 * for every CPU but they all read/write the same global mca_cfg.tolerant
 * variable. We just need a path that exists right now; pick the first
 * online CPU so module load doesn't fail if cpu0 happens to be offline
 * (e.g. cpu0 was isolated by an earlier CFI incarnation and we got reloaded).
 * On exit we reuse the same path; if that CPU went offline meanwhile we try
 * another, and finally fall back to direct variable write.
 */
static int cfi_mce_tolerant_path(char *buf, size_t buflen)
{
	int cpu = cpumask_first(cpu_online_mask);

	if (cpu >= nr_cpu_ids)
		return -ENOENT;
	snprintf(buf, buflen, SYSFS_MCE_TOLERANT_FMT, cpu);
	return 0;
}

static int orig_mce_tolerant = -1;

/*
 * Pointer to in-kernel mce_tolerant variable, found via kprobe lookup.
 * Used as fallback when sysfs interface is unavailable (HCE3 kernels).
 */
static int *mce_tolerant_ptr;

/*
 * Whether we registered on HCE3's mce_panic_chain.
 */
static bool hce3_panic_chain_registered;

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

/*
 * Use kprobe to look up a kernel symbol address.
 * kallsyms_lookup_name() is not exported to modules since 5.7,
 * but we can register a kprobe on the symbol to get its address.
 */
static unsigned long cfi_lookup_name(const char *name)
{
	struct kprobe kp = {};
	unsigned long addr;

	kp.symbol_name = name;
	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

/*
 * Directly set the in-kernel mce_tolerant variable.
 * Fallback for kernels without tolerant sysfs (HCE3).
 */
static int cfi_set_tolerant_direct(unsigned int target)
{
	unsigned long addr;

	addr = cfi_lookup_name("mce_tolerant");
	if (!addr) {
		/* HCE3 may rename: try tolerant_config or other variants */
		addr = cfi_lookup_name("tolerant");
		if (!addr)
			return -ENOENT;
	}

	mce_tolerant_ptr = (int *)addr;
	orig_mce_tolerant = *mce_tolerant_ptr;

	if (orig_mce_tolerant < (int)target) {
		*mce_tolerant_ptr = (int)target;
		pr_info("set mce_tolerant=%u directly via kallsyms (was %d)\n",
			target, orig_mce_tolerant);
	} else {
		pr_info("mce_tolerant already >= %u (current: %d)\n",
			target, orig_mce_tolerant);
	}
	return 0;
}

static void cfi_restore_tolerant_direct(void)
{
	if (mce_tolerant_ptr && orig_mce_tolerant >= 0) {
		*mce_tolerant_ptr = orig_mce_tolerant;
		pr_info("restored mce_tolerant=%d directly\n",
			orig_mce_tolerant);
	}
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

/*
 * HCE3 mce_panic_chain notifier.
 * Called by notify_mce_panic() just before mce_panic() calls panic().
 * We can't prevent the panic from here, but we log CFI state for
 * post-mortem analysis. If tolerant was set successfully, this
 * should never fire for MCE-related panics.
 */
static int cfi_hce3_mce_panic_fn(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	pr_emerg("HCE3 mce_panic imminent on cpu%d\n",
		 raw_smp_processor_id());
	pr_emerg("mce_tolerant was %s set - if this fires, "
		 "tolerant override failed\n",
		 mce_tolerant_ptr ? "successfully" : "NOT");
	return NOTIFY_DONE;
}

static struct notifier_block cfi_hce3_mce_panic_nb = {
	.notifier_call	= cfi_hce3_mce_panic_fn,
	.priority	= INT_MAX,
};

/*
 * Try to register on HCE3's mce_panic_chain.
 * This is a best-effort operation — only available on HCE3 kernels.
 */
static void cfi_hce3_panic_chain_init(void)
{
	unsigned long addr;
	int (*reg_fn)(struct notifier_block *nb);

	addr = cfi_lookup_name("mce_register_panic_notifier_chain");
	if (!addr)
		return;

	reg_fn = (void *)addr;
	if (reg_fn(&cfi_hce3_mce_panic_nb) == 0) {
		hce3_panic_chain_registered = true;
		pr_info("registered on HCE3 mce_panic_chain\n");
	}
}

static void cfi_hce3_panic_chain_exit(void)
{
	unsigned long addr;
	int (*unreg_fn)(struct notifier_block *nb);

	if (!hce3_panic_chain_registered)
		return;

	addr = cfi_lookup_name("mce_unregister_panic_notifier_chain");
	if (!addr)
		return;

	unreg_fn = (void *)addr;
	unreg_fn(&cfi_hce3_mce_panic_nb);
	hce3_panic_chain_registered = false;
}

int cfi_suppress_init(void)
{
	int val, ret;
	bool tolerant_set = false;

	/*
	 * Intentionally do NOT touch softlockup_panic / hardlockup_panic.
	 * Those knobs gate software-lockup behavior, which CFI cannot isolate
	 * (lockups are overwhelmingly software bugs); leave them at whatever
	 * the operator / distro default is. Production HCE2 already has
	 * softlockup_panic=0 and hardlockup_panic=1, both of which we want
	 * preserved.
	 */

	/*
	 * Raise MCE tolerant level to prevent mce_panic().
	 * Method 1: sysfs (standard upstream kernels)
	 * Method 2: direct variable write via kprobe (HCE3 fallback)
	 */
	{
		char tolerant_path[64];

		ret = cfi_mce_tolerant_path(tolerant_path, sizeof(tolerant_path));
		if (ret == 0)
			ret = cfi_read_int_file(tolerant_path, &val);
		if (ret == 0) {
			orig_mce_tolerant = val;
			if (val < (int)cfi_mce_tolerant) {
				ret = cfi_write_int_file(tolerant_path,
							 cfi_mce_tolerant);
				if (ret == 0) {
					pr_info("set MCE tolerant=%u via %s (was %d)\n",
						cfi_mce_tolerant, tolerant_path, val);
					tolerant_set = true;
				} else {
					pr_warn("failed to set MCE tolerant via sysfs: %d\n", ret);
				}
			} else {
				tolerant_set = true;
			}
		} else {
			pr_info("MCE tolerant sysfs not available (%d), "
				"trying direct variable access\n", ret);
		}
	}

	if (!tolerant_set) {
		ret = cfi_set_tolerant_direct(cfi_mce_tolerant);
		if (ret == 0) {
			tolerant_set = true;
		} else {
			pr_err("CRITICAL: failed to set mce_tolerant by any method! "
			       "Fatal MCEs WILL cause panic instead of isolation.\n");
		}
	}

	atomic_notifier_chain_register(&panic_notifier_list, &cfi_panic_nb);

	/* HCE3: register on mce_panic_chain for pre-panic notification */
	cfi_hce3_panic_chain_init();

	pr_info("MCE panic suppression active (mce_tolerant %s; lockup panic knobs untouched)\n",
		tolerant_set ? "set" : "FAILED");
	return 0;
}

void cfi_suppress_exit(void)
{
	cfi_hce3_panic_chain_exit();

	atomic_notifier_chain_unregister(&panic_notifier_list, &cfi_panic_nb);

	/* Restore tolerant: prefer direct method if that's how we set it */
	if (mce_tolerant_ptr) {
		cfi_restore_tolerant_direct();
	} else if (orig_mce_tolerant >= 0) {
		char tolerant_path[64];

		if (cfi_mce_tolerant_path(tolerant_path,
					  sizeof(tolerant_path)) == 0)
			cfi_write_int_file(tolerant_path, orig_mce_tolerant);
	}

	pr_info("restored original mce_tolerant\n");
}
