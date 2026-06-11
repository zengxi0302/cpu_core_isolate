// SPDX-License-Identifier: GPL-2.0
/*
 * CPU Fault Isolation (CFI) - Generic Netlink Interface
 *
 * Provides communication between the kernel module and the userspace
 * daemon (cfid) using generic netlink:
 *
 * Kernel -> Userspace (multicast):
 *   - CFI_CMD_ERROR_EVENT: New error record
 *   - CFI_CMD_STATE_CHANGE: CPU state transition
 *
 * Userspace -> Kernel (unicast):
 *   - CFI_CMD_GET_STATUS: Query CPU state
 *   - CFI_CMD_SET_POLICY: Update thresholds
 *   - CFI_CMD_ISOLATE: Force isolate a CPU
 *   - CFI_CMD_UNISOLATE: Bring CPU back online
 *   - CFI_CMD_ACK_ISOLATE: Daemon confirms pre-isolation steps done
 */

#define pr_fmt(fmt) "cpu_fault_isolate: " fmt

#include <linux/module.h>
#include <net/genetlink.h>
#include "cfi_internal.h"

/* Attribute validation policy */
static const struct nla_policy cfi_nl_policy[__CFI_ATTR_MAX] = {
	[CFI_ATTR_CPU]		= { .type = NLA_U32 },
	[CFI_ATTR_STATE]	= { .type = NLA_U8 },
	[CFI_ATTR_ERROR_REC]	= { .len = sizeof(struct cfi_error_event) },
	[CFI_ATTR_CE_THRESH]	= { .type = NLA_U32 },
	[CFI_ATTR_UCE_THRESH]	= { .type = NLA_U32 },
	[CFI_ATTR_WINDOW_SEC]	= { .type = NLA_U32 },
	[CFI_ATTR_CE_COUNT]	= { .type = NLA_U32 },
	[CFI_ATTR_UCE_COUNT]	= { .type = NLA_U32 },
	[CFI_ATTR_CE_TOTAL]	= { .type = NLA_U32 },
	[CFI_ATTR_ERR_TYPES]	= { .type = NLA_U32 },
	[CFI_ATTR_USER_PINNED]	= { .type = NLA_U8 },
	[CFI_ATTR_SOCKET]	= { .type = NLA_U32 },
	[CFI_ATTR_CORE_ID]	= { .type = NLA_U32 },
};

/* Forward declarations for command handlers */
static int cfi_nl_get_status(struct sk_buff *skb, struct genl_info *info);
static int cfi_nl_set_policy(struct sk_buff *skb, struct genl_info *info);
static int cfi_nl_isolate(struct sk_buff *skb, struct genl_info *info);
static int cfi_nl_unisolate(struct sk_buff *skb, struct genl_info *info);
static int cfi_nl_ack_isolate(struct sk_buff *skb, struct genl_info *info);

/* Multicast groups */
static const struct genl_multicast_group cfi_mcgrps[] = {
	[0] = { .name = CFI_MCGRP_EVENTS },
};

/* Command definitions */
static const struct genl_small_ops cfi_nl_ops[] = {
	{
		.cmd	= CFI_CMD_GET_STATUS,
		.doit	= cfi_nl_get_status,
	},
	{
		.cmd	= CFI_CMD_SET_POLICY,
		.doit	= cfi_nl_set_policy,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= CFI_CMD_ISOLATE,
		.doit	= cfi_nl_isolate,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= CFI_CMD_UNISOLATE,
		.doit	= cfi_nl_unisolate,
		.flags	= GENL_ADMIN_PERM,
	},
	{
		.cmd	= CFI_CMD_ACK_ISOLATE,
		.doit	= cfi_nl_ack_isolate,
		.flags	= GENL_ADMIN_PERM,
	},
};

/* Generic netlink family definition */
static struct genl_family cfi_genl_family = {
	.name		= CFI_GENL_NAME,
	.version	= CFI_GENL_VERSION,
	.maxattr	= CFI_ATTR_MAX,
	.policy		= cfi_nl_policy,
	.module		= THIS_MODULE,
	.small_ops	= cfi_nl_ops,
	.n_small_ops	= ARRAY_SIZE(cfi_nl_ops),
	.mcgrps		= cfi_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(cfi_mcgrps),
};

/* --- Command handlers --- */

static int cfi_nl_get_status(struct sk_buff *skb, struct genl_info *info)
{
	struct sk_buff *reply;
	struct cfi_cpu_info *ci;
	unsigned int cpu;
	unsigned long flags;
	void *hdr;

	if (!info->attrs[CFI_ATTR_CPU])
		return -EINVAL;

	cpu = nla_get_u32(info->attrs[CFI_ATTR_CPU]);
	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	ci = &cfi_cpus[cpu];

	reply = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!reply)
		return -ENOMEM;

	hdr = genlmsg_put_reply(reply, info, &cfi_genl_family, 0,
				CFI_CMD_GET_STATUS_REPLY);
	if (!hdr) {
		nlmsg_free(reply);
		return -EMSGSIZE;
	}

	spin_lock_irqsave(&ci->lock, flags);
	nla_put_u32(reply, CFI_ATTR_CPU, cpu);
	nla_put_u8(reply, CFI_ATTR_STATE, ci->state);
	nla_put_u32(reply, CFI_ATTR_CE_COUNT, ci->ce_count);
	nla_put_u32(reply, CFI_ATTR_UCE_COUNT, ci->uce_count);
	nla_put_u32(reply, CFI_ATTR_CE_TOTAL, ci->ce_count_total);
	nla_put_u32(reply, CFI_ATTR_ERR_TYPES, ci->error_types_seen);
	nla_put_u8(reply, CFI_ATTR_USER_PINNED, ci->user_pinned ? 1 : 0);
	spin_unlock_irqrestore(&ci->lock, flags);

	genlmsg_end(reply, hdr);
	return genlmsg_reply(reply, info);
}

static int cfi_nl_set_policy(struct sk_buff *skb, struct genl_info *info)
{
	if (info->attrs[CFI_ATTR_CE_THRESH])
		cfi_ce_threshold = nla_get_u32(info->attrs[CFI_ATTR_CE_THRESH]);
	if (info->attrs[CFI_ATTR_UCE_THRESH])
		cfi_uce_threshold = nla_get_u32(info->attrs[CFI_ATTR_UCE_THRESH]);
	if (info->attrs[CFI_ATTR_WINDOW_SEC])
		cfi_window_secs = nla_get_u32(info->attrs[CFI_ATTR_WINDOW_SEC]);

	pr_info("policy updated: ce_thresh=%u uce_thresh=%u window=%us\n",
		cfi_ce_threshold, cfi_uce_threshold, cfi_window_secs);

	return 0;
}

static int cfi_nl_isolate(struct sk_buff *skb, struct genl_info *info)
{
	struct cfi_cpu_info *ci;
	unsigned int cpu;
	unsigned long flags;

	if (!info->attrs[CFI_ATTR_CPU])
		return -EINVAL;

	cpu = nla_get_u32(info->attrs[CFI_ATTR_CPU]);
	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	ci = &cfi_cpus[cpu];
	spin_lock_irqsave(&ci->lock, flags);

	if (ci->state != CFI_STATE_ONLINE &&
	    ci->state != CFI_STATE_DEGRADED) {
		spin_unlock_irqrestore(&ci->lock, flags);
		return -EINVAL;
	}

	ci->state = CFI_STATE_ISOLATING;
	spin_unlock_irqrestore(&ci->lock, flags);

	pr_info("cpu%u: manual isolation requested\n", cpu);
	cfi_nl_send_state_change(cpu, CFI_STATE_ISOLATING);
	cfi_begin_isolation(cpu, false);

	return 0;
}

static int cfi_nl_unisolate(struct sk_buff *skb, struct genl_info *info)
{
	unsigned int cpu;

	if (!info->attrs[CFI_ATTR_CPU])
		return -EINVAL;

	cpu = nla_get_u32(info->attrs[CFI_ATTR_CPU]);
	return cfi_unisolate_cpu(cpu);
}

static int cfi_nl_ack_isolate(struct sk_buff *skb, struct genl_info *info)
{
	unsigned int cpu;

	if (!info->attrs[CFI_ATTR_CPU])
		return -EINVAL;

	cpu = nla_get_u32(info->attrs[CFI_ATTR_CPU]);

	/* Defined in cfi_hotplug.c */
	cfi_daemon_ack_isolate(cpu);
	return 0;
}

/* --- Multicast event senders --- */

int cfi_nl_send_error(const struct cfi_error_event *event)
{
	struct sk_buff *skb;
	void *hdr;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	hdr = genlmsg_put(skb, 0, 0, &cfi_genl_family, 0,
			   CFI_CMD_ERROR_EVENT);
	if (!hdr) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	nla_put(skb, CFI_ATTR_ERROR_REC, sizeof(*event), event);
	nla_put_u32(skb, CFI_ATTR_CPU, event->cpu);

	genlmsg_end(skb, hdr);
	return genlmsg_multicast(&cfi_genl_family, skb, 0, 0, GFP_ATOMIC);
}

int cfi_nl_send_state_change(unsigned int cpu, enum cfi_cpu_state state)
{
	struct sk_buff *skb;
	void *hdr;

	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	hdr = genlmsg_put(skb, 0, 0, &cfi_genl_family, 0,
			   CFI_CMD_STATE_CHANGE);
	if (!hdr) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	nla_put_u32(skb, CFI_ATTR_CPU, cpu);
	nla_put_u8(skb, CFI_ATTR_STATE, state);

	genlmsg_end(skb, hdr);
	return genlmsg_multicast(&cfi_genl_family, skb, 0, 0, GFP_ATOMIC);
}

/* --- Init / Exit --- */

int cfi_netlink_init(void)
{
	return genl_register_family(&cfi_genl_family);
}

void cfi_netlink_exit(void)
{
	genl_unregister_family(&cfi_genl_family);
}
