# HCE 2.0（5.10 内核）移植与虚拟机验证说明

**分支**: `claude/hce2-510-port`
**目标环境**: 华为云 C7 实例 / HCE 2.0 / 5.10.x x86_64
**口径**: C7 计算节点宿主机同为 5.10 系内核，guest 内验证的软件路径
（模块逻辑、页隔离、netlink、甄别判决）与宿主机一致；真实 MCE 硬件
通路仍需物理机。

---

## 1. 移植内容（单一代码库 + 版本宏，未分叉）

| 差异点 | 6.x 行为 | 5.10 适配 |
|--------|---------|----------|
| folio API（5.16+） | `folio_test_lru/anon/hugetlb` | `PageLRU/PageAnon/PageHuge`（`PageHuge` 在 5.10 是导出函数） |
| `linux/panic.h`（5.18+ 拆分） | 直接包含 | 版本宏守卫，5.10 由 `kernel.h` 提供 panic 声明 |
| `kvcalloc/kvfree` 头文件 | `slab.h` 间接可见 | 显式包含 `linux/mm.h`（两边都正确） |
| mce tolerant | 5.19 起被移除，需 kprobe 直写 | **5.10 原生 sysfs 可用**，suppression 走标准路径（比 6.x 更顺） |
| `genl_small_ops` | 可用 | 5.10 恰好引入，可用 |
| `sysfs_emit` | 可用 | 5.10 引入，可用 |
| `soft_offline_page(pfn, flags)` | 同签名 | 5.4+ 已是 pfn 签名，kprobe 解析方案不变 |
| `mc_event`/`memory_failure_event` tracepoint | 同原型 | 已对照 5.15 头文件确认一致；对目标内核头编译，漂移会在编译期暴露 |

关键导出符号（5.15 Module.symvers 实测，5.10 上由一键脚本再次确认）：
`memory_failure_queue` / `__tracepoint_mc_event` / `for_each_kernel_tracepoint` /
`PageHuge` / `remove_cpu` / `add_cpu` 均为 EXPORT_SYMBOL_GPL；
`pfn_to_online_page` 在 5.10 为头文件内联（无需导出），5.12+ 为导出函数。

## 1.1 物理机 CPU 下线：cpu_subsys_offline 被打桩的绕过

虚拟机上 `remove_cpu()` 正常下线；但在 C7 计算节点这类物理机上，宿主的
健康守护代理（hostguard 之类）会把 `cpu_subsys_offline()` 打桩成直接返回
`-EINVAL`（防止 CPU 被随意摘除），导致 `remove_cpu()` 拿到 -22、CPU 并未
真正下线。

调用链 `remove_cpu → device_offline → bus->offline=cpu_subsys_offline(桩)
→ cpu_device_down → cpu_down`。`cfi_hotplug.c` 改为**三级递进下线**，每级
绕过更深一层：

| 级别 | 入口 | 导出状态 | 作用 |
|------|------|---------|------|
| 1 | `remove_cpu(cpu)` | 导出 | 正常路径，保持设备模型一致（VM / 未打桩主机） |
| 2 | `cpu_device_down(dev)` | 未导出，kprobe 解析 | 绕过 `cpu_subsys_offline` 桩 |
| 3 | `cpu_down(cpu, CPUHP_OFFLINE)` | 未导出（static），kprobe 解析 | 连 `cpu_device_down` 也被桩时的兜底 |

判定逻辑：`remove_cpu` 返回非 0 **且** `cpu_online(cpu)` 仍为真才升级
（若 CPU 实际已下线只是返回码异常，则不重复动作）。走 2/3 级后会手动
`dev->offline = true` 并补发 `KOBJ_OFFLINE` uevent——否则跳过了
`device_offline()` 的记账，后续 `add_cpu()/device_online()` 会误判 CPU 仍
在线而拒绝拉回。re-online 路径（`add_cpu → cpu_device_up → cpu_up`）做了
对称处理。

开关：模块参数 `offline_bypass`（默认 Y）；置 N 则只用 `remove_cpu`，
保留旧行为作为应急退路。加载时 dmesg 打印
`offline bypass resolved: cpu_device_down=ok ...` 表明各级符号解析情况。

## 1.2 物理机首次注入「系统卡死」根因与修复

bypass 让 cpu71 真的下线了（dmesg `smpboot: CPU 71 is now offline`），
但整机随后冻死、远程 panic。逐行还原日志后定位到三个**叠加**根因，全部
在本模块侧，已修复：

1. **offline work 跑在 system_wq 上**。HCE2 内核把 `cpu_down_maps_locked`
   改成用 `work_on_cpu()` 在 housekeeping CPU 上跑 `_cpu_down`（堆栈
   `work_for_cpu_fn → _cpu_down`），而 `work_on_cpu` 也排到 system_wq 并
   flush。外层（我们的 offline work）占着 system_wq worker 等内层、内层
   又抢 system_wq worker → 叠加洪泛后死锁。
   **修复**：改用独立的有序工作队列 `alloc_ordered_workqueue("cfi_hotplug")`
   （`WQ_MEM_RECLAIM`），与 system_wq 隔离，且 `max_active=1` 串行化下线，
   unbound 池不会调度到已卡死的 CPU。
2. **lockup 检测器雪崩**。下线过程本身会瞬时拖停其他 CPU（stop-machine /
   IRQ 迁移 / `work_on_cpu` teardown），心跳变陈旧，我们的检测器误判为
   lockup 又去隔离 cpu0/cpu5，往死锁上加码。
   **修复**：新增 `cfi_offline_in_progress()`（in-flight 原子计数）；下线
   执行期间 lockup 检测器整轮暂停、`cfi_report_error` 的 lockup 分支也直接
   跳过，不标记 `lockup_reported`（避免永久屏蔽后续真实检测）。
3. **cpu0 被卷入隔离**。cpu0 是 boot CPU，且在 HCE2 上正是 `work_on_cpu`
   跑 teardown 的宿主，隔离它 = 自掘坟墓。
   **修复**：新增 `protect_cpu0`（默认 Y），`cfi_begin_isolation` 单一
   chokepoint 拒绝隔离受保护 CPU 与最后一个在线 CPU。

测试脚本侧（非内核 bug，但会加剧）：`mce_inject` 增加「目标 CPU 离线则
跳过」护栏（`smp_call_function_single` 打正在下线的 CPU 会硬卡死）；隔离
测试改用中段安全 CPU（非 cpu0、非末位），A4b–A7 关 `auto_isolate` 只验
分类记账，隔离能力由 A4 单独验证；重新上线走稳健重试函数。

## 1.3 完整下线在 HCE2 上仍死锁 → 三档隔离模式（最终方案）

加固后第二次实测：`cpu36` 真的下线了（`smpboot: CPU 36 is now offline`），
但 `cfi_offline_work_fn` 的 kworker 仍 D 住、整机再次雪崩。剖析两个 D 进程：

- pid7 `kworker/u144:0+cfi_hotplug`：**持有 `cpus_write_lock`**，卡在
  `cpu_device_down → cpu_down_maps_locked → work_on_cpu → __flush_work`
  （HCE2 把 `_cpu_down` 用 `work_on_cpu` 包到 system_wq 同步等待）。
- pid554 `kworker/36:1+events`：绑在**正在下线的 cpu36** 上的 per-cpu
  worker，跑 cgroup-v1 `cpuset_hotplug_workfn`，卡在 `cgroup_attach_lock →
  cpus_read_lock`——被 pid7 的 write lock 挡死。

死锁闭环：cpu36 下线要排空 cpu36 的 worker pool（含正在执行、卡在 read
lock 的 pid554）→ pid554 等 read lock → read lock 被下线持有的 write lock
挡 → 下线完不成。**这与我们用什么上下文调用无关**，是 HCE2 定制内核
（`work_on_cpu` 包装 `_cpu_down` + legacy v1 cpuset 的 in-flight hotplug
work 争 `cpus_rwsem`）的固有缺陷。厂商把 `cpu_subsys_offline` 打桩成
-EINVAL，本意大概率就是**禁止整机做完整 hotplug offline**——硬绕过等于
撞上他们要规避的死锁。另外那条
`WQ_MEM_RECLAIM ... is flushing !WQ_MEM_RECLAIM` 警告证实 1.2 里给
`cfi_hotplug` 加 `WQ_MEM_RECLAIM` 是错的（reclaim wq 不应 flush 非 reclaim
wq），已去掉。

**结论**：完整 hotplug offline 在该机型不可用。但只迁中断（软隔离）的
强度有上限——调度器仍会向故障核派 user/kernel 任务。新增**第三档
INACTIVE 模式**：把故障核从 `cpu_active_mask` 移除 + 迁中断，CPU 仍 online，
但调度器停止派新任务、load balancer 把已有可迁移任务推走（数毫秒内完成）。

### 1.3.1 INACTIVE 模式的实现路径

`set_cpu_active(cpu, false)` 是上游 API，但在 HCE 5.10 内核里被 inline，
kallsyms 里查不到（实测）；它操作的 `__cpu_active_mask` 反而是
`EXPORT_SYMBOL`。我们直接 `cpumask_clear_cpu(cpu, cpu_active_mask)` 等价
于 set_cpu_active(false) 对调度器的影响，**完全不依赖 kprobe，编译时 link
就拿到**。

放弃了 `set_cpu_active` 同时更新的 `sched_smt_active` 计数：那个只影响 SMT
兄弟核协同调度的启发式，对"故障核不被选中"这个目标不是 load-bearing。

### 1.3.2 为什么 INACTIVE 模式不死锁

HCE2 的 ABBA 死锁需要三件事同时成立：(a) 调用持有 `cpus_write_lock` 的
API；(b) 进入 CPU-hotplug 状态机；(c) 用 `work_on_cpu(target_cpu, ...)` 把
work 排到正在下线 CPU 的 events 池。INACTIVE 路径**三个都不沾**：

- `cpumask_clear_cpu` 是 atomic bit clear，无锁；
- 不进 `_cpu_down` 状态机，所以不会触发 `cpuset_hotplug_workfn` 调度；
- 不调用任何 `work_on_cpu`。

### 1.3.3 隔离强度对比

| 模式 | 机制 | User-mode tasks | 内核态非 per-cpu tasks | 中断 | per-cpu kthread | 死锁风险 |
|------|------|---|---|---|---|---|
| `full` | hotplug offline | 不调度 ✓ | 不调度 ✓ | 不投递 ✓ | 退出 ✓ | HCE2 上**会死锁** |
| `inactive`（推荐） | `cpumask_clear(cpu_active)` + 迁中断 | 不调度 ✓ | 不调度 ✓ | 不投递 ✓ | 仍跑（idle/short） | **无** |
| `soft`（最弱） | 仅迁中断 | 仍可能调度 ✗ | 仍可能调度 ✗ | 不投递 ✓ | 仍跑 | **无** |

`inactive` 与 `full` 的差距仅在「per-cpu kthread 还会跑」一项；这些都是
idle/migration/ksoftirqd/cpuhp/rcu_sched，几乎不消耗故障 cache，对"避免
再次 MCE/panic"目标 ≥ 99% 等价 `full`。

### 1.3.4 模块参数

- 新参数 `isolation_mode=full|inactive|soft`（charp，默认 `full`）。
- 旧参数 `soft_isolation`（bool）保留作为 deprecated 别名：=1 强制 SOFT。
- VM 一键脚本沿用默认 `full`（VM 上没有该死锁）；
- 物理机一键 `run_real_inject.sh` 默认 `--mode=inactive`；
  `--mode=full` / `--mode=soft` / `--offline` 可显式覆盖。
- 加载时 dmesg 打印 `isolation mode: <name>` 与该模式描述行。

## 2. 本开发环境的编译验证矩阵

| 内核 | 来源 | 结果 |
|------|------|------|
| 5.15.0-97（jammy） | 与 5.10 同代际：无 folio、有 tolerant | 零警告编译 |
| 6.8.0-124（noble） | 回归基线 | 零警告编译 |
| 5.10.x（HCE 2.0） | 无法从本环境获取（仓库不在出网白名单） | 由 VM 一键脚本现场编译 |

策略单元测试（18 用例）与用户态工具在两个分支上均通过/构建成功。

## 3. 虚拟机一键验证

本开发环境出网受白名单限制（22 端口与目标 IP 均被代理拒绝），无法直连
ECS。在云服务器上执行：

```bash
# 从你的终端把仓库推到服务器（二选一）
scp -r cpu_core_isolate root@120.46.95.93:/root/
# 或在服务器上: git clone -b claude/hce2-510-port <repo-url>

# 服务器上执行（root）
cd /root/cpu_core_isolate
bash test/run_vm_validation.sh              # P0-P6 + P8，约 2 分钟
bash test/run_vm_validation.sh --with-panic # 追加 P7 受控 panic（机器重启!）
```

输出：终端实时 PASS/FAIL + `/root/cfi_validation_report.txt`。
`--with-panic` 会先设 `kernel.panic=10`，panic 后约 10 秒自动重启；
重启后用 `journalctl -k -b -1 | grep 'MFI: unrecoverable'` 确认受控
panic 出自甄别引擎。

## 4. 脚本覆盖范围（P0–P8）

| 阶段 | 内容 | 验证点 |
|------|------|--------|
| P0 | 环境/编译 | kernel-devel、CONFIG_MEMORY_FAILURE/X86_MCE、模块零错编译、工具与 18 项单测 |
| P1 | 加载/接口 | insmod、sysfs 双子树、debugfs、soft_offline 符号解析、tolerant=3（5.10 原生 sysfs 路径）|
| P2 | CPU 域 | CE 计数→DEGRADED、UCE→隔离→手动恢复、user_pinned、auto_isolate=0、窗口重置 |
| P3 | netlink | CPU_ERROR 组播、MEM_GET_STATUS 往返 |
| P4 | MFI | 页 CE 记账、阈值→soft offline（属主存活=无损）、uce_srao→memory_failure、HWPoison 去重、MEM_* 事件 |
| P5 | 真实毒页 | madvise(MADV_HWPOISON) → SIGBUS（内核原生 memory_failure 消费路径）|
| P6 | 甄别 RECOVER | mem_triage=1，内核态 UCE 落用户页 → triage_saved、MEM_VM_KILLED、不误 panic |
| P7* | 甄别 PANIC | RIPV=0 → 受控 panic + 自动重启（opt-in）|
| P8 | 卸载 | rmmod 干净、sysfs 清理、tolerant 恢复、重载循环 |

VM 内天然测不到（物理机阶段）：真实 MCE 硬件通路、EDAC DIMM 定位、
patrol scrub、EINJ、HCE3 FMA 钩子。

## 5. 物理机验证指南（INACTIVE 模式）

适用于已在物理机上撞过完整下线死锁（pid7 `cpu_device_down` D 住 + pid554
`cpuset_hotplug_workfn` D 住）或 percpu_counter hardlockup 的 HCE2 物理机。
本节默认 git 上已 fast-forward 到含本次改动的最新 commit。

### 5.1 编译与加载

```bash
cd /path/to/cpu_core_isolate     # 你的物理机本地仓库
git pull                          # 拉到本次提交

# 二进制构建（kernel-devel/gcc/make 须就绪）
make -C kernel KDIR=/lib/modules/$(uname -r)/build
make -C test/tools

# 卸载旧的，加载新的 (inactive)
rmmod cpu_fault_isolate 2>/dev/null
modprobe mce_inject hwpoison_inject

# 显式指定模式，避免误用 full 撞死锁
insmod kernel/cpu_fault_isolate.ko isolation_mode=inactive

# 确认模式生效
dmesg | tail -20 | grep -E "isolation mode|inactive"
# 期望看到:
#   cpu_fault_isolate: isolation mode: inactive
#   cpu_fault_isolate:   (set_cpu_active(false) + IRQ migration — recommended for HCE2 physical hosts)

cat /sys/module/cpu_fault_isolate/parameters/isolation_mode
# 期望: inactive
```

### 5.2 单点 A4 烟雾测试（最重要的一项，先验证不死锁）

挑一个非 cpu0、非末位的核做隔离目标，避开物理机 housekeeping CPU：

```bash
TGT=$(( $(nproc) / 2 ))   # 例如 72 核机器上是 cpu36

# 注入 L2 cache UCE 触发隔离
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo "cpu=$TGT type=cache_l2 severity=ucr" > /sys/kernel/debug/cfi/inject
sleep 2

# 期望状态
echo "cfi state:      $(cat /sys/devices/system/cpu/cpu$TGT/cfi/state)"   # → isolated
echo "online:         $(cat /sys/devices/system/cpu/cpu$TGT/online)"      # → 1 (仍 online!)
echo "online_mask:    $(cat /sys/devices/system/cpu/online)"
# 检查 cpu_active_mask 已剔除 TGT（间接 — 看调度器是否还派任务给它）
```

dmesg 应看到（**没有** D 住进程，**没有** `WARNING: WQ_MEM_RECLAIM`，**没有**
hardlockup）：
```
cpu_fault_isolate: cpuN: online -> isolating (...)
cpu_fault_isolate: cpuN: inactive-isolating (sched_active=false + IRQ migration)
cpu_fault_isolate: cpuN: inactive-isolated (cleared from cpu_active_mask + migrated K IRQs; CPU stays online)
cpu_fault_isolate: cpuN: isolated successfully via inactive (total isolations: 1)
```

观测窗口（隔离后 30s 内）：
```bash
top -p 0 -d 5            # cpu$TGT 的 %us+%sy 应迅速降到接近 0
cat /proc/stat | awk -v c=$TGT 'NR==c+2 {print}'   # 看 user+system 增长应停滞
ps -eo psr,pid,comm | awk -v c=$TGT '$1==c'        # 应该几乎为空（只剩 per-cpu kthread）
```

判定：**没有死锁、没有重启、cpu 真的不再被调度** → 通过。

### 5.3 全套真实硬件路径验证

```bash
bash test/run_real_inject.sh                  # 默认 --mode=inactive
bash test/run_real_inject.sh --hw             # 加 hw 注入（物理机上 #MC 是真实的）
bash test/run_real_inject.sh --mode=soft      # 对比：仅迁中断（旧默认）
# 物理机上不要跑 --mode=full / --offline，会死锁
```

artifact 落 `/home/cfi_logs/`：`real_inject_report.txt`、`real_inject_dmesg.txt`、
`real_inject_cfimon.log`。A4 在 inactive 模式下应输出 `state=isolated online=1
cleared from cpu_active_mask + migrated N IRQs`。

### 5.4 解隔离

```bash
# rmmod 时会调 cfi_unisolate_cpu 对所有隔离 CPU 自动回滚
rmmod cpu_fault_isolate
# 或运行时单点回滚（需 daemon/CLI 支持，目前以 rmmod 整体回滚为主）
```

dmesg 期望：
```
cpu_fault_isolate: cpuN: inactive isolation cleared (re-added to cpu_active_mask)
```

### 5.5 失败排查

| 现象 | 原因 / 处置 |
|------|-----|
| 加载时 `isolation mode: full` 但参数 `inactive` | 命令行错；检查 `cat /sys/module/.../parameters/isolation_mode` |
| 隔离后 cpu 还有任务 | per-cpu kthread 正常；user task 检查 `taskset -p <pid>` 是否被绑死 |
| 解隔离后 cpu 仍空闲 | scheduler 域域未重建是预期；任务自然落到 `cpu_active_mask` 重新涵盖的核 |
| pid `D` 在 `cpu_device_down` 路径 | 用错模式（full/offline）；改 inactive |
| dmesg 出现 `cleared from cpu_active_mask` 但 `cpu_active_mask` 文件不变 | sysfs 没有 `cpu_active_mask` 文件，要通过观测调度行为间接验证 |
