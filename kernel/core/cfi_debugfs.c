// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - Debugfs Software Error Injection
 *
 * Provides /sys/kernel/debug/cfi/inject for software-simulated error
 * injection. This bypasses the real hardware error path and directly
 * calls cfi_report_error(), enabling testing on virtual machines and
 * environments without hardware error injection support.
 *
 * Usage:
 *   echo "cpu=4 type=cache_l2 severity=ce" > /sys/kernel/debug/cfi/inject
 *
 * Supported type values:
 *   cache_l1d, cache_l1i, cache_l2, cache_l3,
 *   tlb, bus, internal, generic_core
 *
 * Supported severity values:
 *   ce   - Corrected Error
 *   ucr  - Uncorrected Recoverable
 *   ucf  - Uncorrected Fatal
 */

#define pr_fmt(fmt) CFI_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include "cfi_internal.h"

static struct dentry *cfi_debugfs_dir;

struct type_map {
	const char *name;
	u32 type;
};

static const struct type_map type_table[] = {
	{ "cache_l1d",		CFI_ERR_CACHE_L1D },
	{ "cache_l1i",		CFI_ERR_CACHE_L1I },
	{ "cache_l2",		CFI_ERR_CACHE_L2 },
	{ "cache_l3",		CFI_ERR_CACHE_L3 },
	{ "tlb",		CFI_ERR_TLB },
	{ "bus",		CFI_ERR_BUS },
	{ "internal",		CFI_ERR_INTERNAL },
	{ "generic_core",	CFI_ERR_GENERIC_CORE },
	{ NULL, 0 },
};

static u32 parse_type(const char *str, size_t len)
{
	const struct type_map *t;

	for (t = type_table; t->name; t++) {
		if (len == strlen(t->name) &&
		    strncmp(str, t->name, len) == 0)
			return t->type;
	}
	return 0;
}

static u8 parse_severity(const char *str, size_t len)
{
	if (len == 2 && strncmp(str, "ce", 2) == 0)
		return CFI_SEV_CE;
	if (len == 3 && strncmp(str, "ucr", 3) == 0)
		return CFI_SEV_UCR;
	if (len == 3 && strncmp(str, "ucf", 3) == 0)
		return CFI_SEV_UCF;
	return 0xFF; /* invalid */
}

/*
 * Extract value for a given key from the input string.
 * Input format: "cpu=4 type=cache_l2 severity=ce"
 * Returns pointer to value start and sets *val_len.
 */
static const char *find_key(const char *buf, size_t buflen,
			     const char *key, size_t *val_len)
{
	size_t keylen = strlen(key);
	const char *p = buf;
	const char *end = buf + buflen;

	while (p < end) {
		/* Skip whitespace */
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n'))
			p++;
		if (p >= end)
			break;

		/* Check if this is our key */
		if (end - p > keylen + 1 &&
		    strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
			const char *val = p + keylen + 1;
			const char *val_end = val;

			while (val_end < end && *val_end != ' ' &&
			       *val_end != '\t' && *val_end != '\n' &&
			       *val_end != '\0')
				val_end++;

			*val_len = val_end - val;
			return val;
		}

		/* Skip to next token */
		while (p < end && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
	}
	return NULL;
}

/*
 * Write handler for /sys/kernel/debug/cfi/inject.
 *
 * Parses: "cpu=N type=<type> severity=<sev>"
 * Constructs a cfi_error_event and calls cfi_report_error().
 */
static ssize_t cfi_inject_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct cfi_error_event event = {};
	char *buf;
	const char *val;
	size_t val_len;
	unsigned int cpu;
	u32 type;
	u8 severity;
	int ret;

	if (count > 256)
		return -EINVAL;

	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, ubuf, count)) {
		kfree(buf);
		return -EFAULT;
	}
	buf[count] = '\0';

	/* Parse cpu=N */
	val = find_key(buf, count, "cpu", &val_len);
	if (!val) {
		pr_err("inject: missing 'cpu=' parameter\n");
		ret = -EINVAL;
		goto out;
	}
	ret = kstrtouint_from_user(ubuf + (val - buf), val_len, 10, &cpu);
	if (ret) {
		/* kstrtouint_from_user needs user pointer; parse locally */
		char tmp[16] = {};

		if (val_len >= sizeof(tmp)) {
			ret = -EINVAL;
			goto out;
		}
		memcpy(tmp, val, val_len);
		ret = kstrtouint(tmp, 10, &cpu);
		if (ret) {
			pr_err("inject: invalid cpu value\n");
			goto out;
		}
	}
	if (cpu >= nr_cpu_ids) {
		pr_err("inject: cpu %u out of range (max %u)\n",
		       cpu, nr_cpu_ids - 1);
		ret = -EINVAL;
		goto out;
	}

	/* Parse type=<type> */
	val = find_key(buf, count, "type", &val_len);
	if (!val) {
		pr_err("inject: missing 'type=' parameter\n");
		ret = -EINVAL;
		goto out;
	}
	type = parse_type(val, val_len);
	if (!type) {
		pr_err("inject: unknown type (use: cache_l1d, cache_l1i, cache_l2, cache_l3, tlb, bus, internal, generic_core)\n");
		ret = -EINVAL;
		goto out;
	}

	/* Parse severity=<sev> */
	val = find_key(buf, count, "severity", &val_len);
	if (!val) {
		pr_err("inject: missing 'severity=' parameter\n");
		ret = -EINVAL;
		goto out;
	}
	severity = parse_severity(val, val_len);
	if (severity == 0xFF) {
		pr_err("inject: unknown severity (use: ce, ucr, ucf)\n");
		ret = -EINVAL;
		goto out;
	}

	/* Build the error event */
	event.cpu = cpu;
	event.error_type = type;
	event.severity = severity;
	event.timestamp_ns = ktime_get_ns();

	/* Fill topology from the target CPU */
	if (cpu_online(cpu)) {
		event.socket = topology_physical_package_id(cpu);
		event.core_id = topology_core_id(cpu);
#ifdef CONFIG_X86
		event.thread_id = topology_smt_thread_id(cpu);
#endif
	}

	pr_info("inject: cpu=%u type=0x%x severity=%u\n",
		cpu, type, severity);

	/* Report the simulated error */
	cfi_report_error(&event);

	ret = count;

out:
	kfree(buf);
	return ret;
}

static const struct file_operations cfi_inject_fops = {
	.owner	= THIS_MODULE,
	.write	= cfi_inject_write,
};

/*
 * Create a read-only summary file showing supported injection parameters.
 */
static int cfi_inject_help_show(struct seq_file *m, void *v)
{
	const struct type_map *t;

	seq_puts(m, "CPU Fault Isolation - Software Error Injection\n\n");
	seq_puts(m, "Usage: echo \"cpu=N type=TYPE severity=SEV\" > inject\n\n");

	seq_puts(m, "Supported types:\n");
	for (t = type_table; t->name; t++)
		seq_printf(m, "  %-16s (0x%02x)\n", t->name, t->type);

	seq_puts(m, "\nSupported severity:\n");
	seq_puts(m, "  ce              Corrected Error\n");
	seq_puts(m, "  ucr             Uncorrected Recoverable\n");
	seq_puts(m, "  ucf             Uncorrected Fatal\n");

	return 0;
}

static int cfi_inject_help_open(struct inode *inode, struct file *file)
{
	return single_open(file, cfi_inject_help_show, NULL);
}

static const struct file_operations cfi_inject_help_fops = {
	.owner	= THIS_MODULE,
	.open	= cfi_inject_help_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release = single_release,
};

int cfi_debugfs_init(void)
{
	cfi_debugfs_dir = debugfs_create_dir("cfi", NULL);
	if (IS_ERR(cfi_debugfs_dir)) {
		pr_warn("failed to create debugfs dir: %ld\n",
			PTR_ERR(cfi_debugfs_dir));
		cfi_debugfs_dir = NULL;
		return 0; /* Non-fatal */
	}

	debugfs_create_file("inject", 0200, cfi_debugfs_dir, NULL,
			    &cfi_inject_fops);
	debugfs_create_file("help", 0444, cfi_debugfs_dir, NULL,
			    &cfi_inject_help_fops);

	pr_info("debugfs inject interface at /sys/kernel/debug/cfi/\n");
	return 0;
}

void cfi_debugfs_exit(void)
{
	debugfs_remove_recursive(cfi_debugfs_dir);
}
