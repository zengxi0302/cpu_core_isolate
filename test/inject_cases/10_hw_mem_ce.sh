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
#   The original draft of this case used a debugfs flags=hw "WRMSR no-op"
#   probe which gave a false negative on HCE2 physical hosts. The userspace
#   tool is the verified path — empirically delivers #MC NMI even on
#   FMA-configured Purley platforms.

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
