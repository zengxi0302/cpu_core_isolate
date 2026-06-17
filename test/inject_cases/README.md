# 故障注入逐 case 对照矩阵

每个 `NN_<fault>.sh` 是一个独立可重复的故障注入实验，对应 `test/run_real_inject.sh` 里的一个分段。每个脚本在 CFI 模块加载与未加载两种状态下都能跑，输出固定格式，方便手动对比"无 CFI 时发生了什么"与"有 CFI 时发生了什么"。

## 重要说明：三种注入路径

注入工具有两层 —— 内核 `mce_inject` 模块（debugfs `/sys/kernel/debug/mce-inject/`）与用户态 `mce-inject(8)` 工具，它们对路径的影响**不一样**：

| 路径 | 触发方式 | 走 do_machine_check？ | 真发 #MC？ | 用在哪些 case |
|------|---------|----------------------|-----------|--------------|
| `flags=sw` + debugfs 写 bank | `echo sw > flags; echo $bank > bank` | 否 | 否，只 `mce_log()` | 01–08 |
| `mce-inject(8)` + `.mce` 文件 | `mce-inject xxx.mce` —— 工具内部强制 `flags=raise`，WRMSR + IPI-NMI | **是** | **是** | 10–14 |
| `hwpoison_inject` | `echo $pfn > /sys/.../corrupt-pfn` —— 直接调内核 `memory_failure()` | 不相关（不走 MCE chain） | 否 | 09 |

**关键坑点**：用户态 `mce-inject` 工具会**强制覆写** `/sys/kernel/debug/mce-inject/flags` 为 `raise`，所以你在 cat flags 看到 `sw` 不代表注入瞬间走的是 sw 路径。Cases 10–14 改用了 `mce_inject_file` 助手（直接调用 `mce-inject` 工具），把这层显式化掉，不再依赖 debugfs `flags=hw` 的状态。

**对 panic 抑制 demo 的影响**：

- sw 注入（01–08）：无论 CFI 在不在，都**不会 panic**，差别只是"有没有策略动作"。
- hw 路径（10–14）+ UC bits：广播 MCE 默认开启，`mce-inject` 工具只在单 CPU 上 raise；其它 CPU 来不及响应 → `mce_panic` 触发"Some CPUs didn't answer in synchronization"。**这就是无 CFI 整机宕机的真实成因**。CFI 通过把 `mce_tolerant` 抬到 3，门控掉这条 panic 分支，让 do_machine_check 干净返回，再由 decode chain 进入 cfi notifier 隔离 cpu。

**已在 HCE2 + 2288H V5 物理机实证**：cases 12 与 13 的 .mce 注入，无 CFI 时触发 `mce_panic + kdump`，加载 CFI（inactive 模式）后同样注入只导致目标 cpu 被 inactive-isolated，整机存活。详见 `docs/physical-host-panic-vs-isolation.md`。

## 模式适用速查表

| # | Case | 默认 sw | `--hw` | `--lmce` | 隔离行为（CFI 加载，inactive 模式） | 备注 |
|---|------|--------|--------|---------|--------------------------------|------|
| 01 | mem CE | ✅ | ❌ | ❌ | 阈值后软下线 page | CE 走 CMC IRQ，HCE2 FMA 拦 |
| 02 | mem SRAO | ✅ | ✅ | ❌ | 硬下线 page，cpu 不动 | LMCE 路径要求 S/AR 位 |
| 03 | **mem SRAR** | ✅ | ✅ | ⭐ **panic demo** | 硬下线 page + SIGBUS owner | **`--lmce` 唯一真实用例**（S+AR）|
| 04 | cache UCE L2 | ✅ | ✅ | ❌ | **隔离 cpu**（L2 per-core）| L2 是 per-core，单核隔离正确 |
| 05 | cache UCE L3 | ✅ | ✅ | ❌ | **不隔离 cpu**（L3 socket 共享，默认 `isolate_on_l3_uce=0`）| 仅记账 + netlink；可 sysfs 切换 |
| 06 | cache CE L2 | ✅ | ❌ | ❌ | 仅记账 | CE 走 CMC，FMA 拦 |
| 07 | TLB UCE | ✅ | ✅ | ❌ | 仅记账（脚本 force `auto_isolate=0`，只验证分类）| TLB 物理上 per-core，去 hack 后可隔离 |
| 08 | Bus UCE | ✅ | ✅ | ❌ | 仅记账（同上）| Bus 错误归属可能模糊，默认不真隔离 |
| 09 | hwpoison | ✅ | (sw) | (sw) | 硬下线 page | 不走 MCE 链路 |
| 10 | hw mem CE | – | ❌ | ❌ | 同 01 | 同 case 01 hw 路径 |
| 11 | hw mem SRAO | – | ✅ | ❌ | 同 02 | 同 case 02 |
| 12 | **hw mem SRAR** | – | ✅ | ⭐ **panic demo** | 同 03 | 同 case 03，"始终 hw"入口 |
| 13 | hw cache UCE L2 | – | ✅ | ❌ | **隔离 cpu** | 同 case 04 |
| 14 | hw cache CE L2 | – | ❌ | ❌ | 仅记账 | 同 case 06 |
| 15 | **L1D Cache UCE** | ✅ | ✅ | ❌ | **隔离 cpu**（L1 per-core）| 新增；L1 simple-code，分类成 L1D |
| 16 | **L1I Cache UCE** | ✅ | ✅ | ❌ | **隔离 cpu**（L1 per-core）| 新增；compound 0x0801 (TT=Instr, LL=L1) |
| 17 | L1 Cache CE | ✅ | ❌ | ❌ | 仅记账 | 新增；CE 阈值前不隔离 |

图例：✅ 工作；❌ 该平台不投递；⭐ 强烈推荐用于 demo。

**三条平台限制 / 设计决策**：
1. **CE 经 hw raise 不投递**（cases 01/06/10/14/17 的 `--hw`）—— CE 走 CMC IRQ，HCE2 FMA 在这条 polling 路径上拦截、scrub MCi_STATUS。**用默认 sw 注入做 CE 测试**。
2. **`--lmce` 仅对自带 S/AR 位的注入有效**（即 case 03 / 12 SRAR）—— LMCE 路径要求 AR severity 才完整走 do_machine_check。**用 `--lmce` 做 panic demo 时选 03 或 12**。
3. **L3 cache UCE 默认不隔离 cpu**（case 05）—— L3 是 socket 共享（Intel CHA tile / AMD CCX-shared），隔离单核不解决根因。默认 `isolate_on_l3_uce=0`，仅记账 + netlink 让 daemon 决定。要恢复"激进保守"行为：`echo 1 > /sys/kernel/cfi/isolate_on_l3_uce`。L1/L2 cache 是 per-core，单核隔离仍正确，行为不变。

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
| 10 | `10_hw_mem_ce.sh` | 真 #MC 内存 CE | mce-inject(8) | 4 | `0x9c00…0094` | 真 #MC corrected handler 记录 | 同上 + cfi 通知器记账 |
| 11 | `11_hw_mem_srao.sh` | 真 #MC SRAO | mce-inject(8) | 4 | `0xbc00…0094` | mce_severity=AO，异步 memory_failure | 同上 + cfi 记账 + netlink |
| 12 | `12_hw_mem_srar.sh` | 真 #MC SRAR | mce-inject(8) | 4 | `0xbd80…0094` | **mce_panic + kdump** (sync timeout) | tolerant=3 门控 panic + decode chain → sync memory_failure，owner SIGBUS，整机存活 |
| 13 | `13_hw_cache_uce.sh` | 真 #MC L2 cache UCE | mce-inject(8) | 1 | `0xb800…000e` | **mce_panic + kdump**（已在物理机实证）| tolerant=3 门控 panic + cfi 按 mode 隔离 cpu，整机存活 |
| 14 | `14_hw_cache_ce.sh` | 真 #MC L2 cache CE | mce-inject(8) | 1 | `0x9000…000e` | CMC handler 记录 | cfi cache CE 累加 |

**真正能演示"宕机 vs 存活"的是 12 和 13**（已在 HCE2 + 2288H V5 物理机实证）。其它 case 演示的是"无策略动作 vs 有策略动作"——可以证明 CFI 分类与隔离链路工作，但不演示救机能力。

## 前置准备

```bash
# 0. 确保内核模块已加载
modprobe mce_inject
modprobe hwpoison_inject

# 1. 安装 mce-inject 用户态工具（cases 10–14 强依赖）
dnf install -y mce-inject || zypper install -y mce-inject

# 2. 编译仓库内的用户态工具（ownpage / cfimon）
make -C test/tools

# 3.（可选）启动 cfimon 抓 netlink，供 09 之外的 case 验证
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

## `--hw` / `--sw` / `--lmce`：同一个 case 切换注入路径

01–08 默认走 sw 路径（debugfs flags=sw，纯 `mce_log` 解码链）。加 `--hw` 参数后会切到**真 #MC 路径**（自动选 `mce-inject(8)` 用户态 raise / debugfs flags=hw 中能用的那条），整组 fault matrix 都能在两种路径下重复实验：

```bash
# 默认 sw 路径（不进 do_machine_check）
bash test/inject_cases/04_cache_uce_l2.sh

# 真 #MC 路径（do_machine_check → mce_severity → decode chain）
bash test/inject_cases/04_cache_uce_l2.sh --hw
```

输出 banner 会显示当前 `inject mode: sw/hw`。

`--hw` 模式下首次注入会执行一次 hw 投递探测（mce-inject 用户态优先，回退 debugfs flags=hw），探测成功后才发起真实注入。探测失败会直接 abort，给出原因（WRMSR #GP / 两条路径都没投递 / 工具缺包）。

**UC 类（02/03/04/05/07/08）+ `--hw` + 未加载 CFI = 几乎肯定 panic + kdump**（Intel 广播 MCE 同步超时），脚本会给 5 秒 Ctrl-C 窗口提醒确认 kdump 状态。CE 类（01/06）不带 UC 位，hw 模式下走 CMC handler，不会 panic。

### `--lmce`：更接近真实硬件的注入语义

默认 `--hw` 的 panic 路径是 **mce-inject 的工艺**——它只在单 CPU 上 raise，其它核来不及响应，`mce_reign` 同步超时 → `mce_panic("Some CPUs didn't answer in synchronization")`。真实硬件 MCA 微码会广播给所有核，sync 必过，**真正的 panic 是 `mce_severity` 决策（AR/PCC=1 + tolerant<3）触发的**。

`--lmce` 把 `MCGSTATUS` 拉到 `0xF`（`MCIP|EIPV|RIPV|LMCE_S`），告诉内核这是一个**局部 MCE**，不走广播同步。这样 panic 走 `mce_severity` 那条决策路径，dmesg 里看到的就是 `Fatal machine check on current CPU` —— 真实硬件的标准信息，对外汇报"无 CFI = 真实硬件故障下整机宕机"更有说服力。

```bash
# 物理机上验证 LMCE-style 真实路径
bash test/inject_cases/04_cache_uce_l2.sh --lmce        # implies --hw
# 等价于:
bash test/inject_cases/04_cache_uce_l2.sh --hw --mcgstatus=0xf
```

需要 CPU 支持 LMCE（Skylake+ 服务器型号都支持）。`--lmce` 在加载 CFI 后跑会触发 inactive 隔离（与默认 --hw 一样的隔离效果）；在没有 CFI 时会以 severity-driven 路径触发 panic（即真实硬件意义上的 mce_panic）。

`--mcgstatus=X` 暴露出来给做实验时手动改各 bit，例如：
- `0xE` = `MCIP|EIPV|LMCE_S`（去掉 RIPV，PCC=1 的非可恢复场景）
- `0x7` = `MCIP|EIPV|RIPV`（不加 LMCE，仍走广播路径，方便对照）

### `--lmce` 的两条已知边界（HCE2 + 2288H V5 物理机实测）

**边界 1：`--lmce` 对没有 `S`/`AR` 位的事件观察不到信号。**

LMCE 设置 `MCGSTATUS.LMCE_S` 后，内核 do_machine_check 优先看严重性档位。不带 S/AR 位的 UC（SRAO、cache UCE simple-code、TLB UCE、Bus UCE、CE）会走 `machine_check_poll` 这条 polling 路径；MCi_STATUS 又可能被 FMA / SMM scrub，最终既不 panic 也不投递到 decode chain。**`--lmce` 实际只对 SRAR（自带 S+AR）类型的注入有明显效果**（cases 03、12）。其它 case 想走真 #MC 全链路应该用默认 `--hw`（广播路径），CFI notifier 反而能在 broadcast 同步完后接管整个 decode chain。

实测对照：

| Case | `--hw`（broadcast）信号 | `--lmce`（LMCE_S）信号 |
|------|-------------------------|------------------------|
| 02 mem SRAO   | uce_async +2，page offlined ✅ | 无 ❌ |
| 03 mem SRAR   | demoted to async，page offlined ✅ | **触发 `Fatal local machine check` panic ⭐** |
| 05 L3 UCE     | uce_count +1 ✅ | 无 ❌ |
| 06 cache CE   | 无（CE 走 CMC，FMA 拦） | 无 ❌ |
| 07 TLB UCE    | uce_count +1，error_types=0x10 ✅ | 无 ❌ |
| 08 Bus UCE    | uce_count +1，error_types=0x30 ✅ | 无 ❌ |

**边界 2：`--lmce` + AR 严重性 + 内核态 → CFI 拦不住 panic（这是真实硬件下也无解的边界）。**

03 `--lmce` 即使在 CFI 加载的情况下，仍然 panic：

```
mce: [Hardware Error]: Machine check: Action required: unknown MCACOD
Kernel panic - not syncing: Fatal local machine check
cpu_fault_isolate: PANIC on cpu0: Fatal local machine check
cpu_fault_isolate: CFI was unable to prevent this panic.
```

CFI 的 panic_notifier 确实 fire 了，但**没法阻止** —— 因为这条 `mce_panic("Fatal local machine check")` 是 `do_machine_check` 在 `no_way_out` 路径里**无条件触发**的，不被 `mce_tolerant` 门控。原因很硬：内核态遇到 AR 严重性错误，**没有用户进程可以 kill 来恢复**，内核只能死。

**这不是 CFI 的缺陷，是内核 MCE 设计的边界**。CFI 覆盖的是：
- ✅ 广播 sync 超时 panic（`mce_tolerant=3` 门控）
- ✅ 非 AR 严重性事件（不到 mce_panic 就处理完了）
- ❌ 内核态 AR 严重性事件（无论 tolerant 设多少都 panic）

对外汇报时这是个**应该主动讲**的点 —— 它确立了 CFI 价值的真实边界，避免被反问"那真坏 DIMM 命中内核态的时候 CFI 救得了吗"。诚实答："救不了，那种情况下真实硬件下也会 panic；CFI 把不必要的 panic（mce-inject 工艺、广播 sync 误判）拦住了。"

09 hwpoison_inject 走 `memory_failure` 直调，跟 MCE 通路无关，`--hw` 不适用，参数会被忽略。

10–14 是"始终 hw"快捷形态（永远走真 #MC，无 `--sw` 选项），主要用来固化 panic-vs-isolate 演示与对外说法。功能上 `01–06 --hw` 与 10–14 等价；保留 10–14 是为了让"hw 路径"这条话术在脚本目录里一眼可见。

## hw 模式 case（10–14）跑之前

cases 10–14 通过 `mce-inject(8)` 用户态工具触发真 #MC（工具内部把 debugfs `flags` 改为 `raise` 然后 WRMSR + IPI-NMI），不再依赖之前那个不可靠的"debugfs flags=hw 投递探测"。

12 / 13 在**无 CFI** 时**必然**触发 panic + kdump（不是"如果 tolerant<3"才会，而是 Intel 广播 MCE 同步超时这条独立路径，与严重性档位无关）。跑之前确认：

```bash
# kdump 已启用 & 配置了 dump 路径
systemctl status kdump

# /proc/sys/kernel/panic > 0，否则 panic 后机器不会自动 reboot
sysctl kernel.panic
```

## 安全栏

- `mce_submit` 与 `mce_inject_file` 都拒绝向已离线的 cpu 注入（`smp_call_function_single` 会卡死）；
- 04/13 case 隔离 CPU 后会自动尝试 `echo 1 > .../online` 把它拉回（full 模式才会有变化，inactive 模式 cpu 本就在线）；
- 12/13 case 在 CFI 未加载时会给 5 秒 Ctrl-C 窗口，确认 kdump 状态后再继续。
- full 模式在 HCE2 `r2673_211_284` 及更老物理机上会触发 `percpu_counter_cpu_dead` 硬死锁；需要先打 `livepatch_percpu_counter` 热补丁才能用。inactive 是 HCE2 物理机的推荐默认。

## 这套对外能讲什么

- **CFI 故障覆盖面**：内存 CE/SRAO/SRAR、cache CE/UCE（L2/L3）、TLB UCE、bus UCE、外加内核态 memory_failure 全路径 + 真 #MC 全路径 = 一套完整的 x86 MCE → 隔离策略验证矩阵。
- **加载与不加载对比**：每一类故障在无 CFI 时的内核默认行为是什么，有 CFI 后多出哪些动作（隔离、记账、netlink、panic 抑制），都有可重复的对照输出。
- **整机宕机 vs 存活**：12/13 两个 case 能在物理机上演示，前提是 hw 注入路径未被 vendor 屏蔽。
