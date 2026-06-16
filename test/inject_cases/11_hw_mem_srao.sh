#!/bin/bash
# Case 11 — Real #MC Memory SRAO (hw mode)
#
#   path     : mce_inject hw  ->  do_machine_check -> mce_severity()
#              -> severity = MCE_AO_SEVERITY -> memory_failure_queue (async)
#   bank     : 4
#   status   : 0xbc00000000000094
#
#   no  CFI  : kernel's existing AO handler queues memory_failure on the
#              poisoned pfn; page hard-offlined. NO PANIC (PCC=0, S=0).
#              Pre-CFI behavior is already "isolate page, keep machine alive".
#   has CFI  : same, plus cfi mfi accounts uce_async++ and emits netlink.
#
#   prereq   : hw-mode delivery must be functional (see 10_hw_mem_ce.sh).

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
status_banner "11 hw Memory SRAO (hw flag, bank=4, status=$STAT_MEM_SRAO)"
require_mce_inject

UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page hw_mem_srao
PROBE_CPU=$(safe_cpu)
dmesg_mark

mce_submit hw 4 "$STAT_MEM_SRAO" "$PADDR" 0 "$PROBE_CPU"
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
