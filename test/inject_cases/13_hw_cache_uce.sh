#!/bin/bash
# Case 13 — Real #MC L2 Cache UCE (mce-inject userspace tool)  ★ PANIC-vs-ISOLATE ★
#
#   path     : mce-inject(8) -> raise -> do_machine_check -> mce_severity
#              -> uncorrected cache error
#   bank     : 1
#   status   : 0xb80000000000000e  (VAL|UC|EN|MISCV | simple-cache L2 0x000E)
#   target   : cpu picked by safe_cpu() (NCPU/2)
#
#   no  CFI  : Intel MCE handler broadcasts; mce-inject only raises on one cpu.
#              Other cpus -> "Some CPUs didn't answer in synchronization"
#              -> mce_reign -> mce_panic -> Fatal machine check -> kdump.
#              == VERIFIED kdump on HCE2 + 2288H V5 ==
#              (vmcore-dmesg shows: mce_panic -> mce_reign -> mce_end
#               -> do_machine_check -> raise_exception [mce_inject].)
#   has CFI  : mce_tolerant=3 gates off the sync-timeout panic branch.
#              do_machine_check returns cleanly. Decode chain delivers UC
#              to cfi notifier; cfi classifies as cache UCE; cpu isolated
#              per isolation_mode=:
#                inactive (DEFAULT): set_cpu_active(cpuN, false), IRQs
#                                    migrated, cpu stays online.
#                                    Verified: ~462 IRQs migrated on the
#                                    physical host; host stays up;
#                                    taskset -c <cpuN> refuses bind.
#                full              : real cpu_down. NEEDS the
#                                    livepatch_percpu_counter hot-patch
#                                    on HCE2 r2673_211_284 — otherwise
#                                    cpu_dead callback hard-lockup at
#                                    percpu_counter_cpu_dead+0x44.
#
#   WARNING  : without CFI this WILL panic the host.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
TC=$(safe_cpu)
status_banner "13 hw L2 Cache UCE on cpu$TC (mce-inject userspace tool, bank=1)"
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

UC0=$(cpu_attr "$TC" uce_count)
ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
dmesg_mark

mce_inject_file "$TC" 1 0xb80000000000000e 0xdeadbeef000 0x0
sleep 3

UC1=$(cpu_attr "$TC" uce_count)
ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi state"     "$ST0" "$ST1"
printf "    %-26s: %s -> %s\n" "online"        "$ON0" "$ON1"
printf "    %-26s: %s -> %s\n" "cfi uce_count" "$UC0" "$UC1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

# In inactive mode the cpu stays online; the scheduler-exclusion check is
# the real "is this cpu actually isolated" signal — taskset must refuse.
if cfi_loaded && [[ "$ST1" == "isolated" ]]; then
    echo
    echo "  scheduler-exclusion check (inactive mode signature):"
    if taskset -c "$TC" true 2>/dev/null; then
        echo "    [WARN] taskset -c $TC succeeded — cpu still in active_mask?"
    else
        echo "    [OK]   taskset -c $TC refused (cpu cleared from active_mask)"
    fi
fi

# Full-mode safety: re-online if the cpu was actually hot-unplugged.
if [[ "$ON1" == "0" && -w /sys/devices/system/cpu/cpu$TC/online ]]; then
    echo 1 > /sys/devices/system/cpu/cpu$TC/online 2>/dev/null
    sleep 1
    echo "  cpu$TC online now: $(cpu_online "$TC")"
fi
