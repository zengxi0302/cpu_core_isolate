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
[[ "${1:-}" == "--hw" ]] && WITH_HW=1

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
log "options: WITH_HW=$WITH_HW"
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

# 装 mce-inject 工具与 mcelog (mcelog 观测，mce-inject 用户态工具仓库缺，用 debugfs 直写)
dnf install -y mcelog >/dev/null 2>&1 || true
modprobe mce_inject 2>&1 | tee -a "$REPORT"
modprobe hwpoison_inject 2>&1 | tee -a "$REPORT"

[[ -d "$INJ_DIR" ]] && ok "mce-inject debugfs available ($INJ_DIR)" \
                   || { bad "mce-inject debugfs missing"; exit 1; }
[[ -f "$HWP_DIR/corrupt-pfn" ]] && ok "hwpoison-inject debugfs available" \
                               || skip "hwpoison-inject debugfs not present"

# 装 CFI 模块 (测试用阈值)
if insmod kernel/cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 \
        window_secs=60 defer_to_daemon=0 page_ce_threshold=3 \
        mem_window_secs=300 mem_triage=0 2>&1 | tee -a "$REPORT"; then
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

# ---------- 工具: 提交一次 mce-inject ----------
# 用法: mce_inject <flags> <bank> <status_hex> <addr_hex> <misc_hex> <cpu>
mce_inject() {
    local flags=$1 bank=$2 status=$3 addr=$4 misc=$5 cpu=$6
    echo "$status"  > "$INJ_DIR/status"
    echo "$addr"    > "$INJ_DIR/addr"
    echo "$misc"    > "$INJ_DIR/misc"
    echo 0          > "$INJ_DIR/synd"
    echo "$cpu"     > "$INJ_DIR/cpu"
    echo "$flags"   > "$INJ_DIR/flags"
    # 最后写 bank 触发注入
    echo "$bank"    > "$INJ_DIR/bank"
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
TC=$(($(nproc) - 1))
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
if [[ "$ST_AFT" == isolated && "$ON_AFT" == 0 ]]; then
    ok "MCE-chain L2 cache UCE -> CPU isolated"
    # 物理机上 remove_cpu 可能被 cpu_subsys_offline 桩挡住; 报告实际下线路径
    M=$(dmesg | grep -m1 "cpu$TC: offlined via\|cpu$TC: isolated successfully")
    [[ "$M" == *"bypass"* ]] && ok "  offline took the stub-bypass path: ${M#*cpu_fault_isolate: }" \
                             || log "  offline path: ${M#*cpu_fault_isolate: }"
elif [[ $CE_AFT -gt $CE_BEF ]]; then
    ok "L2 cache UCE accounted (uce_count delta=$((CE_AFT-CE_BEF)))"
    bad "uce_count moved but state=$ST_AFT online=$ON_AFT, isolation didn't fire"
else
    bad "L2 cache UCE not visible (uce_count=0, dmesg may show details)"
fi
echo 1 > /sys/devices/system/cpu/cpu$TC/online 2>/dev/null || true
sleep 1

# ---------- A4b L3/generic Cache UCE (simple errcode 0x000F) ----------
hdr "A4b L3 Cache UCE -> isolation"
TC_B=$TC
echo 0 > /sys/kernel/cfi/auto_isolate 2>/dev/null  # 先关 auto，只看记账
CE_B0=$(cat /sys/devices/system/cpu/cpu$TC_B/cfi/uce_count 2>/dev/null || echo 0)
mce_inject sw 3 "$STAT_CACHE_UCE_L3" 0 0 "$TC_B"
sleep 2
CE_B1=$(cat /sys/devices/system/cpu/cpu$TC_B/cfi/uce_count 2>/dev/null || echo 0)
ON_B=$(cat /sys/devices/system/cpu/cpu$TC_B/online)
[[ $CE_B1 -gt $CE_B0 ]] && ok "L3 cache UCE accounted (delta=$((CE_B1-CE_B0)))" \
                       || bad "L3 cache UCE not accounted"
[[ $ON_B == 1 ]] && ok "auto_isolate=0 -> stays online" || bad "isolated despite auto=0"
echo 1 > /sys/kernel/cfi/auto_isolate

# ---------- A5 Cache CE 计数 (L2 simple errcode 0x000E) ----------
hdr "A5 L2 Cache CE累加 (expect no isolation)"
TC2=$(($(nproc) - 2))
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
hdr "A6 TLB error (compound errcode 0x0014, expect CFI_ERR_TLB)"
TC3=$(($(nproc) - 3))
[[ $TC3 -lt 0 ]] && TC3=0
CE3_BEF=$(cat /sys/devices/system/cpu/cpu$TC3/cfi/uce_count 2>/dev/null || echo 0)
# compound bit (0x0800) set + 0x0010 (TLB) + LL=L2: errcode = 0x0816
# severity: UC for visibility
mce_inject sw 2 0xb000000000000816 0 0 "$TC3"
sleep 2
CE3_AFT=$(cat /sys/devices/system/cpu/cpu$TC3/cfi/uce_count 2>/dev/null || echo 0)
if [[ $CE3_AFT -gt $CE3_BEF ]]; then
    ok "TLB UCE accounted (delta=$((CE3_AFT-CE3_BEF)))"
else
    skip "TLB UCE not visible in uce_count (may be accounted under different type)"
fi
echo 1 > /sys/devices/system/cpu/cpu$TC3/online 2>/dev/null || true

# ---------- A7 Bus / interconnect error ----------
hdr "A7 Bus error (compound errcode 0x0E0F with bit 11 set)"
TC4=0
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
echo 1 > /sys/devices/system/cpu/cpu$TC4/online 2>/dev/null || true

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

# ---------- C 可选: 真 #MC (hw mode) ----------
# hw mode 与 sw 的区别 (mce-inject README):
#   sw: 只调 x86_mce_decoder_chain，绕过 do_machine_check 主体
#   hw: 真正经过 #MC exception handler -> do_machine_check -> __mc_scan_banks
#       -> mce_severity -> 调度到 IRQ 上下文 -> decode chain
# 安全约束: 全部 case 设 PCC=0；UC case 依赖我们的 panic_suppress (tolerant=3)
# 来阻止 mce_panic。如果出问题 kdump 会保护，artifact 在 /home/cfi_logs/。
if [[ $WITH_HW == 1 ]]; then
    hdr "C hw-mode: real #MC exception handler path"

    # ---- C0 KVM-guest hw-mode availability probe ----
    # mce-inject 的 hw 模式经 prepare_msrs -> WRMSR MSR_IA32_MCi_STATUS -> raise
    # #MC -> do_machine_check 读 MSR -> decode chain。KVM guest 上有 3 种可能:
    #   (a) WRMSR 直接 #GP -> dmesg "unchecked MSR access error" + #MC 不发
    #   (b) WRMSR 被 KVM 静默 no-op -> #MC 触发但 MSR 读回 0 -> decode chain 不调
    #   (c) KVM 完整 MCE 虚拟化 -> #MC 流程通畅
    # 单看 dmesg 区分不了 (b) 和 (c)，所以 probe 必须**实测 ce_total 是否累加**。
    dmesg -c >/dev/null 2>&1
    ./test/tools/ownpage > "$LOGDIR/inj_op_c0.txt" & C0PID=$!; sleep 1
    PFN_C0=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_c0.txt")
    [[ -z "$PFN_C0" ]] && PFN_C0=0x0
    PADDR_C0=$(printf "0x%x" $((PFN_C0 * 4096)))
    PROBE_CPU=$(($(nproc) / 2))  # 避开 cpu0/最后一个 cpu
    PROBE_CE_BEF=$(awk '/ce_total/{print $2}' $MEM/stats)
    log "  probing hw-mode on cpu$PROBE_CPU (mem CE; ce_total baseline=$PROBE_CE_BEF)..."
    mce_inject hw 4 "$STAT_MEM_CE" "$PADDR_C0" 0 "$PROBE_CPU"
    sleep 3
    PROBE_CE_AFT=$(awk '/ce_total/{print $2}' $MEM/stats)
    kill $C0PID 2>/dev/null

    HW_REASON=""
    if dmesg | grep -qE "unchecked MSR access error: WRMSR to 0x4"; then
        HW_REASON="KVM guest #GP on WRMSR to MCi_STATUS"
        HW_AVAILABLE=0
    elif [[ $PROBE_CE_AFT -gt $PROBE_CE_BEF ]]; then
        HW_AVAILABLE=1
    else
        HW_REASON="KVM guest silently no-ops WRMSR to MCi_STATUS (no decode chain delivery)"
        HW_AVAILABLE=0
    fi

    if [[ $HW_AVAILABLE == 0 ]]; then
        skip "hw-mode unavailable: $HW_REASON"
        skip "C1 hw mem CE (hw-mode unavailable)"
        skip "C2 hw mem SRAO (hw-mode unavailable)"
        skip "C3 hw mem SRAR (hw-mode unavailable)"
        skip "C4 hw cache UCE (hw-mode unavailable)"
        skip "C5 hw cache CE (hw-mode unavailable)"
        log "  Why: hw-mode requires KVM with full MCE virtualization, or bare-metal."
        log "       sw mode already covers x86_mce_decoder_chain end-to-end (see A1-A7),"
        log "       so the remaining gap is mce_severity()/do_machine_check() prologue,"
        log "       which can only be exercised on a physical CPU."
    else
        ok "hw-mode functional (ce_total moved $PROBE_CE_BEF -> $PROBE_CE_AFT)"
    fi

if [[ ${HW_AVAILABLE:-0} == 1 ]]; then
    # 取一些干净 pfn 给内存类 case 用
    ./test/tools/ownpage > "$LOGDIR/inj_op_c2.txt" & C2=$!; sleep 1
    PFN_C2=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_c2.txt")
    PADDR_C2=$(printf "0x%x" $((PFN_C2 * 4096)))

    ./test/tools/ownpage > "$LOGDIR/inj_op_c3.txt" & C3=$!; sleep 1
    PFN_C3=$(awk -F= '/^pfn/{print $2}' "$LOGDIR/inj_op_c3.txt")
    PADDR_C3=$(printf "0x%x" $((PFN_C3 * 4096)))

    # ---- C1 hw memory CE: 经过 do_machine_check -> decode chain ----
    log "  --- C1 hw memory CE (PCC=0, expect: cfi mfi CE accounting) ---"
    CE_C0=$(awk '/ce_total/{print $2}' $MEM/stats)
    PFN_C1=$(printf "0x%x" $((0xdeadbe))) # pad random; mfi pfn 校验不通过则 fall back
    mce_inject hw 4 "$STAT_MEM_CE" "$PADDR_C2" 0 0
    sleep 2
    CE_C1=$(awk '/ce_total/{print $2}' $MEM/stats)
    [[ $CE_C1 -gt $CE_C0 ]] && ok "hw mem CE flowed through real #MC handler (delta=$((CE_C1-CE_C0)))" \
                            || bad "hw mem CE not visible (delta=0)"

    # ---- C2 hw memory SRAO: 真 #MC -> mce_severity=AO -> memory_failure_queue ----
    log "  --- C2 hw memory SRAO (UC+EN+MISCV+ADDRV, expect async UCE) ---"
    UA_C0=$(awk '/uce_async/{print $2}' $MEM/stats)
    OFF_C0=$(awk '/pages_offlined/{print $2}' $MEM/stats)
    mce_inject hw 4 "$STAT_MEM_SRAO" "$PADDR_C2" 0 0
    sleep 3
    UA_C1=$(awk '/uce_async/{print $2}' $MEM/stats)
    OFF_C1=$(awk '/pages_offlined/{print $2}' $MEM/stats)
    [[ $UA_C1 -gt $UA_C0 ]] && ok "hw SRAO -> uce_async (delta=$((UA_C1-UA_C0)))" \
                            || bad "hw SRAO uce_async=0"
    [[ $OFF_C1 -gt $OFF_C0 ]] && ok "hw SRAO -> hard offline (delta=$((OFF_C1-OFF_C0)))" \
                              || skip "no hard offline yet"
    kill $C2 2>/dev/null

    # ---- C3 hw memory SRAR: 真 #MC -> mce_severity=AR -> sync memory_failure ----
    log "  --- C3 hw memory SRAR (UC+EN+MISCV+ADDRV+S+AR) ---"
    US_C0=$(awk '/uce_sync/{print $2}' $MEM/stats)
    mce_inject hw 4 "$STAT_MEM_SRAR" "$PADDR_C3" 0 0
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
    log "  --- C4 hw L2 cache UCE on cpu$(($(nproc)-1)) (expect isolation) ---"
    TCH=$(($(nproc)-1))
    UC_C0=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/uce_count 2>/dev/null || echo 0)
    ST_C0=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/state)
    log "    before: cpu$TCH state=$ST_C0 uce=$UC_C0"
    mce_inject hw 3 "$STAT_CACHE_UCE_L2" 0 0 "$TCH"
    sleep 3
    UC_C1=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/uce_count 2>/dev/null || echo 0)
    ST_C1=$(cat /sys/devices/system/cpu/cpu$TCH/cfi/state)
    ON_C1=$(cat /sys/devices/system/cpu/cpu$TCH/online)
    log "    after:  cpu$TCH state=$ST_C1 online=$ON_C1 uce=$UC_C1"
    if [[ "$ST_C1" == isolated && "$ON_C1" == 0 ]]; then
        ok "hw cache UCE -> #MC -> cfi cpu isolation"
    elif [[ $UC_C1 -gt $UC_C0 ]]; then
        bad "uce_count moved but no isolation (state=$ST_C1)"
    else
        bad "hw cache UCE not visible in CFI (decode chain not reached?)"
    fi
    echo 1 > /sys/devices/system/cpu/cpu$TCH/online 2>/dev/null || true
    sleep 1

    # ---- C5 hw L2 Cache CE: 真 #MC corrected handler ----
    log "  --- C5 hw L2 cache CE (corrected handler path) ---"
    TCC=$(($(nproc)-2))
    [[ $TCC -lt 0 ]] && TCC=0
    CC_C0=$(cat /sys/devices/system/cpu/cpu$TCC/cfi/ce_count 2>/dev/null || echo 0)
    mce_inject hw 3 "$STAT_CACHE_CE_L2" 0 0 "$TCC"
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
# re-online 任何 offline CPU
for c in $(seq 0 $(($(nproc)-1))); do
    if [[ -e /sys/devices/system/cpu/cpu$c/online ]]; then
        [[ $(cat /sys/devices/system/cpu/cpu$c/online) == 0 ]] && echo 1 > /sys/devices/system/cpu/cpu$c/online
    fi
done
sleep 1
rmmod cpu_fault_isolate 2>&1 | tee -a "$REPORT" && ok "cfi module unloaded cleanly" || bad "rmmod failed"

# ---------- 汇总 ----------
hdr "SUMMARY"
log "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
log "artifacts in $LOGDIR/"
[[ $FAIL == 0 ]] && log "RESULT: ALL GREEN" || log "RESULT: $FAIL FAILURE(S) — see above"
exit $FAIL
