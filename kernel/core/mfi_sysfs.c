// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Fault Isolation (MFI) - Sysfs Interface
 *
 * Exposes the memory fault domain under the shared CFI sysfs root:
 *
 *   /sys/kernel/cfi/mem/
 *     enable             (rw) - memory fault domain master switch
 *     pre_isolate        (rw) - proactive CE-driven soft offline
 *     page_ce_threshold  (rw) - per-page CE count to trigger pre-isolation
 *     window_secs        (rw) - page/DIMM CE sliding window
 *     dimm_ce_threshold  (rw) - per-DIMM CE count to advise evacuation
 *     dimm_uce_threshold (rw) - per-DIMM UCE count to advise evacuation
 *     triage             (rw) - Phase-2 kernel-context UCE triage switch
 *     stats              (ro) - counters snapshot (key value per line)
 *     dimms              (ro) - per-DIMM accounting table
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "cfi_internal.h"
#include "mfi_internal.h"

static struct kobject *mfi_kobj;

#define MFI_ATTR_UINT_RW(_name, _var)					\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%u\n", _var);				\
}									\
static ssize_t _name##_store(struct kobject *kobj,			\
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
static struct kobj_attribute mfi_attr_##_name =				\
	__ATTR(_name, 0644, _name##_show, _name##_store)

#define MFI_ATTR_BOOL_RW(_name, _var)					\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%d\n", _var ? 1 : 0);			\
}									\
static ssize_t _name##_store(struct kobject *kobj,			\
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
static struct kobj_attribute mfi_attr_##_name =				\
	__ATTR(_name, 0644, _name##_show, _name##_store)

MFI_ATTR_BOOL_RW(enable, mfi_enable);
MFI_ATTR_BOOL_RW(pre_isolate, mfi_pre_isolate);
MFI_ATTR_UINT_RW(page_ce_threshold, mfi_page_ce_threshold);
MFI_ATTR_UINT_RW(window_secs, mfi_window_secs);
MFI_ATTR_UINT_RW(dimm_ce_threshold, mfi_dimm_ce_threshold);
MFI_ATTR_UINT_RW(dimm_uce_threshold, mfi_dimm_uce_threshold);
MFI_ATTR_BOOL_RW(triage, mfi_triage);

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct mfi_stats_rec rec;

	mfi_stats_snapshot(&rec);
	return sysfs_emit(buf,
			  "ce_total %llu\n"
			  "uce_async %llu\n"
			  "uce_consumed %llu\n"
			  "pages_watched %llu\n"
			  "pages_pre_offlined %llu\n"
			  "pages_offlined %llu\n"
			  "pages_failed %llu\n"
			  "triage_saved %llu\n"
			  "triage_panic %llu\n"
			  "soft_offline_available %d\n",
			  rec.ce_total, rec.uce_async, rec.uce_consumed,
			  rec.pages_watched, rec.pages_pre_offlined,
			  rec.pages_offlined, rec.pages_failed,
			  rec.triage_saved, rec.triage_panic,
			  mfi_soft_offline_available() ? 1 : 0);
}
static struct kobj_attribute mfi_attr_stats = __ATTR_RO(stats);

static ssize_t dimms_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return mfi_dimm_show(buf, PAGE_SIZE);
}
static struct kobj_attribute mfi_attr_dimms = __ATTR_RO(dimms);

static struct attribute *mfi_attrs[] = {
	&mfi_attr_enable.attr,
	&mfi_attr_pre_isolate.attr,
	&mfi_attr_page_ce_threshold.attr,
	&mfi_attr_window_secs.attr,
	&mfi_attr_dimm_ce_threshold.attr,
	&mfi_attr_dimm_uce_threshold.attr,
	&mfi_attr_triage.attr,
	&mfi_attr_stats.attr,
	&mfi_attr_dimms.attr,
	NULL,
};

static const struct attribute_group mfi_attr_group = {
	.attrs	= mfi_attrs,
};

int mfi_sysfs_init(struct kobject *parent)
{
	int ret;

	mfi_kobj = kobject_create_and_add("mem", parent);
	if (!mfi_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(mfi_kobj, &mfi_attr_group);
	if (ret) {
		kobject_put(mfi_kobj);
		mfi_kobj = NULL;
		return ret;
	}

	return 0;
}

void mfi_sysfs_exit(void)
{
	if (mfi_kobj) {
		sysfs_remove_group(mfi_kobj, &mfi_attr_group);
		kobject_put(mfi_kobj);
		mfi_kobj = NULL;
	}
}
