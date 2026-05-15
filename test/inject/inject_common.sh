#!/bin/bash
# CPU Fault Isolation - Error Injection Test Script
# Supports both x86 (mce-inject / EINJ) and ARM64 (EINJ / debugfs)
#
# Usage:
#   ./inject_common.sh --cpu <N> --type <ce|uce> --level <l1|l2|l3> [--count <N>]
#
# Examples:
#   ./inject_common.sh --cpu 4 --type ce --level l2 --count 15
#   ./inject_common.sh --cpu 0 --type uce --level l3

set -euo pipefail

CFI_DEBUGFS="/sys/kernel/debug/cfi/inject"
EINJ_PATH="/sys/kernel/debug/apei/einj"

CPU=0
TYPE="ce"
LEVEL="l2"
COUNT=1

usage() {
    echo "Usage: $0 --cpu <N> --type <ce|uce> --level <l1|l2|l3> [--count <N>]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpu)   CPU="$2"; shift 2;;
        --type)  TYPE="$2"; shift 2;;
        --level) LEVEL="$2"; shift 2;;
        --count) COUNT="$2"; shift 2;;
        *)       usage;;
    esac
done

detect_arch() {
    local arch
    arch=$(uname -m)
    case "$arch" in
        x86_64)  echo "x86";;
        aarch64) echo "arm64";;
        *)       echo "unknown";;
    esac
}

# Map our level/type to CFI error type values
map_cfi_type() {
    local level="$1"
    local type="$2"
    case "$level" in
        l1)
            if [[ "$type" == "ce" ]]; then
                echo "cache_l1d ce"
            else
                echo "cache_l1d ucr"
            fi
            ;;
        l2) echo "cache_l2 $([[ "$type" == "ce" ]] && echo ce || echo ucr)";;
        l3) echo "cache_l3 $([[ "$type" == "ce" ]] && echo ce || echo ucr)";;
    esac
}

# Method 1: CFI debugfs software injection (works on both arches)
inject_via_debugfs() {
    local cfi_type severity
    read -r cfi_type severity <<< "$(map_cfi_type "$LEVEL" "$TYPE")"

    if [[ ! -w "$CFI_DEBUGFS" ]]; then
        echo "ERROR: CFI debugfs inject interface not available at $CFI_DEBUGFS"
        echo "Ensure cpu_fault_isolate module is loaded."
        return 1
    fi

    echo "Injecting via CFI debugfs: cpu=$CPU type=$cfi_type severity=$severity count=$COUNT"
    for ((i=1; i<=COUNT; i++)); do
        echo "cpu=$CPU type=$cfi_type severity=$severity" > "$CFI_DEBUGFS"
        echo "  Injected $i/$COUNT"
    done
}

# Method 2: ACPI EINJ (requires firmware support)
inject_via_einj() {
    if [[ ! -d "$EINJ_PATH" ]]; then
        echo "EINJ not available. Falling back to debugfs injection."
        inject_via_debugfs
        return
    fi

    local err_type
    case "$TYPE" in
        ce)  err_type=0x00000008;;   # Processor Correctable
        uce) err_type=0x00000004;;   # Processor Uncorrectable non-fatal
    esac

    echo "Injecting via ACPI EINJ: type=$err_type"
    echo "$err_type" > "$EINJ_PATH/error_type"
    echo "1" > "$EINJ_PATH/error_inject"
    echo "EINJ injection triggered"
}

# Method 3: x86 mce-inject (x86 only)
inject_via_mce_inject() {
    if ! command -v mce-inject &>/dev/null; then
        echo "mce-inject not found. Falling back to debugfs injection."
        inject_via_debugfs
        return
    fi

    local status_uc=""
    local mca_code
    case "$LEVEL" in
        l1) mca_code="0x000D";;
        l2) mca_code="0x000E";;
        l3) mca_code="0x000F";;
    esac

    [[ "$TYPE" == "uce" ]] && status_uc="S"

    local tmpfile
    tmpfile=$(mktemp /tmp/mce-inject.XXXXXX)
    cat > "$tmpfile" <<EOF
CPU $CPU
BANK 0
STATUS ${status_uc}Val CE MiscV AddrV ${mca_code}
ADDR 0x0
MISC 0x0
EOF

    echo "Injecting via mce-inject:"
    cat "$tmpfile"
    for ((i=1; i<=COUNT; i++)); do
        mce-inject "$tmpfile"
        echo "  Injected $i/$COUNT"
    done
    rm -f "$tmpfile"
}

# Main
ARCH=$(detect_arch)
echo "=== CPU Fault Isolation - Error Injection ==="
echo "Architecture: $ARCH"
echo "Target CPU: $CPU, Type: $TYPE, Level: $LEVEL, Count: $COUNT"
echo ""

# Check current state
if [[ -f "/sys/devices/system/cpu/cpu${CPU}/cfi/state" ]]; then
    echo "Current CPU $CPU state: $(cat /sys/devices/system/cpu/cpu${CPU}/cfi/state)"
fi

echo ""

# Choose injection method
case "$ARCH" in
    x86)
        echo "Available methods: mce-inject, EINJ, debugfs"
        if [[ -e /dev/mcelog ]] || command -v mce-inject &>/dev/null; then
            inject_via_mce_inject
        else
            inject_via_debugfs
        fi
        ;;
    arm64)
        echo "Available methods: EINJ, debugfs"
        inject_via_debugfs
        ;;
    *)
        echo "Unknown architecture. Using debugfs injection."
        inject_via_debugfs
        ;;
esac

echo ""
echo "=== Post-injection state ==="
if [[ -f "/sys/devices/system/cpu/cpu${CPU}/cfi/state" ]]; then
    echo "CPU $CPU state:  $(cat /sys/devices/system/cpu/cpu${CPU}/cfi/state)"
    echo "CPU $CPU CE:     $(cat /sys/devices/system/cpu/cpu${CPU}/cfi/ce_count)"
    echo "CPU $CPU UCE:    $(cat /sys/devices/system/cpu/cpu${CPU}/cfi/uce_count)"
    echo "CPU $CPU types:  $(cat /sys/devices/system/cpu/cpu${CPU}/cfi/error_types)"
fi
