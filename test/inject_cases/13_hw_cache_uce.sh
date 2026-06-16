#!/bin/bash
# Case 13 — Real #MC L2 Cache UCE (hw mode)  ★ CRASH-vs-ISOLATE demo ★
#
#   path     : mce_inject hw  ->  do_machine_check -> mce_severity()
#              -> non-memory UC error (no ADDRV/MISCV)
#              -> severity = MCE_AR_SEVERITY or higher
#   bank     : 3
#   status   : 0xb00000000000000e  (VAL|UC|EN | L2)
#
#   no  CFI  : kernel sees a UC machine check it cannot attribute to a
#              specific page. With mce_tolerant<3 -> mce_panic.
#              -> WHOLE MACHINE PANIC + KDUMP.
#   has CFI  : MCE panic_notifier intercepts; cfi notifier sees cache UCE
#              on cpuX; isolates cpuX (full/inactive/soft per module param);
#              machine survives.
#
#   prereq   : hw-mode delivery functional.
#   warning  : without CFI this WILL panic the host; ensure kdump.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
TC=$(safe_cpu)
status_banner "13 hw L2 Cache UCE on cpu$TC (hw flag, bank=3, status=$STAT_CACHE_UCE_L2)"
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

UC0=$(cpu_attr "$TC" uce_count)
ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
dmesg_mark

mce_submit hw 3 "$STAT_CACHE_UCE_L2" 0 0 "$TC"
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

if [[ "$ON1" == "0" && -w /sys/devices/system/cpu/cpu$TC/online ]]; then
    echo 1 > /sys/devices/system/cpu/cpu$TC/online 2>/dev/null
    sleep 1
    echo "  cpu$TC online now: $(cpu_online "$TC")"
fi
