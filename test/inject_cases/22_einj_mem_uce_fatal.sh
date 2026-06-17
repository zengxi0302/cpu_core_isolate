#!/bin/bash
# Case 22 — Memory UCE fatal via APEI EINJ  ★ PANIC-vs-ISOLATE ★
#
#   path     : EINJ -> firmware -> GHES/APEI -> memory_failure (sync, AR)
#   type     : 0x00000020 (Memory Uncorrectable fatal)
#   target   : a self-owned 4 KiB anonymous page
#
#   Equivalent to cases 03/12 (SRAR) but uses APEI EINJ.
#   EINJ Memory UCE fatal is the strongest memory error type: firmware
#   signals that the error requires immediate action (action-required).
#   This is the EINJ equivalent of mce-inject SRAR (UC|S|AR).
#
#   no  CFI  : kernel processes the fatal error via GHES; depending on
#              platform firmware behavior, this may trigger:
#              - Direct mce_panic via severity-driven path (real hardware
#                semantics, NOT the broadcast sync timeout artifact)
#              - memory_failure with MF_ACTION_REQUIRED -> SIGBUS or panic
#              Either way, without CFI this is expected to crash the host.
#   has CFI  : cfi's tolerant=3 + panic notifier suppress the panic path;
#              memory_failure fires on the owning task; SIGBUS delivered;
#              page hard-offlined; HOST SURVIVES.
#
#   Advantage over mce-inject: the panic is severity-driven (real hardware
#   semantics), not the "Some CPUs didn't answer in synchronization"
#   artifact from mce-inject's single-CPU raise.
#
#   WARNING  : without CFI this WILL panic the host. Confirm:
#                - kdump configured (systemctl status kdump)
#                - /proc/sys/kernel/panic > 0 for auto-reboot

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
einj_banner "22 EINJ Memory UCE fatal (type=$EINJ_MEM_UCE_FATAL)"
require_einj
require_einj_type "$EINJ_MEM_UCE_FATAL" "Memory Uncorrectable fatal"

echo "  pre-state:"
echo "    /proc/sys/kernel/mce_tolerant = $(cat /proc/sys/kernel/mce_tolerant 2>/dev/null || echo '?')"
echo "    /proc/sys/kernel/panic        = $(cat /proc/sys/kernel/panic 2>/dev/null || echo '?')"
echo

einj_panic_warning

US0=$(mfi_stat uce_consumed)
UA0=$(mfi_stat uce_async)
OFF0=$(mfi_stat pages_offlined)
own_page einj_mem_uce_fatal
dmesg_mark

einj_inject_mem "$EINJ_MEM_UCE_FATAL" "$PADDR"
sleep 5

US1=$(mfi_stat uce_consumed)
UA1=$(mfi_stat uce_async)
OFF1=$(mfi_stat pages_offlined)
OWNER_ALIVE=$(kill -0 "$PFN_OWNER_PID" 2>/dev/null && echo "alive (async kill may still be pending)" || echo "killed")

echo
echo "  observations:"
observe_row "mfi uce_consumed"   "$US0"  "$US1"
observe_row "mfi uce_async"      "$UA0"  "$UA1"
observe_row "mfi pages_offlined" "$OFF0" "$OFF1"
echo "    owner process           : $OWNER_ALIVE"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
