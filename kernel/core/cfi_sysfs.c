// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - Sysfs Interface
 *
 * Exposes per-CPU fault isolation state and global configuration
 * through sysfs:
 *
 *   /sys/devices/system/cpu/cpuX/cfi/
 *     state         (ro) - current isolation state
 *     ce_count      (ro) - corrected errors in current window
 *     uce_count     (ro) - uncorrected errors in current window
 *     ce_total      (ro) - lifetime corrected errors
 *     error_types   (ro) - hex bitmask of error types seen
 *     user_pinned   (rw) - prevent automatic isolation
 *
 *   /sys/kernel/cfi/
 *     ce_threshold  (rw) - corrected error threshold
 *     uce_threshold (rw) - uncorrected error threshold
 *     window_secs   (rw) - counting window duration
 *     auto_isolate  (rw) - enable/disable automatic isolation
 *     version       (ro) - module version string
 */

#define pr_fmt(fmt) CFI_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "cfi_internal.h"

/* ====== Per-CPU attributes ====== */

#define CFI_CPU_ATTR_RO(_name)						\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf);	\
static DEVICE_ATTR_RO(_name)

#define CFI_CPU_ATTR_RW(_name)						\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf);	\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count);		\
static DEVICE_ATTR_RW(_name)

static inline unsigned int dev_to_cpu(struct device *dev)
{
	return dev->id;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	unsigned int cpu = dev_to_cpu(dev);
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];
	unsigned long flags;
	enum cfi_cpu_state s;

	spin_lock_irqsave(&ci->lock, flags);
	s = ci->state;
	spin_unlock_irqrestore(&ci->lock, flags);

	return sysfs_emit(buf, "%s\n", cfi_state_name(s));
}
static DEVICE_ATTR_RO(state);

static ssize_t ce_count_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	unsigned int cpu = dev_to_cpu(dev);
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];

	return sysfs_emit(buf, "%u\n", READ_ONCE(ci->ce_count));
}
static DEVICE_ATTR_RO(ce_count);

static ssize_t uce_count_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	unsigned int cpu = dev_to_cpu(dev);
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];

	return sysfs_emit(buf, "%u\n", READ_ONCE(ci->uce_count));
}
static DEVICE_ATTR_RO(uce_count);

static ssize_t ce_total_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	unsigned int cpu = dev_to_cpu(dev);
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];

	return sysfs_emit(buf, "%u\n", READ_ONCE(ci->ce_count_total));
}
static DEVICE_ATTR_RO(ce_total);

static ssize_t error_types_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	unsigned int cpu = dev_to_cpu(dev);
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];

	return sysfs_emit(buf, "0x%x\n", READ_ONCE(ci->error_types_seen));
}
static DEVICE_ATTR_RO(error_types);

static ssize_t user_pinned_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	unsigned int cpu = dev_to_cpu(dev);
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];

	return sysfs_emit(buf, "%d\n", READ_ONCE(ci->user_pinned) ? 1 : 0);
}

static ssize_t user_pinned_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int cpu = dev_to_cpu(dev);
	struct cfi_cpu_info *ci = &cfi_cpus[cpu];
	unsigned long flags;
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	spin_lock_irqsave(&ci->lock, flags);
	ci->user_pinned = val;
	spin_unlock_irqrestore(&ci->lock, flags);

	pr_info("cpu%u: user_pinned set to %d\n", cpu, val);
	return count;
}
static DEVICE_ATTR_RW(user_pinned);

static struct attribute *cfi_cpu_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_ce_count.attr,
	&dev_attr_uce_count.attr,
	&dev_attr_ce_total.attr,
	&dev_attr_error_types.attr,
	&dev_attr_user_pinned.attr,
	NULL,
};

static const struct attribute_group cfi_cpu_attr_group = {
	.name	= "cfi",
	.attrs	= cfi_cpu_attrs,
};

/* ====== Global attributes under /sys/kernel/cfi/ ====== */

static struct kobject *cfi_kobj;

#define CFI_GLOBAL_ATTR_RW(_name, _var)					\
static ssize_t _name##_g_show(struct kobject *kobj,			\
			      struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%u\n", _var);				\
}									\
static ssize_t _name##_g_store(struct kobject *kobj,			\
			       struct kobj_attribute *attr,		\
			       const char *buf, size_t count)		\
{									\
	unsigned int val;						\
	int ret = kstrtouint(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	_var = val;							\
	return count;							\
}									\
static struct kobj_attribute cfi_attr_##_name =				\
	__ATTR(_name, 0644, _name##_g_show, _name##_g_store)

#define CFI_GLOBAL_ATTR_BOOL_RW(_name, _var)				\
static ssize_t _name##_g_show(struct kobject *kobj,			\
			      struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%d\n", _var ? 1 : 0);			\
}									\
static ssize_t _name##_g_store(struct kobject *kobj,			\
			       struct kobj_attribute *attr,		\
			       const char *buf, size_t count)		\
{									\
	bool val;							\
	int ret = kstrtobool(buf, &val);				\
	if (ret)							\
		return ret;						\
	_var = val;							\
	return count;							\
}									\
static struct kobj_attribute cfi_attr_##_name =				\
	__ATTR(_name, 0644, _name##_g_show, _name##_g_store)

CFI_GLOBAL_ATTR_RW(ce_threshold, cfi_ce_threshold);
CFI_GLOBAL_ATTR_RW(uce_threshold, cfi_uce_threshold);
CFI_GLOBAL_ATTR_RW(window_secs, cfi_window_secs);
CFI_GLOBAL_ATTR_BOOL_RW(auto_isolate, cfi_auto_isolate);

static ssize_t version_g_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0.1.0\n");
}
static struct kobj_attribute cfi_attr_version =
	__ATTR(version, 0444, version_g_show, NULL);

static struct attribute *cfi_global_attrs[] = {
	&cfi_attr_ce_threshold.attr,
	&cfi_attr_uce_threshold.attr,
	&cfi_attr_window_secs.attr,
	&cfi_attr_auto_isolate.attr,
	&cfi_attr_version.attr,
	NULL,
};

static const struct attribute_group cfi_global_attr_group = {
	.attrs	= cfi_global_attrs,
};

/* ====== Init / Exit ====== */

int cfi_sysfs_init(void)
{
	unsigned int cpu;
	int ret;

	/* Create /sys/kernel/cfi/ */
	cfi_kobj = kobject_create_and_add("cfi", kernel_kobj);
	if (!cfi_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(cfi_kobj, &cfi_global_attr_group);
	if (ret) {
		kobject_put(cfi_kobj);
		return ret;
	}

	/* Create per-CPU /sys/devices/system/cpu/cpuX/cfi/ groups */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct device *dev = get_cpu_device(cpu);

		if (!dev)
			continue;
		ret = sysfs_create_group(&dev->kobj, &cfi_cpu_attr_group);
		if (ret)
			pr_warn("cpu%u: failed to create sysfs group: %d\n",
				cpu, ret);
	}
	cpus_read_unlock();

	return 0;
}

void cfi_sysfs_exit(void)
{
	unsigned int cpu;

	cpus_read_lock();
	for_each_possible_cpu(cpu) {
		struct device *dev = get_cpu_device(cpu);

		if (!dev)
			continue;
		sysfs_remove_group(&dev->kobj, &cfi_cpu_attr_group);
	}
	cpus_read_unlock();

	if (cfi_kobj) {
		sysfs_remove_group(cfi_kobj, &cfi_global_attr_group);
		kobject_put(cfi_kobj);
	}
}
