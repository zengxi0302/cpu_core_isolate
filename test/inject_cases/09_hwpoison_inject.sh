#!/bin/bash
# Case 09 — hwpoison_inject (kernel-direct memory_failure)
#
#   path     : echo <pfn> > /sys/kernel/debug/hwpoison/corrupt-pfn
#              -> memory_failure(pfn) full path (rmap unmap, SIGBUS if synced)
#   bank     : n/a — doesn't touch the MCE chain at all
#   target   : a self-owned 4 KiB anonymous page
#
#   no  CFI  : memory_failure runs regardless. Page is hard-offlined.
#              Owner may keep the poisoned PTE or be SIGBUSed depending on
#              consumption. No netlink to a userspace daemon.
#   has CFI  : same as above, PLUS cfi gets a HWPoison notifier callback,
#              increments pages_offlined, and emits MEM_PAGE_OFFLINED via
#              netlink. (This case demonstrates the kernel-direct path is
#              wired to cfi — not a panic-vs-survive contrast.)

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
status_banner "09 hwpoison_inject (kernel memory_failure path)"
require_hwpoison_inject

OFF0=$(mfi_stat pages_offlined)
own_page hwpoison
dmesg_mark

echo "$PFN" > $HWP_DIR/corrupt-pfn
echo "  echoed pfn=$PFN to $HWP_DIR/corrupt-pfn"
sleep 3

OFF1=$(mfi_stat pages_offlined)
OWNER_ALIVE=$(kill -0 "$PFN_OWNER_PID" 2>/dev/null && echo "alive (poisoned PTE kept)" || echo "killed (SIGBUS)")

echo
echo "  observations:"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    owner process           : $OWNER_ALIVE"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
