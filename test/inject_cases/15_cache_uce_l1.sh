#!/bin/bash
# Case 15 — L1 (L1D) Cache UCE -> CPU isolation
#
#   path     : mce_inject sw -> x86_mce_decoder_chain (default)
#              --hw -> mce-inject(8) raise -> #MC NMI -> do_machine_check
#   bank     : 1 (DCU = L1 data cache on Intel Skylake-SP / Cascade Lake-SP)
#   status   : 0xb00000000000000d  (VAL|UC|EN | simple-cache 0x000D = L1)
#              The cfi classifier treats simple-code L1 as L1D by default
#              (tt=GENERIC, which is not TT_INSTR). For an L1I-specific
#              test see case 16.
#   target   : a "safe" CPU (NCPU/2)
#
#   no  CFI  : decode chain runs; EDAC may log; no isolation.
#   has CFI  : cfi classifies as L1D cache UCE. L1 is per-core, so isolating
#              the reporting CPU is the correct response (unlike L3, which
#              is socket-shared — see case 05). Behavior follows
#              isolation_mode= module param:
#                full     : real cpu_down (NEEDS livepatch_percpu_counter
#                           on HCE2 r2673*)
#                inactive : set_cpu_active(false) + IRQ migration (CPU
#                           stays online but unschedulable) — DEFAULT
#                soft     : IRQ migration only
#
#   Note: in --hw mode the panic suppression depends on CFI's tolerant=3 +
#   panic_notifier; without CFI loaded, expect a panic (cpu local UCE is
#   AR-severity-equivalent for the broadcast sync timeout case).

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
TC=$(safe_cpu)
status_banner "15 L1 (L1D) Cache UCE on cpu$TC (bank=1 DCU, status=$STAT_CACHE_UCE_L1)"
require_mce_inject
hw_panic_warning

ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
UC0=$(cpu_attr "$TC" uce_count)
TY0=$(cpu_attr "$TC" error_types)
dmesg_mark

mce_inject_dispatch "$TC" 1 "$STAT_CACHE_UCE_L1" 0 0
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
