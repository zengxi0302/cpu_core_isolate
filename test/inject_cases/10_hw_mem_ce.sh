#!/bin/bash
# Case 10 — Real #MC Memory CE (via mce-inject(8) userspace tool)
#
#   path     : mce-inject(8) -> debugfs (flags=raise) -> WRMSR MCi_STATUS
#              -> IPI-NMI raise -> do_machine_check -> corrected handler
#              -> x86_mce_decoder_chain
#   bank     : 4
#   status   : 0x9c00000000000094  (VAL|EN|MISCV|ADDRV | mem 0x094)
#
#   no  CFI  : real #MC corrected-path; EDAC logs; no panic (CE).
#   has CFI  : same path + cfi mfi CE accounting + netlink.
#
#   PLATFORM LIMITATION — HCE2 + Huawei 2288H V5 (and other FMA-firmware-
#   first platforms): CE injection via mce-inject(8) raise goes through
#   CMC IRQ -> machine_check_poll on the target CPU. FMA / vendor SMM
#   scrubs MCi_STATUS before the poll reads it back, so the kernel sees
#   nothing and CFI's ce_total never moves. Expected output is
#   "mfi ce_total: 0 -> 0   [delta=0]" on this platform — that is NOT a
#   script bug, it's the firmware-first interception of the CMC path.
#   For memory CE testing on HCE2 use case 01 (sw flag).
#   The hw CE path works on platforms without FMA (e.g. cloud KVM with
#   full MCE virtualization), which is why this case is kept.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env.sh
parse_inject_args "$@"
INJECT_MODE=hw   # this case is always real-#MC; --sw is ignored
status_banner "10 hw Memory CE (mce-inject userspace tool, bank=4)"
require_mce_inject

CE0=$(mfi_stat ce_total)
own_page hw_mem_ce
PROBE_CPU=$(safe_cpu)
dmesg_mark

mce_inject_file "$PROBE_CPU" 4 "$STAT_MEM_CE" "$PADDR" 0x0
sleep 3

CE1=$(mfi_stat ce_total)

echo
echo "  observations:"
observe_row "mfi ce_total" "$CE0" "$CE1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'

own_page_release
