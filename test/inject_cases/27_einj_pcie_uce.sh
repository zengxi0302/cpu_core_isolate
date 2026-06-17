#!/bin/bash
# Case 27 — PCIe UCE non-fatal via APEI EINJ
#
#   path     : EINJ -> firmware -> GHES/APEI -> PCIe AER handler (UC path)
#   type     : 0x00000080 (PCI Express Uncorrectable non-fatal)
#
#   Equivalent to case 08 (Bus UCE) but at PCIe level via EINJ.
#   PCIe UCE non-fatal is a recoverable bus error — the device/link can
#   be reset without system-wide impact.
#
#   no  CFI  : PCIe AER handler logs; device may be disabled/reset.
#   has CFI  : if routed through MCE decode chain, cfi may classify as
#              bus UCE and account it. On most platforms PCIe errors go
#              through AER, not MCE, so CFI state should be unchanged.
#
#   This case verifies EINJ PCIe UCE injection and confirms CFI's
#   behavior (account if routed, ignore if not).

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
einj_banner "27 EINJ PCIe UCE non-fatal (type=$EINJ_PCIE_UCE_NONFATAL)"
require_einj
require_einj_type "$EINJ_PCIE_UCE_NONFATAL" "PCI Express Uncorrectable non-fatal"

TC=$(safe_cpu_alt)
UC0=$(cpu_attr "$TC" uce_count)
TY0=$(cpu_attr "$TC" error_types)
ST0=$(cpu_attr "$TC" state)
dmesg_mark

einj_inject "$EINJ_PCIE_UCE_NONFATAL"
sleep 2

UC1=$(cpu_attr "$TC" uce_count)
TY1=$(cpu_attr "$TC" error_types)
ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC:"
printf "    %-26s: %s -> %s\n" "cfi state"       "$ST0" "$ST1"
printf "    %-26s: %s -> %s\n" "cfi uce_count"   "$UC0" "$UC1"
printf "    %-26s: %s -> %s\n" "cfi error_types" "$TY0" "$TY1"
printf "    %-26s: %s\n"       "online"          "$ON1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
echo
if [[ "$ST1" == "$ST0" ]]; then
    echo "  [OK] CFI state unchanged — PCIe UCE handled by AER, not CFI domain"
else
    echo "  [INFO] CFI state changed — platform routes PCIe UCE to MCE chain"
fi
