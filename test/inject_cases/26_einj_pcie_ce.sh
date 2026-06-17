#!/bin/bash
# Case 26 — PCIe CE via APEI EINJ (correctable, bus-level)
#
#   path     : EINJ -> firmware -> GHES/APEI -> PCIe AER handler
#   type     : 0x00000040 (PCI Express Correctable)
#
#   Rough equivalent to case 08 (Bus UCE) but at PCIe CE level via EINJ.
#   PCIe errors go through a different kernel path than processor/memory:
#   firmware reports via GHES, kernel routes to PCIe AER (Advanced Error
#   Reporting) handler. CFI may or may not see these depending on whether
#   the platform routes them through the MCE decode chain.
#
#   no  CFI  : PCIe AER handler logs; no isolation.
#   has CFI  : if the error flows through MCE decode chain, cfi may account
#              it. Otherwise, EINJ PCIe errors are outside CFI's domain and
#              this case verifies that CFI does NOT false-positive on them.
#
#   This case does not target a specific CPU — PCIe errors are device-scoped.
#   It primarily verifies that EINJ PCIe injection works and CFI handles
#   (or correctly ignores) the resulting error.

set -u
cd "$(dirname "$0")/../.."
source test/inject_cases/env_einj.sh
einj_banner "26 EINJ PCIe CE (type=$EINJ_PCIE_CE)"
require_einj
require_einj_type "$EINJ_PCIE_CE" "PCI Express Correctable"

TC=$(safe_cpu_alt)
CE0=$(cpu_attr "$TC" ce_count)
UC0=$(cpu_attr "$TC" uce_count)
ST0=$(cpu_attr "$TC" state)
dmesg_mark

einj_inject "$EINJ_PCIE_CE"
sleep 2

CE1=$(cpu_attr "$TC" ce_count)
UC1=$(cpu_attr "$TC" uce_count)
ST1=$(cpu_attr "$TC" state)
ON1=$(cpu_online "$TC")

echo
echo "  observations on cpu$TC (monitoring for false positives):"
printf "    %-26s: %s -> %s\n" "cfi state"     "$ST0" "$ST1"
printf "    %-26s: %s -> %s\n" "cfi ce_count"  "$CE0" "$CE1"
printf "    %-26s: %s -> %s\n" "cfi uce_count" "$UC0" "$UC1"
printf "    %-26s: %s\n"       "online"        "$ON1"
echo "    dmesg (relevant):"
dmesg_digest | sed 's/^/      /'
echo
if [[ "$ST1" == "$ST0" ]]; then
    echo "  [OK] CFI state unchanged — PCIe CE correctly outside CFI domain (or not routed)"
else
    echo "  [INFO] CFI state changed — platform routes PCIe errors to MCE chain"
fi
