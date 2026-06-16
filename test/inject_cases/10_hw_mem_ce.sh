#!/bin/bash
# Case 10 — Real #MC Memory CE (hw mode)
#
#   path     : mce_inject hw  ->  prepare_msrs(WRMSR)  ->  raise #MC
#              -> do_machine_check -> __mc_scan_banks -> mce_severity
#              -> CMC handler -> x86_mce_decoder_chain
#   bank     : 4
#   status   : 0x9c00000000000094  (mem CE)
#
#   prereq   : hw-mode must actually deliver MCEs on this host. Some HCE2
#              physical kernels / KVM guests silently no-op WRMSR to
#              MCi_STATUS; this script will detect that and abort early.
#
#   no  CFI  : real #MC corrected-handler path; EDAC logs; no panic (CE).
#   has CFI  : same path, plus cfi notifier accounting + mfi page CE
#              accounting + netlink event.
#
#   This case (and 11/12/13/14) is the "hw" counterpart of cases 01-06 and
#   exists primarily to prove the do_machine_check prologue is exercised.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
status_banner "10 hw Memory CE (hw flag, bank=4, status=$STAT_MEM_CE)"
require_mce_inject

# Probe hw-mode delivery: write MCi_STATUS via mce-inject hw, then watch
# whether dmesg gets a Hardware Error log line or a "unchecked MSR access" #GP.
dmesg -c >/dev/null 2>&1
own_page hw_mem_ce_probe
PROBE_CPU=$(safe_cpu)
mce_submit hw 4 "$STAT_MEM_CE" "$PADDR" 0 "$PROBE_CPU"
sleep 2

if dmesg | grep -qE "unchecked MSR access error: WRMSR to 0x4"; then
    echo "  [ABORT] hw-mode unavailable: WRMSR MSR_IA32_MCi_STATUS triggered #GP"
    echo "          (vendor kernel / KVM blocks user-space MCE injection)"
    own_page_release
    exit 0
fi
if ! dmesg | grep -qE "mce: \[Hardware Error\]"; then
    echo "  [ABORT] hw-mode unavailable: no #MC delivered (WRMSR silently no-op'd)"
    echo "          For the demo path see cases 01-06 (sw mode)."
    own_page_release
    exit 0
fi

CE0=$(mfi_stat ce_total)
dmesg_mark
mce_submit hw 4 "$STAT_MEM_CE" "$PADDR" 0 "$PROBE_CPU"
sleep 2
CE1=$(mfi_stat ce_total)

echo
echo "  observations:"
observe_row "mfi ce_total" "$CE0" "$CE1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
