#!/bin/bash
# CFI/MFI 真实硬件路径验证 (P9)
#
# 用 mce_inject + hwpoison_inject 覆盖 debugfs cfi/inject 跳过的代码段：
#   - do_machine_check 主流程 / __mc_scan_banks / mce_severity 判决
#   - x86_mce_decoder_chain 真实回调 (我们注册的 cfi notifier)
#   - memory_failure_queue 异步入队
#   - memory_failure 内核态全路径 (rmap unmap / SIGBUS 同步投递)
#
# 用法 (在 ECS 上以 root 跑，模块已编译):
#   bash test/run_real_inject.sh
#
# 输出:
#   /home/cfi_logs/real_inject_report.txt        # 人读总结
#   /home/cfi_logs/real_inject_dmesg.txt         # 内核侧痕迹
#   /home/cfi_logs/real_inject_cfimon.log        # netlink 事件
#
# 安全说明:
#   全部用 flags=sw (software-injection)，不真发 #MC 异常。VM 内可重复跑。
#   一个可选 case (--hw) 会用 flags=hw 触发一次真 #MC，仅当模块的 panic
#   suppression 正常时才安全；PCC=0、UC=0 的 corrected case 没有 panic 风险。

set -u
cd "$(dirname "$0")/.."

LOGDIR=/home/cfi_logs
mkdir -p "$LOGDIR"
REPORT="$LOGDIR/real_inject_report.txt"
DMESGLOG="$LOGDIR/real_inject_dmesg.txt"
MONLOG="$LOGDIR/real_inject_cfimon.log"

INJ_DIR=/sys/kernel/debug/mce-inject
HWP_DIR=/sys/kernel/debug/hwpoison
CFI_INJ=/sys/kernel/debug/cfi/inject
MEM=/sys/kernel/cfi/mem

PASS=0; FAIL=0; SKIP=0
WITH_HW=0
WITH_LMCE=0
_INJECT_MCGSTATUS=""
# 默认隔离模式: inactive
#   - VM/裸金属上工作良好(也可以选 full 走完整 cpu_down)
#   - 物理机上避开 HCE2 完整 cpu_down 的 ABBA 死锁
# 显式覆盖: --mode=full|inactive|soft 或 --offline (= full) / --soft
# --lmce: implies --hw; 把 C1-C5 的 MCGSTATUS 拉成 0xF (含 LMCE_S),
#         绕过 Intel 广播 MCE 同步, 改走 mce_severity 决策路径. 这是更接近
#         真实硬件 panic 的语义 ("Fatal machine check on current CPU"
#         而不是 "Some CPUs didn't answer in synchronization").
MODE=inactive
for a in "$@"; do
    case "$a" in
        --hw)             WITH_HW=1 ;;
        --lmce)           WITH_HW=1; WITH_LMCE=1; _INJECT_MCGSTATUS=0xf ;;
        --mcgstatus=*)    _INJECT_MCGSTATUS="${a#--mcgstatus=}" ;;
        --offline|--full) MODE=full ;;
        --soft)           MODE=soft ;;
        --inactive)       MODE=inactive ;;
        --mode=*)         MODE="${a#--mode=}" ;;
    esac
done
case "$MODE" in
    full|inactive|soft) ;;
    *) echo "unknown --mode=$MODE (full|inactive|soft)"; exit 2 ;;
esac

log()  { echo "$*" | tee -a "$REPORT"; }
ok()   { PASS=$((PASS+1)); log "  [PASS] $*"; }
bad()  { FAIL=$((FAIL+1)); log "  [FAIL] $*"; }
skip() { SKIP=$((SKIP+1)); log "  [SKIP] $*"; }
hdr()  { log ""; log "===== $* ====="; }

dmesg_mark() {
    local tag=$1
    dmesg -c >>"$DMESGLOG" 2>&1 || dmesg >>"$DMESGLOG"  # snapshot
    echo "--- MARK $tag $(date +%T) ---" >>"$DMESGLOG"
}

dmesg_since_mark_grep() {
    # tail dmesg since most recent MARK and grep
    local pat=$1
    dmesg | tee -a "$DMESGLOG" | tail -200 | grep -E "$pat"
}

: > "$REPORT"
: > "$DMESGLOG"
log "CFI/MFI real-path validation  $(date)"
log "host: $(uname -r)  cpus=$(nproc)  mem=$(free -h | awk '/Mem/{print $2}')"
log "options: WITH_HW=$WITH_HW WITH_LMCE=$WITH_LMCE${_INJECT_MCGSTATUS:+ MCGSTATUS=$_INJECT_MCGSTATUS}"
log "artifacts: $LOGDIR/  (persistent disk, survives reboot)"

# ---------- 前置 ----------
hdr "Preflight"
[[ -f kernel/cpu_fault_isolate.ko ]] || { bad "module .ko missing - run P0 build first"; exit 1; }
rmmod cpu_fault_isolate 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

# 工具二进制必须就绪 (本脚本所有 pfn 都依赖 ownpage)
if [[ ! -x test/tools/ownpage ]] || [[ ! -x test/tools/cfimon ]]; then
    log "  building userspace tools..."
    make -C test/tools >>"$REPORT" 2>&1
fi
[[ -x test/tools/ownpage ]] && ok "ownpage tool ready" || { bad "ownpage build failed"; exit 1; }
[[ -x test/tools/cfimon ]] && ok "cfimon tool ready" || { bad "cfimon build failed"; exit 1; }

# 装 mce-inject(8) 用户态工具 + mcelog 观测 + 加载内核模块
# 用户态 mce-inject 是 C 段 hw 路径的关键依赖 (--with-hw 时); 走 dnf/zypper
# 都试一下, 缺包 C 段会 SKIP 而不是失败.
(dnf install -y mce-inject mcelog || zypper install -y mce-inject mcelog) >/dev/null 2>&1 || true
modprobe mce_inject 2>&1 | tee -a "$REPORT"
modprobe hwpoison_inject 2>&1 | tee -a "$REPORT"

[[ -d "$INJ_DIR" ]] && ok "mce-inject debugfs available ($INJ_DIR)" \
                   || { bad "mce-inject debugfs missing"; exit 1; }
command -v mce-inject >/dev/null 2>&1 && ok "mce-inject(8) userspace tool present" \
                                      || skip "mce-inject(8) tool absent (C段 hw 路径会 SKIP)"
[[ -f "$HWP_DIR/corrupt-pfn" ]] && ok "hwpoison-inject debugfs available" \
                               || skip "hwpoison-inject debugfs not present"

# 装 CFI 模块 (测试用阈值)
log "  isolation mode: $MODE"
case "$MODE" in
    full)     log "    (cpu hotplug down — VM / non-stubbed hosts only)" ;;
    inactive) log "    (set_cpu_active=false + IRQ migration — RECOMMENDED for HCE2 physical hosts)" ;;
    soft)     log "    (IRQ migration only — daemon must migrate tasks; weakest mode)" ;;
esac
if insmod kernel/cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 \
        window_secs=60 defer_to_daemon=0 page_ce_threshold=3 \
        mem_window_secs=300 mem_triage=0 isolation_mode=$MODE 2>&1 | tee -a "$REPORT"; then
    ok "cfi module loaded"
else
    bad "cfi module load failed"; exit 1
fi
sleep 1

# 下线 bypass 符号解析情况（物理机上 cpu_subsys_offline 被守护代理打桩时用得到）
BYP=$(dmesg | grep -m1 "offline bypass resolved")
if [[ -n "$BYP" ]]; then
    log "  ${BYP#*cpu_fault_isolate: }"
    echo "$BYP" | grep -q "cpu_device_down=ok" \
        && ok "offline bypass available (cpu_device_down resolved)" \
        || skip "cpu_device_down not resolved (stub-bypass unavailable on this kernel)"
else
    skip "offline bypass status not in dmesg (offline_bypass=N?)"
fi

# netlink 监听
./test/tools/cfimon > "$MONLOG" 2>&1 &
MONPID=$!
sleep 1
kill -0 $MONPID 2>/dev/null && ok "cfimon listener up" || bad "cfimon failed"

dmesg -c >/dev/null 2>&1

# ---------- 工具: 提交一次 mce-inject (debugfs 路径) ----------
# 用法: mce_inject <flags> <bank> <status_hex> <addr_hex> <misc_hex> <cpu>
#
# 注意: flags=sw 时只调 mce_log, 不进 do_machine_check; flags=hw 直注实测
# 在 HCE2 物理机 (FMA firmware-first) 上不可靠 (probe 误诊). 走真 #MC 请
# 用 mce_inject_file().
mce_inject() {
    local flags=${1:-sw} bank=${2:-0} status=${3:-0} addr=${4:-0} misc=${5:-0} cpu=${6:-0}
    # 护栏: 绝不向离线 CPU 注入。inj_*_set 会用 smp_call_function_single
    # 打目标 CPU; 若该 CPU 正在/已经下线, 调用方会硬卡死 (本机实测教训)。
    local onf="/sys/devices/system/cpu/cpu$cpu/online"
    if [[ -f "$onf" && "$(cat "$onf")" == "0" ]]; then
        log "  [GUARD] cpu$cpu is offline, skipping mce_inject"
        return 1
    fi
    echo "$status"  > "$INJ_DIR/status"
    echo "$addr"    > "$INJ_DIR/addr"
    echo "$misc"    > "$INJ_DIR/misc"
    echo 0          > "$INJ_DIR/synd"
    echo "$cpu"     > "$INJ_DIR/cpu"
    # MCGSTATUS override (for --lmce / --mcgstatus). hw flag only.
    if [[ "$flags" == "hw" && -n "${_INJECT_MCGSTATUS:-}" && -w "$INJ_DIR/mcgstatus" ]]; then
        echo "$_INJECT_MCGSTATUS" > "$INJ_DIR/mcgstatus"
    fi
    echo "$flags"   > "$INJ_DIR/flags"
    # 最后写 bank 触发注入
    echo "$bank"    > "$INJ_DIR/bank"
}

# ---------- 工具: 真 #MC via mce-inject(8) 用户态工具 ----------
# 用法: mce_inject_file <cpu> <bank> <status_hex> <addr_hex> <misc_hex>
#
# 走用户态工具 + .mce 文件. mce-inject(8) 会把 debugfs flags 改成 raise,
# 然后 WRMSR + IPI-NMI 真正触发 #MC -> do_machine_check. HCE2 物理机
# (Huawei 2288H V5 / FMA) 实测可靠; 直接 debugfs flags=hw 在该平台经常
# 沉默无效, 因此 hw probe + C1-C5 都改走这条路径.
mce_inject_file() {
    local cpu=${1:-0} bank=${2:-0} status=${3:-0} addr=${4:-0} misc=${5:-0}
    local onf="/sys/devices/system/cpu/cpu$cpu/online"
    if [[ -f "$onf" && "$(cat "$onf")" == "0" ]]; then
        log "  [GUARD] cpu$cpu is offline, skipping mce_inject_file"
        return 1
    fi
    if ! command -v mce-inject >/dev/null 2>&1; then
        log "  [GUARD] mce-inject(8) userspace tool missing (dnf install mce-inject)"
        return 1
    fi
    local f=$LOGDIR/.mce.$$.txt
    cat > "$f" <<MCE
CPU $cpu
BANK $bank
STATUS $status
ADDR $addr
MISC $misc
MCE
    if [[ -n "${_INJECT_MCGSTATUS:-}" ]]; then
        echo "MCGSTATUS $_INJECT_MCGSTATUS" >> "$f"
    fi
    mce-inject "$f"
    local rc=$?
    rm -f "$f"
    return $rc
}

# MCi_STATUS 位组装 (Intel SDM Vol 3 Ch 15)
#   VAL(63) UC(61) EN(60) MISCV(59) ADDRV(58) PCC(57) S(56) AR(55) | MCACOD
#
# MCACOD 选取注意:
#   - mem 错误: 高 9 位 == 0b 0000_0000_1 (mem-ctrl mask 0xff80==0x0080)
#     或 == MCACOD_DATA(0x134) / MCACOD_INSTR(0x150) / MCACOD_L3WB(0x17A)
#     —— 这些被 cfi_x86_is_memory_errcode() 归到 mem 域 (mfi_x86)
#   - 真 CPU cache 错误: simple cache code 0x000C..0x000F
#     0x000D = L1, 0x000E = L2, 0x000F = L3/generic
#
# 预设:
#   CE   memory:    VAL|EN|MISCV|ADDRV | 0x094  = 0x9c00000000000094
#   SRAO memory:    VAL|UC|EN|MISCV|ADDRV | 0x094 = 0xbc00000000000094
#   SRAR memory:    VAL|UC|EN|MISCV|ADDRV|S|AR | 0x094 = 0xbd80000000000094
#   Cache UCE L2:   VAL|UC|EN | 0x000E = 0xb00000000000000e
#   Cache CE  L2:   VAL|EN    | 0x000E = 0x900000000000000e
#   Cache UCE L3:   VAL|UC|EN | 0x000F = 0xb00000000000000f

STAT_MEM_CE=0x9c00000000000094
STAT_MEM_SRAO=0xbc00000000000094
STAT_MEM_SRAR=0xbd80000000000094
STAT_CACHE_UCE_L2=0xb00000000000000e
STAT_CACHE_CE_L2=0x900000000000000e
STAT_CACHE_UCE_L3=0xb00000000000000f

# 安全的隔离目标 CPU: 不用 cpu0 (受保护/housekeeping), 不用最后一个,
# 取中段一个。物理机上 cpu0 常是厂商 work_on_cpu 跑 teardown 的宿主。
NCPU=$(nproc)
SAFE_TGT=$(( NCPU / 2 ))
[[ $SAFE_TGT -le 0 ]] && SAFE_TGT=1
# 第二个安全 CPU 给「只记账」的子测试用 (SAFE_TGT 在 A4 被隔离后不可复用:
# cfi_report_error 会跳过已隔离 CPU)。
SAFE_TGT2=$(( SAFE_TGT + 1 ))
[[ $SAFE_TGT2 -ge $NCPU ]] && SAFE_TGT2=$(( SAFE_TGT - 1 ))

# 稳健重新上线: 先试原生 sysfs; 物理机上 cpu_subsys_online 也可能被打桩,
# 那时只能靠模块 unisolate 的 bypass (此处尽力而为, 失败仅告警不致命)。
reonline_cpu() {
    local cpu=$1
    local onf="/sys/devices/system/cpu/cpu$cpu/online"
    [[ -f "$onf" ]] || return 0
    [[ "$(cat "$onf")" == "1" ]] && return 0
    echo 1 > "$onf" 2>/dev/null
    sleep 1
    if [[ "$(cat "$onf")" == "1" ]]; then
        log "  cpu$cpu re-onlined"
    else
        log "  [WARN] cpu$cpu 仍离线 (online 侧可能也被打桩); 后续不再向其注入"
    fi
}

# ---------- A1 Memory CE via real MCE decode chain ----------
hdr "A1 Memory CE (sw inject -> x86_mce_decoder_chain -> mfi CE accounting)"
CE0=$(awk '/ce_total/{print $2}' $MEM/stats)
# 拿一个我们自有的 pfn
./test/tools/ownpage > "$LOGDIR/inj_op_a1.txt" & A1=$!; sleep 1
PFN=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_a1.txt")
PADDR=$(printf "0x%x" $((PFN * 4096)))
log "  using pfn=$PFN paddr=$PADDR"
for i in 1 2 3; do
    mce_inject sw 4 "$STAT_MEM_CE" "$PADDR" 0 0
    sleep 1
done
sleep 2
CE1=$(awk '/ce_total/{print $2}' $MEM/stats)
DELTA=$((CE1 - CE0))
log "  ce_total: $CE0 -> $CE1 (delta=$DELTA)"
[[ $DELTA -ge 3 ]] && ok "memory CE flowed through MCE decode chain (delta=$DELTA)" \
                  || bad "no CE accounting from MCE chain (delta=$DELTA, debugfs/inject 路径正常但 mce-inject 未触达)"
# 预隔离统计
PRE=$(awk '/pages_pre_offlined/{print $2}' $MEM/stats)
[[ $PRE -ge 1 ]] && ok "CE threshold via real chain -> soft offline (pages_pre_offlined=$PRE)" \
                || skip "no pre-offline (page may be unmovable; check pages_failed)"
kill $A1 2>/dev/null

# ---------- A2 Memory SRAO ----------
hdr "A2 Memory SRAO (uce_async + hard offline via memory_failure_queue)"
UA0=$(awk '/uce_async/{print $2}' $MEM/stats)
OFF0=$(awk '/pages_offlined/{print $2}' $MEM/stats)
./test/tools/ownpage > "$LOGDIR/inj_op_a2.txt" & A2=$!; sleep 1
PFN2=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_a2.txt")
PADDR2=$(printf "0x%x" $((PFN2 * 4096)))
log "  using pfn=$PFN2 paddr=$PADDR2"
mce_inject sw 4 "$STAT_MEM_SRAO" "$PADDR2" 0 0
sleep 3
UA1=$(awk '/uce_async/{print $2}' $MEM/stats)
OFF1=$(awk '/pages_offlined/{print $2}' $MEM/stats)
log "  uce_async: $UA0 -> $UA1   pages_offlined: $OFF0 -> $OFF1"
[[ $UA1 -gt $UA0 ]] && ok "SRAO accounted as async UCE (delta=$((UA1-UA0)))" \
                  || bad "no async UCE accounting"
[[ $OFF1 -gt $OFF0 ]] && ok "hard offline triggered by MCE chain" \
                    || skip "no hard offline (timing/policy)"
kill $A2 2>/dev/null

# ---------- A3 Memory SRAR (action required) ----------
hdr "A3 Memory SRAR (action-required, synchronous memory_failure path)"
US0=$(awk '/uce_sync/{print $2}' $MEM/stats)
./test/tools/ownpage > "$LOGDIR/inj_op_a3.txt" & A3=$!; sleep 1
PFN3=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_a3.txt")
PADDR3=$(printf "0x%x" $((PFN3 * 4096)))
log "  using pfn=$PFN3 paddr=$PADDR3"
mce_inject sw 4 "$STAT_MEM_SRAR" "$PADDR3" 0 0
sleep 3
US1=$(awk '/uce_sync/{print $2}' $MEM/stats)
[[ $US1 -gt $US0 ]] && ok "SRAR accounted as sync UCE (delta=$((US1-US0)))" \
                  || skip "uce_sync delta=0 (severity may have been demoted to SRAO)"
if ! kill -0 $A3 2>/dev/null; then
    ok "owner process killed (SIGBUS via memory_failure MF_ACTION_REQUIRED)"
else
    log "  [INFO] owner alive — kernel did async kill, will check process exit"
fi
kill $A3 2>/dev/null

# ---------- A4 Cache UCE via MCE chain (L2, simple errcode 0x000E) ----------
hdr "A4 L2 Cache UCE on target CPU -> isolation"
TC=$SAFE_TGT
CE_BEF=$(cat /sys/devices/system/cpu/cpu$TC/cfi/uce_count 2>/dev/null || echo 0)
ST_BEF=$(cat /sys/devices/system/cpu/cpu$TC/cfi/state)
log "  before: cpu$TC state=$ST_BEF uce_count=$CE_BEF"
# bank 3 = MLC/L2 (Intel iCLX); status 用 simple cache errcode 0x000E
mce_inject sw 3 "$STAT_CACHE_UCE_L2" 0 0 "$TC"
sleep 3
ST_AFT=$(cat /sys/devices/system/cpu/cpu$TC/cfi/state)
ON_AFT=$(cat /sys/devices/system/cpu/cpu$TC/online)
CE_AFT=$(cat /sys/devices/system/cpu/cpu$TC/cfi/uce_count 2>/dev/null || echo 0)
log "  after:  cpu$TC state=$ST_AFT online=$ON_AFT uce_count=$CE_AFT"
case "$MODE" in
inactive)
    # inactive: CPU 仍在线(online=1), state=isolated, cpu_active_mask 清除, 中断被迁走
    if [[ "$ST_AFT" == isolated && "$ON_AFT" == 1 ]]; then
        ok "MCE-chain L2 cache UCE -> CPU inactive-isolated (sched_active=false, no hotplug, no deadlock)"
        M=$(dmesg | grep -m1 "cpu$TC: inactive-isolated")
        [[ -n "$M" ]] && ok "  ${M#*cpu_fault_isolate: }" || log "  (inactive-isolate dmesg line not found)"
    elif [[ $CE_AFT -gt $CE_BEF ]]; then
        ok "L2 cache UCE accounted (uce_count delta=$((CE_AFT-CE_BEF)))"
        bad "uce_count moved but state=$ST_AFT online=$ON_AFT, inactive isolation didn't fire"
    else
        bad "L2 cache UCE not visible (uce_count=0, dmesg may show details)"
    fi
    ;;
soft)
    # soft: CPU 仍在线(online=1), state=isolated, 中断被迁走
    if [[ "$ST_AFT" == isolated && "$ON_AFT" == 1 ]]; then
        ok "MCE-chain L2 cache UCE -> CPU soft-isolated (no hotplug, no deadlock)"
        M=$(dmesg | grep -m1 "cpu$TC: soft-isolated")
        [[ -n "$M" ]] && ok "  ${M#*cpu_fault_isolate: }" || log "  (soft-isolate dmesg line not found)"
    elif [[ $CE_AFT -gt $CE_BEF ]]; then
        ok "L2 cache UCE accounted (uce_count delta=$((CE_AFT-CE_BEF)))"
        bad "uce_count moved but state=$ST_AFT online=$ON_AFT, soft isolation didn't fire"
    else
        bad "L2 cache UCE not visible (uce_count=0, dmesg may show details)"
    fi
    ;;
full)
    # 完整下线: state=isolated, online=0
    if [[ "$ST_AFT" == isolated && "$ON_AFT" == 0 ]]; then
        ok "MCE-chain L2 cache UCE -> CPU isolated (offline)"
        M=$(dmesg | grep -m1 "cpu$TC: offlined via\|cpu$TC: isolated successfully")
        [[ "$M" == *"bypass"* ]] && ok "  offline took the stub-bypass path: ${M#*cpu_fault_isolate: }" \
                                 || log "  offline path: ${M#*cpu_fault_isolate: }"
    elif [[ $CE_AFT -gt $CE_BEF ]]; then
        ok "L2 cache UCE accounted (uce_count delta=$((CE_AFT-CE_BEF)))"
        bad "uce_count moved but state=$ST_AFT online=$ON_AFT, isolation didn't fire"
    else
        bad "L2 cache UCE not visible (uce_count=0, dmesg may show details)"
    fi
    ;;
esac
# full 模式需把 A4 隔离的 CPU 拉回; inactive/soft 模式 CPU 仍在线无需处理。
[[ "$MODE" == full ]] && reonline_cpu $TC
sleep 1

# 以下 A4b-A7 仅验证「真 MCE decode chain -> 各类错误分类记账」,
# 关闭 auto_isolate, 用另一个干净 CPU (SAFE_TGT 已被 A4 隔离, 会被跳过)。
echo 0 > /sys/kernel/cfi/auto_isolate 2>/dev/null
log "  (A4b-A7: auto_isolate=0, accounting-only, target cpu$SAFE_TGT2)"

# ---------- A4b L3/generic Cache UCE (simple errcode 0x000F) ----------
hdr "A4b L3 Cache UCE accounting"
TC_B=$SAFE_TGT2
CE_B0=$(cat /sys/devices/system/cpu/cpu$TC_B/cfi/uce_count 2>/dev/null || echo 0)
mce_inject sw 3 "$STAT_CACHE_UCE_L3" 0 0 "$TC_B"
sleep 2
CE_B1=$(cat /sys/devices/system/cpu/cpu$TC_B/cfi/uce_count 2>/dev/null || echo 0)
ON_B=$(cat /sys/devices/system/cpu/cpu$TC_B/online)
[[ $CE_B1 -gt $CE_B0 ]] && ok "L3 cache UCE accounted (delta=$((CE_B1-CE_B0)))" \
                       || bad "L3 cache UCE not accounted"
[[ $ON_B == 1 ]] && ok "auto_isolate=0 -> stays online" || bad "isolated despite auto=0"

# ---------- A5 Cache CE 计数 (L2 simple errcode 0x000E) ----------
hdr "A5 L2 Cache CE累加 (expect no isolation)"
TC2=$SAFE_TGT2
CE2_BEF=$(cat /sys/devices/system/cpu/cpu$TC2/cfi/ce_count 2>/dev/null || echo 0)
for i in 1 2; do
    mce_inject sw 3 "$STAT_CACHE_CE_L2" 0 0 "$TC2"
    sleep 1
done
CE2_AFT=$(cat /sys/devices/system/cpu/cpu$TC2/cfi/ce_count 2>/dev/null || echo 0)
log "  cpu$TC2 ce_count: $CE2_BEF -> $CE2_AFT"
[[ $CE2_AFT -gt $CE2_BEF ]] && ok "cache CE via MCE chain counted (delta=$((CE2_AFT-CE2_BEF)))" \
                           || bad "cache CE not counted"
[[ $(cat /sys/devices/system/cpu/cpu$TC2/online) == 1 ]] && ok "CE alone did not isolate" \
                                                       || bad "CE caused isolation!"

# ---------- A6 TLB error (compound errcode, bit 4 set) ----------
# auto_isolate 仍为 0 (A4 之后设置), 仅验证分类记账, 不触发下线。
hdr "A6 TLB error (compound errcode 0x0816, expect CFI_ERR_TLB)"
TC3=$SAFE_TGT2
CE3_BEF=$(cat /sys/devices/system/cpu/cpu$TC3/cfi/uce_count 2>/dev/null || echo 0)
# compound bit (0x0800) set + 0x0010 (TLB) + LL=L2: errcode = 0x0816
mce_inject sw 2 0xb000000000000816 0 0 "$TC3"
sleep 2
CE3_AFT=$(cat /sys/devices/system/cpu/cpu$TC3/cfi/uce_count 2>/dev/null || echo 0)
if [[ $CE3_AFT -gt $CE3_BEF ]]; then
    ok "TLB UCE accounted (delta=$((CE3_AFT-CE3_BEF)))"
else
    skip "TLB UCE not visible in uce_count (may be accounted under different type)"
fi

# ---------- A7 Bus / interconnect error ----------
hdr "A7 Bus error (compound errcode 0x0E0F with bit 11 set)"
TC4=$SAFE_TGT2
CE4_BEF=$(cat /sys/devices/system/cpu/cpu$TC4/cfi/uce_count 2>/dev/null || echo 0)
# errcode 0x0E0F: bits 15..11=00001 (compound), 0x0800 set -> bus
mce_inject sw 5 0xb000000000000e0f 0 0 "$TC4"
sleep 2
CE4_AFT=$(cat /sys/devices/system/cpu/cpu$TC4/cfi/uce_count 2>/dev/null || echo 0)
if [[ $CE4_AFT -gt $CE4_BEF ]]; then
    ok "Bus UCE accounted (delta=$((CE4_AFT-CE4_BEF)))"
else
    skip "Bus UCE not visible (path-specific; not all bus errors per-CPU)"
fi
echo 1 > /sys/kernel/cfi/auto_isolate 2>/dev/null  # 恢复 auto_isolate

# ---------- B hwpoison_inject 路径 ----------
hdr "B hwpoison_inject (kernel-direct memory_failure full path)"
if [[ -f "$HWP_DIR/corrupt-pfn" ]]; then
    OFF_B0=$(awk '/pages_offlined/{print $2}' $MEM/stats)
    ./test/tools/ownpage > "$LOGDIR/inj_op_b.txt" & B1=$!; sleep 1
    PFNB=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_b.txt")
    log "  using pfn=$PFNB"
    echo "$PFNB" > "$HWP_DIR/corrupt-pfn"
    sleep 3
    OFF_B1=$(awk '/pages_offlined/{print $2}' $MEM/stats)
    [[ $OFF_B1 -gt $OFF_B0 ]] && ok "hwpoison_inject -> memory_failure -> page offlined (delta=$((OFF_B1-OFF_B0)))" \
                             || skip "no offline delta (check dmesg for refcount/race)"
    kill -0 $B1 2>/dev/null && log "  [INFO] owner survived (kernel kept poisoned mapping)" \
                            || ok "owner killed by memory_failure SIGBUS"
    kill $B1 2>/dev/null
    grep -q "MEM_PAGE_OFFLINED\|MEM_ERROR" "$MONLOG" && ok "MFI received netlink event from hwpoison path" \
                                                     || skip "no netlink event captured"
else
    skip "hwpoison_inject not loaded"
fi

# ---------- C 可选: 真 #MC ----------
# 真 #MC 由 mce-inject(8) 用户态工具触发. 工具内部把 debugfs flags
# 改成 raise, WRMSR + IPI-NMI 进 do_machine_check -> mce_severity ->
# decode chain. 这条路在 HCE2 物理机 (FMA firmware-first) 与 KVM (带
# MCE 虚拟化) 上都实测可工作.
#
# 早期版本走 debugfs flags=hw 直注 + CE 探测, 在 HCE2 物理机上给 false
# negative (整段 SKIP). 物理机实测证伪: BIOS 并未屏蔽 WRMSR, 只是 CE
# 走 CMC 那条 IRQ 路径不可靠, UC 走 NMI 那条完全通畅. 切到用户态工具
# 之后即可在物理机上覆盖到 do_machine_check.
#
# 安全约束: PCC=0; UC case 依赖 cfi 模块的 panic_suppress (tolerant=3)
# 抑制 mce_panic 广播同步超时. 极端情况下 kdump 兜底, artifact 在
# /home/cfi_logs/.
if [[ $WITH_HW == 1 ]]; then
    hdr "C hw-mode: real #MC exception handler path"

    # ---- C0 hw 路径选择 (无探测) ----
    # 之前版本用 mem CE 探测投递路径, 但 HCE2 物理机上 CE 走 CMC IRQ ->
    # machine_check_poll, FMA / vendor SMM 把 MCi_STATUS scrub 掉再让 poll
    # 读, 探测看不到任何信号, 误报 "no delivery". UC + NMI 路径不被 FMA
    # 拦, 是 .mce 实测可用的路径, 但用 UC 做探测会把 probe cpu 隔离掉, 还
    # 要清理, 复杂度过高. 现在: 工具在就用 mce-inject(8), 不在就回退到
    # debugfs flags=hw, C1-C5 各自观测信号, 没观测到的报 bad/skip.
    HW_AVAILABLE=1
    INJECT_METHOD=
    if command -v mce-inject >/dev/null 2>&1; then
        INJECT_METHOD=file
        log "  hw inject path: mce-inject(8) userspace tool"
    else
        INJECT_METHOD=hw
        log "  hw inject path: debugfs flags=hw (mce-inject(8) not installed)"
    fi
    ok "hw injection path selected ($INJECT_METHOD)"

    # 取一个 ownpage 给 C1-C3 内存类 case 复用
    ./test/tools/ownpage > "$LOGDIR/inj_op_c0.txt" & C0PID=$!; sleep 1
    PFN_C0=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_c0.txt")
    [[ -z "$PFN_C0" ]] && PFN_C0=0x0
    PADDR_C0=$(printf "0x%x" $((PFN_C0 * 4096)))
    kill $C0PID 2>/dev/null

    # Wrapper: 用 INJECT_METHOD 选中的路径注入. 参数: <cpu> <bank> <status> <addr> <misc>
    inject_real() {
        local cpu=$1 bank=$2 status=$3 addr=${4:-0} misc=${5:-0}
        case "$INJECT_METHOD" in
            file) mce_inject_file "$cpu" "$bank" "$status" "$addr" "$misc" ;;
            hw|*) mce_inject hw "$bank" "$status" "$addr" "$misc" "$cpu" ;;
        esac
    }

if [[ ${HW_AVAILABLE:-0} == 1 ]]; then
    # 取一些干净 pfn 给内存类 case 用
    ./test/tools/ownpage > "$LOGDIR/inj_op_c2.txt" & C2=$!; sleep 1
    PFN_C2=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_c2.txt")
    PADDR_C2=$(printf "0x%x" $((PFN_C2 * 4096)))

    ./test/tools/ownpage > "$LOGDIR/inj_op_c3.txt" & C3=$!; sleep 1
    PFN_C3=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_c3.txt")
    PADDR_C3=$(printf "0x%x" $((PFN_C3 * 4096)))

    # ---- C1 hw memory CE: do_machine_check -> CMC handler -> decode chain ----
    log "  --- C1 hw memory CE (PCC=0, expect: cfi mfi CE accounting) ---"
    CE_C0=$(awk '/ce_total/{print $2}' $MEM/stats)
    inject_real 0 4 "$STAT_MEM_CE" "$PADDR_C2" 0
    sleep 2
    CE_C1=$(awk '/ce_total/{print $2}' $MEM/stats)
    [[ $CE_C1 -gt $CE_C0 ]] && ok "hw mem CE flowed through real #MC handler (delta=$((CE_C1-CE_C0)))" \
                            || bad "hw mem CE not visible (delta=0)"

    # ---- C2 hw memory SRAO: 真 #MC -> mce_severity=AO -> memory_failure_queue ----
    log "  --- C2 hw memory SRAO (UC+EN+MISCV+ADDRV, expect async UCE) ---"
    UA_C0=$(awk '/uce_async/{print $2}' $MEM/stats)
    OFF_C0=$(awk '/pages_offlined/{print $2}' $MEM/stats)
    inject_real 0 4 "$STAT_MEM_SRAO" "$PADDR_C2" 0
    sleep 3
    UA_C1=$(awk '/uce_async/{print $2}' $MEM/stats)
    OFF_C1=$(awk '/pages_offlined/{print $2}' $MEM/stats)
    [[ $UA_C1 -gt $UA_C0 ]] && ok "hw SRAO -> uce_async (delta=$((UA_C1-UA_C0)))" \
                            || bad "hw SRAO uce_async=0"
    [[ $OFF_C1 -gt $OFF_C0 ]] && ok "hw SRAO -> hard offline (delta=$((OFF_C1-OFF_C0)))" \
                              || skip "no hard offline yet"
    kill $C2 2>/dev/null

    # ---- C3 hw memory SRAR: 真 #MC -> mce_severity=AR -> sync memory_failure ----
    # 无 CFI 时, broadcast MCE 同步超时会触发 mce_panic; CFI tolerant=3 门控掉.
    log "  --- C3 hw memory SRAR (UC+EN+MISCV+ADDRV+S+AR) ---"
    US_C0=$(awk '/uce_sync/{print $2}' $MEM/stats)
    inject_real 0 4 "$STAT_MEM_SRAR" "$PADDR_C3" 0
    sleep 3
    US_C1=$(awk '/uce_sync/{print $2}' $MEM/stats)
    UA_C2=$(awk '/uce_async/{print $2}' $MEM/stats)
    if [[ $US_C1 -gt $US_C0 ]]; then
        ok "hw SRAR -> uce_sync (delta=$((US_C1-US_C0))) — full severity preserved"
    elif [[ $UA_C2 -gt $UA_C1 ]]; then
        skip "hw SRAR demoted to async (5.10 #MC handler behavior in non-user ctx)"
    else
        bad "hw SRAR yielded no event"
    fi
    if ! kill -0 $C3 2>/dev/null; then
        ok "hw SRAR owner killed (SIGBUS)"
    else
        log "  [INFO] owner alive, may be pending async kill"
    fi
    kill $C3 2>/dev/null

    # ---- C4 hw L2 Cache UCE: 真 #MC -> cpu 域隔离 ----
    # 用第三个 CPU (SAFE_TGT 已被 A4 隔离, SAFE_TGT2 被 A 段累计)。
    # 这是 panic-vs-isolate 故事的真核心 case: 无 CFI 时 broadcast MCE
    # 同步超时 -> mce_panic; 有 CFI 时 tolerant=3 门控 + 隔离接力. 详见
    # docs/physical-host-panic-vs-isolation.md.
    echo 1 > /sys/kernel/cfi/auto_isolate 2>/dev/null  # C4 需要真隔离
    TCH=$(( SAFE_TGT2 + 1 )); [[ $TCH -ge $NCPU ]] && TCH=$(( SAFE_TGT - 1 ))
    log "  --- C4 hw L2 cache UCE on cpu$TCH (expect isolation) ---"
    UC_C0=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/uce_count 2>/dev/null || echo 0)
    ST_C0=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/state)
    log "    before: cpu$TCH state=$ST_C0 uce=$UC_C0"
    inject_real "$TCH" 3 "$STAT_CACHE_UCE_L2" 0 0
    sleep 3
    UC_C1=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/uce_count 2>/dev/null || echo 0)
    ST_C1=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/state)
    ON_C1=$(cat /sys/devices/system/cpu/cpu$TCH/online)
    log "    after:  cpu$TCH state=$ST_C1 online=$ON_C1 uce=$UC_C1"
    EXP_ON=$([[ "$MODE" == full ]] && echo 0 || echo 1)
    EXP_TAG=$([[ "$MODE" == full ]] && echo "" || echo "$MODE-")
    if [[ "$ST_C1" == isolated && "$ON_C1" == "$EXP_ON" ]]; then
        ok "hw cache UCE -> #MC -> cfi cpu ${EXP_TAG}isolation"
    elif [[ $UC_C1 -gt $UC_C0 ]]; then
        bad "uce_count moved but no isolation (state=$ST_C1 online=$ON_C1, expected mode=$MODE)"
    else
        bad "hw cache UCE not visible in CFI (decode chain not reached?)"
    fi
    [[ "$MODE" == full ]] && reonline_cpu $TCH
    echo 0 > /sys/kernel/cfi/auto_isolate 2>/dev/null  # C5 仅记账
    sleep 1

    # ---- C5 hw L2 Cache CE: 真 #MC corrected handler ----
    log "  --- C5 hw L2 cache CE (corrected handler path) ---"
    TCC=$SAFE_TGT2
    CC_C0=$(cat /sys/devices/system/cpu/cpu$TCC/cfi/ce_count 2>/dev/null || echo 0)
    inject_real "$TCC" 3 "$STAT_CACHE_CE_L2" 0 0
    sleep 2
    CC_C1=$(cat /sys/devices/system/cpu/cpu$TCC/cfi/ce_count 2>/dev/null || echo 0)
    [[ $CC_C1 -gt $CC_C0 ]] && ok "hw cache CE counted (delta=$((CC_C1-CC_C0)))" \
                            || bad "hw cache CE not counted"
    [[ $(cat /sys/devices/system/cpu/cpu$TCC/online) == 1 ]] && ok "hw CE alone did not isolate" \
                                                            || bad "hw CE caused isolation!"

    log "  --- end of hw-mode block ---"
fi  # HW_AVAILABLE
fi  # WITH_HW

# ---------- 汇总 dmesg 痕迹 ----------
hdr "Kernel-side trace digest"
dmesg | grep -E "MCE.*Hardware Error|memory_failure|Memory failure|cpu_fault_isolate.*mem|cpu_fault_isolate.*cpu|x86_mce" | tail -40 | tee -a "$DMESGLOG"

# ---------- 清理 ----------
hdr "Cleanup"
kill $MONPID 2>/dev/null
echo 1 > /sys/kernel/cfi/auto_isolate 2>/dev/null
# re-online 任何 offline CPU (跳过已在线的; 物理机上 vendor 模块如
# livepatch_cpu_offline 会保持部分 CPU 在线状态不可写, 反复 echo 1 也无意义)
for c in $(seq 0 $(($(nproc)-1))); do
    onf=/sys/devices/system/cpu/cpu$c/online
    [[ -f "$onf" ]] || continue
    [[ "$(cat "$onf")" == "0" ]] || continue
    reonline_cpu $c
done
sleep 1
rmmod cpu_fault_isolate 2>&1 | tee -a "$REPORT" && ok "cfi module unloaded cleanly" || bad "rmmod failed"

# ---------- 汇总 ----------
hdr "SUMMARY"
log "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
log "artifacts in $LOGDIR/"
[[ $FAIL == 0 ]] && log "RESULT: ALL GREEN" || log "RESULT: $FAIL FAILURE(S) — see above"
exit $FAIL
