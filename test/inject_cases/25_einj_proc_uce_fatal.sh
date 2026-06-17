#!/bin/bash
# Case 25 — Processor UCE fatal via APEI EINJ  ★ DANGER — almost certainly panics ★
#
#   path     : EINJ -> firmware -> GHES/APEI -> MCE handler (fatal path)
#   type     : 0x00000004 (Processor Uncorrectable fatal)
#   target   : a "safe" CPU (NCPU/2) via APIC ID targeting
#
#   No direct equivalent in the mce-inject suite. This is the most severe
#   processor error: firmware signals a fatal, non-recoverable error on
#   the targeted core. The kernel's MCE handler sees PCC=1 severity and
#   calls mce_panic unconditionally ("Fatal machine check on current CPU").
#
#   no  CFI  : unconditional mce_panic -> kdump. This is the real hardware
#              panic path — no sync timeout artifact, pure severity-driven.
#   has CFI  : CFI's tolerant=3 may gate off the panic on some kernels,
#              but PCC=1 (processor context corrupt) is the boundary case
#              documented in env.sh: "内核态 AR 严重性事件 — 无论 tolerant
#              设多少都 panic". Result depends on exact firmware behavior:
#              - If firmware sets no_way_out (PCC=1): panic even with CFI
#              - If firmware sets recoverable bits: CFI can intercept
#
#   This case is primarily for boundary testing. Use case 24 (non-fatal)
#   for the standard "panic-vs-isolate" demo.
#
#   WARNING  : this WILL almost certainly panic the host, with or without
#              CFI. Only run with kdump + auto-reboot configured.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
TC=$(safe_cpu)
einj_banner "25 EINJ Processor UCE fatal on cpu$TC (type=$EINJ_PROC_UCE_FATAL)"
require_einj
require_einj_type "$EINJ_PROC_UCE_FATAL" "Processor Uncorrectable fatal"

echo "  pre-state:"
echo "    /proc/sys/kernel/mce_tolerant = $(cat /proc/sys/kernel/mce_tolerant 2>/dev/null || echo '?')"
echo "    /proc/sys/kernel/panic        = $(cat /proc/sys/kernel/panic 2>/dev/null || echo '?')"
echo

echo "  !! EINJ Processor UCE Fatal — this is the most severe error type."
echo "  !! PCC=1 (processor context corrupt) means the kernel has NO safe"
echo "  !! way to continue. Panic is expected EVEN WITH CFI loaded."
echo "  !! This tests the boundary of CFI's panic suppression capability."
echo "  !! Confirm kdump + auto-reboot. Press Ctrl-C within 10s to abort."
sleep 10

UC0=$(cpu_attr "$TC" uce_count)
ST0=$(cpu_attr "$TC" state)
ON0=$(cpu_online "$TC")
dmesg_mark

einj_inject_proc "$EINJ_PROC_UCE_FATAL" "$TC"
sleep 3

# If we reach here, the kernel survived (possible on some platforms).
UC1=$(cpu_attr "$TC" uce_count)
ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")

echo
echo "  !! Host survived Processor UCE Fatal — unexpected on most platforms."
echo "  !! Check dmesg for how the error was processed."
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
