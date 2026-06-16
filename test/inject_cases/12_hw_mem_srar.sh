#!/bin/bash
# Case 12 — Real #MC Memory SRAR (mce-inject userspace tool)  ★ PANIC-vs-ISOLATE ★
#
#   path     : mce-inject(8) -> raise -> do_machine_check -> mce_severity
#              -> AR severity (UC|S|AR)
#   bank     : 4
#   status   : 0xbd80000000000094  (VAL|UC|EN|MISCV|ADDRV|S|AR)
#
#   no  CFI  : the kernel MCE handler broadcasts to all CPUs; mce-inject only
#              raises on one. Other CPUs time out ("Some CPUs didn't answer
#              in synchronization") and with mce_tolerant<3 the kernel calls
#              mce_panic -> Fatal machine check -> kdump.
#              == VERIFIED kdump on HCE2 + Huawei 2288H V5 physical host ==
#   has CFI  : cfi_init() raised mce_tolerant to 3, which gates off the
#              mce_panic broadcast-sync timeout branch. do_machine_check
#              returns cleanly; decode chain delivers the event to cfi
#              notifier; SRAR routed to sync memory_failure (or async fallback)
#              on the owning task; page hard-offlined; HOST SURVIVES.
#
#   WARNING  : without CFI this WILL panic the host. Confirm:
#                - kdump configured (systemctl status kdump)
#                - /proc/sys/kernel/panic > 0 for auto-reboot
#              The script gives a 5s Ctrl-C window when CFI is not loaded.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
status_banner "12 hw Memory SRAR (mce-inject userspace tool, bank=4)"
require_mce_inject

echo "  pre-state:"
echo "    /proc/sys/kernel/mce_tolerant = $(cat /proc/sys/kernel/mce_tolerant 2>/dev/null || echo '?')"
echo "    /proc/sys/kernel/panic        = $(cat /proc/sys/kernel/panic 2>/dev/null || echo '?')"
echo
if ! cfi_loaded; then
    echo "  !! CFI NOT LOADED — this injection is expected to PANIC the host."
    echo "  !! Confirm kdump/auto-reboot is configured. Press Ctrl-C within 5s to abort."
    sleep 5
fi

US0=$(mfi_stat uce_sync)
UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page hw_mem_srar
PROBE_CPU=$(safe_cpu)
dmesg_mark

mce_inject_file "$PROBE_CPU" 4 "$STAT_MEM_SRAR" "$PADDR" 0x0
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
