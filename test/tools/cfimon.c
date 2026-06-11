// SPDX-License-Identifier: GPL-2.0
/*
 * cfimon - CFI/MFI generic netlink event monitor
 *
 * Reference implementation of the daemon-side event handling for the
 * cpu_fault_isolate module, with no external dependencies (raw
 * NETLINK_GENERIC, no libnl). Used by the test plan to verify the
 * netlink protocol end to end; cfid can reuse the parsing as-is.
 *
 * Usage:
 *   cfimon                monitor multicast events (blocks)
 *   cfimon --mem-status   query MFI statistics and exit
 *
 * Build: gcc -O2 -Wall -o cfimon cfimon.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#include "../../kernel/include/uapi/cfi.h"

#define BUF_SZ 8192

static int nl_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);

	if (fd < 0) {
		perror("socket(NETLINK_GENERIC)");
		exit(1);
	}
	return fd;
}

/* Append one attribute to a netlink message under construction */
static void nla_put(struct nlmsghdr *nlh, int type, const void *data, int len)
{
	struct nlattr *nla =
		(struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));

	nla->nla_type = type;
	nla->nla_len = NLA_HDRLEN + len;
	memcpy((char *)nla + NLA_HDRLEN, data, len);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(nla->nla_len);
}

static int nl_send_cmd(__u16 family, __u8 cmd, struct nlmsghdr *nlh)
{
	struct genlmsghdr *gnlh;

	memset(nlh, 0, NLMSG_SPACE(GENL_HDRLEN));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = (unsigned int)time(NULL);

	gnlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gnlh->cmd = cmd;
	gnlh->version = CFI_GENL_VERSION;
	return 0;
}

/* Iterate attributes of a genetlink message into a table */
static void parse_attrs(struct nlmsghdr *nlh, struct nlattr **tb, int max)
{
	struct nlattr *nla = (struct nlattr *)((char *)NLMSG_DATA(nlh) +
					       GENL_HDRLEN);
	int rem = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

	memset(tb, 0, sizeof(*tb) * (max + 1));
	while (rem >= (int)NLA_HDRLEN && nla->nla_len >= NLA_HDRLEN &&
	       nla->nla_len <= rem) {
		int type = nla->nla_type & NLA_TYPE_MASK;

		if (type <= max)
			tb[type] = nla;
		rem -= NLA_ALIGN(nla->nla_len);
		nla = (struct nlattr *)((char *)nla + NLA_ALIGN(nla->nla_len));
	}
}

static void *nla_data(struct nlattr *nla) { return (char *)nla + NLA_HDRLEN; }
static __u32 nla_u32(struct nlattr *nla) { return *(__u32 *)nla_data(nla); }
static __u8  nla_u8(struct nlattr *nla)  { return *(__u8 *)nla_data(nla); }

/*
 * Resolve the "CFI" family id and the "events" multicast group id via
 * the genetlink controller.
 */
static int resolve_family(int fd, __u16 *family_id, __u32 *mcgrp_id)
{
	char buf[BUF_SZ];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nlattr *tb[CTRL_ATTR_MAX + 1];
	int len;

	nl_send_cmd(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, nlh);
	nla_put(nlh, CTRL_ATTR_FAMILY_NAME, CFI_GENL_NAME,
		strlen(CFI_GENL_NAME) + 1);

	if (send(fd, nlh, nlh->nlmsg_len, 0) < 0) {
		perror("send(GETFAMILY)");
		return -1;
	}

	len = recv(fd, buf, sizeof(buf), 0);
	if (len < 0) {
		perror("recv(GETFAMILY)");
		return -1;
	}
	if (nlh->nlmsg_type == NLMSG_ERROR) {
		fprintf(stderr,
			"family '%s' not found - is cpu_fault_isolate loaded?\n",
			CFI_GENL_NAME);
		return -1;
	}

	parse_attrs(nlh, tb, CTRL_ATTR_MAX);
	if (!tb[CTRL_ATTR_FAMILY_ID]) {
		fprintf(stderr, "no family id in GETFAMILY reply\n");
		return -1;
	}
	*family_id = *(__u16 *)nla_data(tb[CTRL_ATTR_FAMILY_ID]);

	*mcgrp_id = 0;
	if (tb[CTRL_ATTR_MCAST_GROUPS]) {
		struct nlattr *grp = nla_data(tb[CTRL_ATTR_MCAST_GROUPS]);
		int rem = tb[CTRL_ATTR_MCAST_GROUPS]->nla_len - NLA_HDRLEN;

		while (rem >= (int)NLA_HDRLEN) {
			/* each group is a nested attr list */
			struct nlattr *ga = nla_data(grp);
			int grem = grp->nla_len - NLA_HDRLEN;
			const char *name = NULL;
			__u32 id = 0;

			while (grem >= (int)NLA_HDRLEN) {
				int t = ga->nla_type & NLA_TYPE_MASK;

				if (t == CTRL_ATTR_MCAST_GRP_NAME)
					name = nla_data(ga);
				else if (t == CTRL_ATTR_MCAST_GRP_ID)
					id = nla_u32(ga);
				grem -= NLA_ALIGN(ga->nla_len);
				ga = (struct nlattr *)((char *)ga +
						NLA_ALIGN(ga->nla_len));
			}
			if (name && !strcmp(name, CFI_MCGRP_EVENTS)) {
				*mcgrp_id = id;
				break;
			}
			rem -= NLA_ALIGN(grp->nla_len);
			grp = (struct nlattr *)((char *)grp +
					NLA_ALIGN(grp->nla_len));
		}
	}
	return 0;
}

static const char *mem_err_type_name(__u8 t)
{
	switch (t) {
	case MFI_MEM_CE:		return "CE";
	case MFI_MEM_UCE_DEFERRED:	return "UCE-deferred";
	case MFI_MEM_UCE_CONSUMED:	return "UCE-consumed";
	default:			return "?";
	}
}

static const char *page_state_name(__u8 s)
{
	switch (s) {
	case MFI_PAGE_WATCHED:		return "watched";
	case MFI_PAGE_PRE_ISO:		return "pre_isolating";
	case MFI_PAGE_PRE_ISO_FAILED:	return "pre_isolate_failed";
	case MFI_PAGE_POISONED:		return "poisoned";
	case MFI_PAGE_OFFLINED:		return "offlined";
	case MFI_PAGE_FAILED:		return "offline_failed";
	default:			return "?";
	}
}

static const char *cpu_state_name(__u8 s)
{
	switch (s) {
	case CFI_STATE_ONLINE:		return "online";
	case CFI_STATE_DEGRADED:	return "degraded";
	case CFI_STATE_ISOLATING:	return "isolating";
	case CFI_STATE_ISOLATED:	return "isolated";
	case CFI_STATE_FAILED:		return "failed";
	default:			return "?";
	}
}

static void print_mem_event(__u8 cmd, struct nlattr **tb)
{
	const char *what;

	switch (cmd) {
	case CFI_CMD_MEM_ERROR_EVENT:	 what = "MEM_ERROR"; break;
	case CFI_CMD_MEM_PAGE_OFFLINED:	 what = "MEM_PAGE_OFFLINED"; break;
	case CFI_CMD_MEM_PAGE_FAILED:	 what = "MEM_PAGE_FAILED"; break;
	case CFI_CMD_MEM_VM_KILLED:	 what = "MEM_VM_KILLED"; break;
	case CFI_CMD_MEM_MIGRATE_ADVISED: what = "MEM_MIGRATE_ADVISED"; break;
	default:			 what = "MEM_?"; break;
	}

	if (cmd == CFI_CMD_MEM_MIGRATE_ADVISED) {
		printf("[%s] dimm=%s  ** evacuate host, file repair **\n",
		       what,
		       tb[CFI_ATTR_DIMM_LABEL] ?
				(char *)nla_data(tb[CFI_ATTR_DIMM_LABEL]) : "?");
		return;
	}

	if (tb[CFI_ATTR_MEM_REC]) {
		struct mfi_mem_event ev;

		memcpy(&ev, nla_data(tb[CFI_ATTR_MEM_REC]), sizeof(ev));
		printf("[%s] pfn=0x%llx type=%s state=%s ce=%u flags=0x%x cpu=%u",
		       what, (unsigned long long)ev.pfn,
		       mem_err_type_name(ev.err_type),
		       page_state_name(ev.page_state),
		       ev.ce_count, ev.flags, ev.cpu);
		if (ev.pid)
			printf(" victim=%u(%.16s)", ev.pid, ev.comm);
		if (ev.dimm_label[0])
			printf(" dimm=%.32s", ev.dimm_label);
		printf("\n");

		/*
		 * Daemon hook points:
		 *  MEM_PAGE_FAILED  -> resolve pfn owner, live-migrate VM
		 *  MEM_VM_KILLED    -> tell scheduler to rebuild VM elsewhere
		 */
	}
}

static void monitor(void)
{
	char buf[BUF_SZ];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nlattr *tb[CFI_ATTR_MAX + 1];
	__u16 family;
	__u32 grp;
	int fd = nl_open();

	if (resolve_family(fd, &family, &grp) < 0)
		exit(1);
	if (!grp) {
		fprintf(stderr, "multicast group '%s' not found\n",
			CFI_MCGRP_EVENTS);
		exit(1);
	}
	if (setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
		       &grp, sizeof(grp)) < 0) {
		perror("NETLINK_ADD_MEMBERSHIP");
		exit(1);
	}

	printf("cfimon: family=%u group=%u, waiting for events...\n",
	       family, grp);

	for (;;) {
		int len = recv(fd, buf, sizeof(buf), 0);
		struct genlmsghdr *gnlh;

		if (len < 0) {
			if (errno == EINTR)
				continue;
			perror("recv");
			break;
		}
		if (nlh->nlmsg_type != family)
			continue;

		gnlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
		parse_attrs(nlh, tb, CFI_ATTR_MAX);

		switch (gnlh->cmd) {
		case CFI_CMD_ERROR_EVENT:
			printf("[CPU_ERROR] cpu=%u\n",
			       tb[CFI_ATTR_CPU] ? nla_u32(tb[CFI_ATTR_CPU]) : 0);
			break;
		case CFI_CMD_STATE_CHANGE:
			printf("[CPU_STATE] cpu=%u state=%s\n",
			       tb[CFI_ATTR_CPU] ? nla_u32(tb[CFI_ATTR_CPU]) : 0,
			       tb[CFI_ATTR_STATE] ?
				cpu_state_name(nla_u8(tb[CFI_ATTR_STATE])) : "?");
			break;
		default:
			if (gnlh->cmd >= CFI_CMD_MEM_ERROR_EVENT)
				print_mem_event(gnlh->cmd, tb);
			break;
		}
		fflush(stdout);
	}
}

static void mem_status(void)
{
	char buf[BUF_SZ];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct nlattr *tb[CFI_ATTR_MAX + 1];
	__u16 family;
	__u32 grp;
	int len;
	int fd = nl_open();

	if (resolve_family(fd, &family, &grp) < 0)
		exit(1);

	nl_send_cmd(family, CFI_CMD_MEM_GET_STATUS, nlh);
	if (send(fd, nlh, nlh->nlmsg_len, 0) < 0) {
		perror("send(MEM_GET_STATUS)");
		exit(1);
	}

	len = recv(fd, buf, sizeof(buf), 0);
	if (len < 0 || nlh->nlmsg_type == NLMSG_ERROR) {
		fprintf(stderr, "MEM_GET_STATUS failed\n");
		exit(1);
	}

	parse_attrs(nlh, tb, CFI_ATTR_MAX);
	if (tb[CFI_ATTR_MEM_STATS]) {
		struct mfi_stats_rec st;

		memcpy(&st, nla_data(tb[CFI_ATTR_MEM_STATS]), sizeof(st));
		printf("ce_total            %llu\n"
		       "uce_async           %llu\n"
		       "uce_consumed        %llu\n"
		       "pages_watched       %llu\n"
		       "pages_pre_offlined  %llu\n"
		       "pages_offlined      %llu\n"
		       "pages_failed        %llu\n"
		       "triage_saved        %llu\n"
		       "triage_panic        %llu\n",
		       (unsigned long long)st.ce_total,
		       (unsigned long long)st.uce_async,
		       (unsigned long long)st.uce_consumed,
		       (unsigned long long)st.pages_watched,
		       (unsigned long long)st.pages_pre_offlined,
		       (unsigned long long)st.pages_offlined,
		       (unsigned long long)st.pages_failed,
		       (unsigned long long)st.triage_saved,
		       (unsigned long long)st.triage_panic);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--mem-status"))
		mem_status();
	else if (argc > 1) {
		fprintf(stderr, "usage: %s [--mem-status]\n", argv[0]);
		return 1;
	} else
		monitor();
	return 0;
}
