#!/bin/bash
# Case 12 — Real #MC Memory SRAR (hw mode)  ★ CRASH-vs-ISOLATE demo ★
#
#   path     : mce_inject hw  ->  do_machine_check -> mce_severity()
#              -> severity = MCE_AR_SEVERITY (action required, recoverable)
#   bank     : 4
#   status   : 0xbd80000000000094  (VAL|UC|EN|MISCV|ADDRV|S|AR)
#
#   no  CFI  : if mce_tolerant < 3, do_machine_check calls mce_panic on
#              an AR-severity event that cannot be recovered to user mode.
#              -> WHOLE MACHINE PANIC + KDUMP.
#              (If tolerant>=3 already, kernel SIGBUSes/kills the owner.)
#   has CFI  : cfi raises mce_tolerant to 3 at insmod time AND registers an
#              MCE panic_notifier that intercepts terminal severities so the
#              machine is steered to memory_failure(MF_ACTION_REQUIRED)
#              instead of mce_panic. Owner gets SIGBUS; page hard-offlined;
#              machine survives.
#
#   prereq   : hw-mode delivery must be functional.
#
#   THIS CASE IS UNSAFE WITHOUT KDUMP CONFIGURED. Make sure /proc/sys/kernel/
#   panic > 0 (auto-reboot) or that kdump captures.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
status_banner "12 hw Memory SRAR (hw flag, bank=4, status=$STAT_MEM_SRAR)"
require_mce_inject

echo "  pre-state:"
echo "    /proc/sys/kernel/mce_tolerant = $(cat /proc/sys/kernel/mce_tolerant 2>/dev/null || echo '?')"
echo "    /proc/sys/kernel/panic        = $(cat /proc/sys/kernel/panic 2>/dev/null || echo '?')"
echo
if ! cfi_loaded; then
    echo "  !! CFI NOT LOADED — this injection may PANIC the host."
    echo "  !! Confirm kdump/auto-reboot is configured. Press Ctrl-C within 5s to abort."
    sleep 5
fi

US0=$(mfi_stat uce_sync)
UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page hw_mem_srar
PROBE_CPU=$(safe_cpu)
dmesg_mark

mce_submit hw 4 "$STAT_MEM_SRAR" "$PADDR" 0 "$PROBE_CPU"
sleep 3

US1=$(mfi_stat uce_sync)
UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)
OWNER_ALIVE=$(kill -0 "$PFN_OWNER_PID" 2>/dev/null && echo "alive" || echo "killed (SIGBUS)")

echo
echo "  observations:"
observe_row "mfi uce_sync"       "$US0"  "$US1"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    owner process           : $OWNER_ALIVE"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
