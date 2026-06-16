#!/bin/bash
# Case 04 — L2 Cache UCE on a specific target CPU (the headline isolation case)
#
#   path     : mce_inject sw  ->  x86_mce_decoder_chain
#   bank     : 3
#   status   : 0xb00000000000000e  (VAL|UC|EN | simple-cache 0x000E = L2)
#   target   : a "safe" CPU (NCPU/2) — not cpu0, not the last cpu
#
#   no  CFI  : sw injection bypasses do_machine_check; decode chain runs but
#              EDAC just logs. CPU stays online and active. No isolation.
#              (For real "kernel panic without CFI" demo see 13_hw_cache_uce.sh.)
#   has CFI  : cfi notifier classifies as cache UCE; according to
#              isolation_mode= module param the CPU goes:
#                full     -> state=isolated, online=0       (real cpu_down)
#                inactive -> state=isolated, online=1, active_mask cleared
#                soft     -> state=isolated, online=1, IRQ migration only

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
TC=$(safe_cpu)
status_banner "04 L2 Cache UCE on cpu$TC (sw, bank=3, status=$STAT_CACHE_UCE_L2)"
require_mce_inject

ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
UC0=$(cpu_attr "$TC" uce_count)
dmesg_mark

mce_submit sw 3 "$STAT_CACHE_UCE_L2" 0 0 "$TC"
sleep 3

ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")
UC1=$(cpu_attr "$TC" uce_count)

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi state"       "$ST0" "$ST1"
printf "    %-26s: %s -> %s\n" "online"          "$ON0" "$ON1"
printf "    %-26s: %s -> %s\n" "cfi uce_count"   "$UC0" "$UC1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

# Re-online if full mode took it offline.
if [[ "$ON1" == "0" && -w /sys/devices/system/cpu/cpu$TC/online ]]; then
    echo 1 > /sys/devices/system/cpu/cpu$TC/online 2>/dev/null
    sleep 1
    echo "  cpu$TC online now: $(cpu_online "$TC")"
fi
