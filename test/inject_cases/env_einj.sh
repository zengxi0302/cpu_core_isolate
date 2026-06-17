#!/bin/bash
# Shared environment for APEI EINJ-based test cases (test/inject_cases/2x_einj_*.sh)
#
# Source from each EINJ case script. Provides:
#   - EINJ sysfs paths and available_error_type detection
#   - einj_inject / einj_inject_mem / einj_inject_proc helpers
#   - Reuses env.sh for sysfs paths, dmesg helpers, observe_* functions
#
# APEI EINJ vs mce-inject:
#   mce-inject writes MCi_STATUS directly and raises #MC via WRMSR+IPI-NMI
#   on a single CPU. Other CPUs miss the broadcast sync -> mce_panic timeout.
#   EINJ goes through firmware (ACPI EINJ action table): the platform's
#   error injection hardware creates a real error record, GHES/APEI processes
#   it, and the kernel's native error handling path fires. No broadcast sync
#   timeout artifact — if the system panics, it's the real severity-driven
#   mce_panic, matching actual hardware failure semantics.
#
# Requirements:
#   - BIOS must expose ACPI EINJ table (check: dmesg | grep EINJ)
#   - Kernel built with CONFIG_ACPI_APEI_EINJ=m or =y
#   - modprobe einj (if built as module)
#   - Some error types require param1/param2/param3 support (kernel 4.17+)

# Pull in the base env.sh for sysfs paths, dmesg helpers, mfi_stat, etc.
source "$(dirname "$0")/env.sh"

EINJ_DIR=/sys/kernel/debug/apei/einj

# ACPI EINJ error type constants (ACPI spec Table 18-393)
EINJ_PROC_CE=0x00000001           # Processor Correctable
EINJ_PROC_UCE_NONFATAL=0x00000002 # Processor Uncorrectable non-fatal
EINJ_PROC_UCE_FATAL=0x00000004    # Processor Uncorrectable fatal
EINJ_MEM_CE=0x00000008            # Memory Correctable
EINJ_MEM_UCE_NONFATAL=0x00000010  # Memory Uncorrectable non-fatal
EINJ_MEM_UCE_FATAL=0x00000020     # Memory Uncorrectable fatal
EINJ_PCIE_CE=0x00000040           # PCI Express Correctable
EINJ_PCIE_UCE_NONFATAL=0x00000080 # PCI Express Uncorrectable non-fatal
EINJ_PCIE_UCE_FATAL=0x00000100    # PCI Express Uncorrectable fatal

# EINJ SET_ERROR_TYPE_WITH_ADDRESS flags
EINJ_FLAG_PROC_APIC_VALID=0x1     # param3 = APIC ID
EINJ_FLAG_MEM_ADDR_VALID=0x2      # param1 = phys addr, param2 = mask
EINJ_FLAG_PCIE_SBDF_VALID=0x4     # param3 = SBDF

# Override inject mode display for EINJ cases
INJECT_MODE=einj

# Override status_banner to show EINJ mode
einj_banner() {
    echo "================================================================"
    if cfi_loaded; then
        echo "  CFI status : LOADED  (cpu_fault_isolate present)"
    else
        echo "  CFI status : NOT LOADED"
    fi
    echo "  kernel     : $(uname -r)"
    echo "  case       : $1"
    echo "  inject mode: APEI EINJ (firmware-level hardware error injection)"
    echo "  einj dir   : $EINJ_DIR"
    echo "  artifacts  : $LOGDIR/"
    echo "================================================================"
}

# Preflight: EINJ must be available.
require_einj() {
    if [[ ! -d $EINJ_DIR ]]; then
        modprobe einj 2>/dev/null
        sleep 1
    fi
    if [[ ! -d $EINJ_DIR ]]; then
        echo "[ABORT] $EINJ_DIR missing — BIOS may not expose ACPI EINJ table"
        echo "  Check: dmesg | grep EINJ"
        echo "  Check: CONFIG_ACPI_APEI_EINJ in kernel config"
        exit 1
    fi
    if [[ ! -f $EINJ_DIR/error_type ]]; then
        echo "[ABORT] $EINJ_DIR/error_type not found — EINJ interface incomplete"
        exit 1
    fi
}

# Check if a specific error type is supported by the platform.
# Usage: einj_type_available 0x00000008
einj_type_available() {
    local etype=$1
    local avail
    avail=$(cat $EINJ_DIR/available_error_type 2>/dev/null)
    if [[ -z "$avail" ]]; then
        echo "[WARN] cannot read available_error_type"
        return 1
    fi
    if echo "$avail" | grep -q "$(printf '0x%08x' "$etype")"; then
        return 0
    fi
    local avail_hex
    avail_hex=$(printf '0x%x' "$((avail & etype))")
    [[ "$avail_hex" != "0x0" ]] && return 0
    return 1
}

# Require a specific EINJ error type, abort if not available.
require_einj_type() {
    local etype=$1 desc=$2
    if ! einj_type_available "$etype"; then
        echo "[ABORT] EINJ error type $desc ($(printf '0x%08x' "$etype")) not available"
        echo "  Available types:"
        cat $EINJ_DIR/available_error_type 2>/dev/null | sed 's/^/    /'
        exit 1
    fi
    echo "  EINJ type $(printf '0x%08x' "$etype") ($desc) is available"
}

# Get the APIC ID for a given logical CPU number.
# Usage: cpu_apicid <cpu_number>
cpu_apicid() {
    local cpu=$1
    local apicid
    # Try /sys/devices/system/cpu/cpuN/topology/initial_apicid first
    apicid=$(cat /sys/devices/system/cpu/cpu$cpu/topology/initial_apicid 2>/dev/null)
    if [[ -n "$apicid" ]]; then
        echo "$apicid"
        return
    fi
    # Fallback: parse /proc/cpuinfo
    apicid=$(awk -v cpu=$cpu '
        /^processor/ { p=$3 }
        /^apicid/ && p==cpu { print $3; exit }
    ' /proc/cpuinfo)
    if [[ -n "$apicid" ]]; then
        echo "$apicid"
        return
    fi
    echo "[WARN] cannot determine APIC ID for cpu$cpu" >&2
    echo "0"
}

# Core EINJ injection function.
# Usage: einj_inject <error_type_hex> [flags] [param1] [param2] [param3] [param4]
#
# All params are optional; only written if non-empty. The error_inject
# trigger is always written last.
einj_inject() {
    local etype=$1
    local flags=${2:-}
    local param1=${3:-}
    local param2=${4:-}
    local param3=${5:-}
    local param4=${6:-}

    echo "$etype" > $EINJ_DIR/error_type
    [[ -n "$flags"  && -f $EINJ_DIR/flags  ]] && echo "$flags"  > $EINJ_DIR/flags
    [[ -n "$param1" && -f $EINJ_DIR/param1 ]] && echo "$param1" > $EINJ_DIR/param1
    [[ -n "$param2" && -f $EINJ_DIR/param2 ]] && echo "$param2" > $EINJ_DIR/param2
    [[ -n "$param3" && -f $EINJ_DIR/param3 ]] && echo "$param3" > $EINJ_DIR/param3
    [[ -n "$param4" && -f $EINJ_DIR/param4 ]] && echo "$param4" > $EINJ_DIR/param4

    echo 1 > $EINJ_DIR/error_inject
    local rc=$?
    local desc="type=$(printf '0x%08x' "$etype")"
    [[ -n "$flags" ]]  && desc="$desc flags=$flags"
    [[ -n "$param1" ]] && desc="$desc param1=$param1"
    [[ -n "$param2" ]] && desc="$desc param2=$param2"
    [[ -n "$param3" ]] && desc="$desc param3=$param3"
    echo "  injected (EINJ): $desc  [rc=$rc]"
    return $rc
}

# Memory error injection with address targeting.
# Usage: einj_inject_mem <error_type_hex> <phys_addr_hex> [mask_hex]
#
# Sets flags=0x2 (memory address valid), param1=addr, param2=mask.
# Default mask = 0xfffffffffffff000 (4K page granularity).
einj_inject_mem() {
    local etype=$1 paddr=$2
    local mask=${3:-0xfffffffffffff000}
    einj_inject "$etype" "$EINJ_FLAG_MEM_ADDR_VALID" "$paddr" "$mask" "" ""
}

# Processor error injection with CPU targeting via APIC ID.
# Usage: einj_inject_proc <error_type_hex> <cpu_number>
#
# Sets flags=0x1 (APIC valid), param3=APIC_ID.
# Requires param3 support in kernel (4.17+).
einj_inject_proc() {
    local etype=$1 cpu=$2
    local apicid
    apicid=$(cpu_apicid "$cpu")
    if [[ ! -f $EINJ_DIR/param3 ]]; then
        echo "  [WARN] $EINJ_DIR/param3 not available — injecting without APIC targeting"
        echo "  (error will hit whichever CPU the firmware chooses)"
        einj_inject "$etype" "" "" "" "" ""
        return $?
    fi
    einj_inject "$etype" "$EINJ_FLAG_PROC_APIC_VALID" "" "" "$apicid" ""
}

# Processor error injection with both APIC and memory address.
# Usage: einj_inject_proc_addr <error_type_hex> <cpu_number> <phys_addr_hex> [mask_hex]
#
# Sets flags=0x3 (APIC + memory address valid).
einj_inject_proc_addr() {
    local etype=$1 cpu=$2 paddr=$3
    local mask=${4:-0xfffffffffffff000}
    local apicid
    apicid=$(cpu_apicid "$cpu")
    local flags=$(( EINJ_FLAG_PROC_APIC_VALID | EINJ_FLAG_MEM_ADDR_VALID ))
    if [[ ! -f $EINJ_DIR/param3 ]]; then
        echo "  [WARN] param3 unavailable — falling back to memory-only targeting"
        einj_inject_mem "$etype" "$paddr" "$mask"
        return $?
    fi
    einj_inject "$etype" "$(printf '0x%x' $flags)" "$paddr" "$mask" "$apicid" ""
}

# Warn before injecting a fatal/UC error without CFI loaded.
einj_panic_warning() {
    cfi_loaded && return 0
    echo
    echo "  !! EINJ fatal/UC injection + CFI NOT LOADED — kernel panic expected."
    echo "  !! Unlike mce-inject, EINJ goes through firmware — the panic is"
    echo "  !! severity-driven (real hardware semantics), not a sync timeout artifact."
    echo "  !! Confirm kdump configured and kernel.panic > 0. Ctrl-C within 5s to abort."
    sleep 5
}

# Show available EINJ error types (human-readable).
einj_show_available() {
    echo "  available EINJ error types:"
    if [[ -f $EINJ_DIR/available_error_type ]]; then
        cat $EINJ_DIR/available_error_type | sed 's/^/    /'
    else
        echo "    (cannot read)"
    fi
}
