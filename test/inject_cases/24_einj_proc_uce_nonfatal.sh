#!/bin/bash
# Case 24 — Processor UCE non-fatal via APEI EINJ  ★ PANIC-vs-ISOLATE ★
#
#   path     : EINJ -> firmware -> GHES/APEI -> MCE handler
#              -> x86_mce_decoder_chain -> cfi notifier -> cpu isolation
#   type     : 0x00000002 (Processor Uncorrectable non-fatal)
#   target   : a "safe" CPU (NCPU/2) via APIC ID targeting
#
#   Equivalent to cases 04/13 (L2 cache UCE -> CPU isolation) but uses
#   APEI EINJ. This is the headline isolation case for EINJ.
#
#   EINJ Processor UCE non-fatal maps to a recoverable uncorrectable
#   processor error. The firmware creates a CPER with appropriate severity,
#   GHES processes it, and the error flows to the MCE decode chain.
#   Unlike mce-inject, there is no broadcast sync timeout — the firmware
#   handles MCE delivery properly.
#
#   no  CFI  : depending on platform behavior:
#              - Firmware may signal panic-level severity -> mce_panic
#                ("Fatal machine check") — real hardware semantics
#              - Some platforms downgrade to GHES-recoverable -> no panic
#                but no isolation either
#   has CFI  : cfi notifier classifies as processor UCE; cpu isolated per
#              isolation_mode= module param:
#                full     -> state=isolated, online=0
#                inactive -> state=isolated, online=1, active_mask cleared
#                soft     -> state=isolated, online=1, IRQ migration only
#
#   WARNING  : without CFI, platform-dependent panic risk. Confirm kdump.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
TC=$(safe_cpu)
einj_banner "24 EINJ Processor UCE non-fatal on cpu$TC (type=$EINJ_PROC_UCE_NONFATAL)"
require_einj
require_einj_type "$EINJ_PROC_UCE_NONFATAL" "Processor Uncorrectable non-fatal"

echo "  pre-state:"
echo "    /proc/sys/kernel/mce_tolerant = $(cat /proc/sys/kernel/mce_tolerant 2>/dev/null || echo '?')"
echo "    /proc/sys/kernel/panic        = $(cat /proc/sys/kernel/panic 2>/dev/null || echo '?')"
echo

einj_panic_warning

ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
UC0=$(cpu_attr "$TC" uce_count)
TY0=$(cpu_attr "$TC" error_types)
dmesg_mark

einj_inject_proc "$EINJ_PROC_UCE_NONFATAL" "$TC"
sleep 3

ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")
UC1=$(cpu_attr "$TC" uce_count)
TY1=$(cpu_attr "$TC" error_types)

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi state"       "$ST0" "$ST1"
printf "    %-26s: %s -> %s\n" "online"          "$ON0" "$ON1"
printf "    %-26s: %s -> %s\n" "cfi uce_count"   "$UC0" "$UC1"
printf "    %-26s: %s -> %s\n" "cfi error_types" "$TY0" "$TY1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

if ! dmesg_since_mark | grep -qE 'Hardware Error|GHES|mce|uncorrected|cpu_fault_isolate|machine check'; then
    echo
    echo "  [INFO] No kernel-side trace for Processor UCE non-fatal injection."
    echo "  Possible causes:"
    echo "    - FMA firmware consumed the error before kernel MCE handler"
    echo "    - BIOS doesn't support SET_ERROR_TYPE_WITH_ADDRESS (APIC targeting)"
    echo "    - Processor UCE injection not actually delivered on this platform"
    echo "  Workaround: use mce-inject cases 04/13 for CPU isolation demo"
fi

# Scheduler-exclusion check (inactive-mode signature).
if cfi_loaded && [[ "$ST1" == "isolated" ]]; then
    echo
    echo "  scheduler-exclusion check:"
    if taskset -c "$TC" true 2>/dev/null; then
        echo "    [WARN] taskset -c $TC succeeded — cpu still schedulable?"
    else
        echo "    [OK]   taskset -c $TC refused (cpu cleared from active_mask)"
    fi
fi

# Re-online if full mode took it offline.
if [[ "$ON1" == "0" && -w /sys/devices/system/cpu/cpu$TC/online ]]; then
    echo 1 > /sys/devices/system/cpu/cpu$TC/online 2>/dev/null
    sleep 1
    echo "  cpu$TC online now: $(cpu_online "$TC")"
fi
