# 故障注入逐 case 对照矩阵

每个 `NN_<fault>.sh` 是一个独立可重复的故障注入实验，对应 `test/run_real_inject.sh` 里的一个分段。每个脚本在 CFI 模块加载与未加载两种状态下都能跑，输出固定格式，方便手动对比"无 CFI 时发生了什么"与"有 CFI 时发生了什么"。

## 重要说明：sw 注入与 hw 注入的本质区别

`mce_inject` 的 `sw` flag 只把伪造的 MCi_STATUS 喂进 `x86_mce_decoder_chain`，**绕过** `do_machine_check` 与 `mce_severity`。无论 CFI 在不在场，**sw 注入都不会让内核 panic**，差别只是"有没有策略动作（隔离 / 软硬下线 / netlink）"。

`hw` flag 才真发 `#MC` 异常，走完 `do_machine_check → mce_severity → CMC/MCE handler`。这才是触发 panic 的路径——`mce_tolerant<3` 时遇到 AR/PCC=1 类严重事件，内核就 `mce_panic`。"无 CFI 整机宕机 vs 有 CFI 隔离"的对照实验**只在 hw 模式下成立**。

`hwpoison_inject` 走 `memory_failure` 内核态全路径，跟 CFI 在不在场无关都会下线页，差别只在 CFI 的记账与 netlink。

`hw` 模式在部分 HCE2 物理机内核 / KVM guest 上被屏蔽（WRMSR MCi_STATUS 被静默 no-op 或 #GP），脚本会自检并提前退出。

## 对照矩阵

| # | File | 故障类型 | 注入路径 | bank | MCi_STATUS | **无 CFI 行为** | **有 CFI 行为** |
|---|------|---------|---------|------|-----------|---------------|---------------|
| 01 | `01_mem_ce.sh` | 内存 CE (correctable) | sw chain | 4 | `0x9c00…0094` | EDAC 计数，页面继续使用 | mfi CE 累加，到阈值软下线该 pfn，daemon 收 netlink |
| 02 | `02_mem_srao.sh` | 内存 SRAO (UC,async) | sw chain | 4 | `0xbc00…0094` | 解码链运行，**无动作**（sw 不走 memory_failure） | 异步 UCE 累加，memory_failure 入队，页硬下线 |
| 03 | `03_mem_srar.sh` | 内存 SRAR (UC,action-required) | sw chain | 4 | `0xbd80…0094` | 解码链运行，**无动作**，**不 panic** | 同步 memory_failure（或被降级到 async），owner SIGBUS，页下线 |
| 04 | `04_cache_uce_l2.sh` | L2 cache UCE（目标 CPU） | sw chain | 3 | `0xb000…000e` | EDAC 记录，**CPU 不隔离** | 按 `isolation_mode=` 隔离（full/inactive/soft） |
| 05 | `05_cache_uce_l3.sh` | L3 cache UCE（仅记账） | sw chain | 3 | `0xb000…000f` | EDAC 记录 | uce_count++ 归类为 L3 cache，auto=0 不隔离 |
| 06 | `06_cache_ce_l2.sh` | L2 cache CE（累计） | sw chain | 3 | `0x9000…000e` | EDAC 记录 | ce_count++，**CE 单独绝不隔离** |
| 07 | `07_tlb_uce.sh` | TLB UCE（compound 0x0816） | sw chain | 2 | `0xb000…0816` | EDAC 记录 | uce_count++ 归类为 TLB |
| 08 | `08_bus_uce.sh` | Bus UCE（compound 0x0E0F） | sw chain | 5 | `0xb000…0e0f` | EDAC 记录 | uce_count++ 归类为 BUS（部分总线错误非 per-cpu，可能 SKIP） |
| 09 | `09_hwpoison_inject.sh` | 直发 memory_failure | hwpoison debugfs | – | – | 内核 memory_failure 下线页 | 同上 + mfi 记账 + netlink |
| 10 | `10_hw_mem_ce.sh` | 真 #MC 内存 CE | hw 异常 | 4 | `0x9c00…0094` | 真 #MC corrected handler 记录 | 同上 + cfi 通知器记账 |
| 11 | `11_hw_mem_srao.sh` | 真 #MC SRAO | hw 异常 | 4 | `0xbc00…0094` | mce_severity=AO，异步 memory_failure | 同上 + cfi 记账 + netlink |
| 12 | `12_hw_mem_srar.sh` | 真 #MC SRAR | hw 异常 | 4 | `0xbd80…0094` | **`tolerant<3` 时 mce_panic → 整机宕机 + kdump** | cfi 把 tolerant 升到 3 + panic_notifier 拦截 → 同步 memory_failure，owner SIGBUS，整机存活 |
| 13 | `13_hw_cache_uce.sh` | 真 #MC L2 cache UCE | hw 异常 | 3 | `0xb000…000e` | **mce_panic → 整机宕机 + kdump** | cfi panic_notifier 拦截 + 按 mode 隔离 cpu，整机存活 |
| 14 | `14_hw_cache_ce.sh` | 真 #MC L2 cache CE | hw 异常 | 3 | `0x9000…000e` | CMC handler 记录 | cfi cache CE 累加 |

**真正能演示"宕机 vs 存活"的只有 12 和 13。** 其它 11 个 case 都是"无策略动作 vs 有策略动作"——可以用来证明 CFI 工作正常，但不能证明 CFI 救了整机。

## 前置准备

```bash
# 0. 确保 mce_inject 与 hwpoison_inject 内核模块已加载
modprobe mce_inject
modprobe hwpoison_inject

# 1. 编译用户态工具（ownpage / cfimon）
make -C test/tools

# 2.（可选）启动 cfimon 抓 netlink，供 09 之外的 case 验证
./test/tools/cfimon > /home/cfi_logs/inject_cases/cfimon.log &
```

跑 hw 模式 case（10–14）之前，**必须**确认 kdump 已配置：

```bash
# panic 后多少秒自动 reboot（0 = 永远不重启，演示场景不建议）
cat /proc/sys/kernel/panic
# kdump 服务
systemctl status kdump
```

## 操作流程

每个 case **跑两遍**，一次有 CFI 一次没 CFI，对比输出：

```bash
# ---- 第一遍：不加载 CFI ----
rmmod cpu_fault_isolate 2>/dev/null
bash test/inject_cases/01_mem_ce.sh > /tmp/01_no_cfi.txt 2>&1

# ---- 第二遍：加载 CFI ----
insmod kernel/cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 \
    window_secs=60 defer_to_daemon=0 page_ce_threshold=3 \
    mem_window_secs=300 mem_triage=0 isolation_mode=inactive
bash test/inject_cases/01_mem_ce.sh > /tmp/01_with_cfi.txt 2>&1

# 对比
diff -u /tmp/01_no_cfi.txt /tmp/01_with_cfi.txt
```

每个脚本输出都包含同一段 `observations:` 区块，无 CFI 时 mfi 计数器显示 `(n/a — CFI not loaded)`，有 CFI 时显示真实增量。

## hw 模式 case（10–14）跑之前

先用 case 10 探一下 hw 投递是否可用。它会自检 WRMSR 是否被屏蔽，不可用就直接退出，不会发起真注入。如果 case 10 直接 abort，case 11–14 都跑不了 hw 路径——这是 vendor kernel / KVM 的限制，不是 CFI 的问题。可考虑：
- 真正裸金属 + 未屏蔽的 BIOS/MCE 配置；
- ACPI EINJ（参考 `test/inject/inject_common.sh` 的 EINJ 分支）；
- 真坏 DIMM。

## 安全栏

- `mce_submit` 拒绝向已离线的 cpu 注入（`smp_call_function_single` 会卡死）；
- 04/13 case 隔离 CPU 后会自动尝试 `echo 1 > .../online` 把它拉回；
- 12/13 case 在 CFI 未加载时会给 5 秒 Ctrl-C 窗口，确认 kdump 状态后再继续。

## 这套对外能讲什么

- **CFI 故障覆盖面**：内存 CE/SRAO/SRAR、cache CE/UCE（L2/L3）、TLB UCE、bus UCE、外加内核态 memory_failure 全路径 + 真 #MC 全路径 = 一套完整的 x86 MCE → 隔离策略验证矩阵。
- **加载与不加载对比**：每一类故障在无 CFI 时的内核默认行为是什么，有 CFI 后多出哪些动作（隔离、记账、netlink、panic 抑制），都有可重复的对照输出。
- **整机宕机 vs 存活**：12/13 两个 case 能在物理机上演示，前提是 hw 注入路径未被 vendor 屏蔽。
