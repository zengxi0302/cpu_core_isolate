#!/bin/bash
# Case 11 — Real #MC Memory SRAO (via mce-inject(8) userspace tool)
#
#   path     : mce-inject(8) -> raise -> do_machine_check -> mce_severity
#              -> MCE_AO_SEVERITY -> memory_failure_queue (async)
#   bank     : 4
#   status   : 0xbc00000000000094 (VAL|UC|EN|MISCV|ADDRV)
#
#   no  CFI  : kernel's AO handler queues memory_failure on the poisoned pfn;
#              page hard-offlined. PCC=0,S=0 so no panic.
#   has CFI  : same, plus cfi mfi uce_async++ + netlink event.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
status_banner "11 hw Memory SRAO (mce-inject userspace tool, bank=4)"
require_mce_inject

UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page hw_mem_srao
PROBE_CPU=$(safe_cpu)
dmesg_mark

mce_inject_file "$PROBE_CPU" 4 "$STAT_MEM_SRAO" "$PADDR" 0x0
sleep 3

UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)

echo
echo "  observations:"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
