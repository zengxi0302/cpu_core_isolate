# CPU 核心故障自隔离（CFI）— 技术设计文档

**版本**: 0.2.0  
**日期**: 2026-06  
**作者**: openEuler Community  
**状态**: 实现完成，验证中

---

## 1. 背景与问题定义

### 1.1 现状痛点

公有云计算节点（宿主机）运行大量虚拟机（VM），当 CPU 出现以下硬件故障时：

| 故障类型 | 内核默认行为 | 影响 |
|---------|------------|------|
| Cache UCE（L1/L2/L3 不可纠正错误） | `mce_panic()` → 整机宕机 | **所有 VM 全部丢失** |
| 硬锁（Hardlockup） | `panic()` → 整机宕机 | **所有 VM 全部丢失** |
| 软锁（Softlockup） | `panic()` → 整机宕机 | **所有 VM 全部丢失** |

**核心矛盾**：单个 CPU 核心故障 → 牵连整机 100+ 个核心上的所有 VM。

### 1.2 设计目标

1. **拦截 panic**：阻止单核故障导致的整机宕机
2. **故障隔离**：将故障 CPU 从调度域中摘除（offline）
3. **有限牺牲**：仅丢失绑定在故障 CPU 上的 1 个 VM
4. **保全大局**：其余 VM 继续正常运行
5. **上报降级**：通过 netlink 通知上层调度系统，触发 VM 热迁移

### 1.3 约束条件

- 以内核模块（ko）形式交付，不改内核源码
- 兼容 openEuler 2403 LTS / HCE3（华为云 EulerOS 3.0）/ Linux 6.6
- 支持 x86_64（Intel/AMD）和 aarch64（鲲鹏 920/930）
- 与 rasdaemon、EDAC、mcelog 共存

---

## 2. 系统架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                        cpu_fault_isolate.ko                         │
├──────────────────────┬──────────────────────┬───────────────────────┤
│   Panic Suppression  │   Error Accounting   │   CPU Isolation       │
│   Layer              │   & State Machine    │   Engine              │
│                      │                      │                       │
│  • mce_tolerant=3    │  • Per-CPU counters  │  • Deferred work      │
│  • softlockup=0      │  • Sliding window    │  • remove_cpu()       │
│  • hardlockup=0      │  • Threshold logic   │  • Daemon deferral    │
│  • kprobe fallback   │  • State transitions │  • Cross-CPU sched    │
├──────────────────────┼──────────────────────┼───────────────────────┤
│  Lockup Detection    │   Netlink/Sysfs      │   Arch Backends       │
│                      │   Interface          │                       │
│  • hrtimer heartbeat │  • genl "CFI"        │  • x86: MCE decode    │
│  • kthread watchdog  │  • Per-CPU sysfs     │  • x86: FMA (HCE3)   │
│  • Global monitor    │  • debugfs inject    │  • arm64: GHES/APEI   │
└──────────────────────┴──────────────────────┴───────────────────────┘
```

---

## 3. 关键设计决策

### 3.1 时序问题（核心挑战）

**原始问题**：`do_machine_check()` 的执行顺序是：

```
MCE 中断 → do_machine_check()
  ├── mce_severity() 评估严重性
  ├── if worst >= MCE_PANIC_SEVERITY:
  │     └── mce_panic() → panic() → 系统死亡  ← 在这里就结束了！
  └── 只有非致命 MCE 才走到:
        └── mce_log() → decode chain → CFI notifier  ← 太晚了
```

**解决方案**：在 MCE handler 注册之前，先设置 `mce_tolerant=3`，使 `do_machine_check()` 跳过 `mce_panic()` 分支，所有 MCE（包括 fatal）都流入 decode chain。

### 3.2 tolerant 设置方案（HCE3 适配）

| 方法 | 适用内核 | 机制 |
|------|---------|------|
| **sysfs 写入** | upstream 6.6 | `echo 3 > /sys/.../tolerant` |
| **kprobe 直写** | HCE3（无 sysfs） | kprobe 查找 `mce_tolerant` 符号地址，直接写内存 |

```c
// kprobe 技巧：获取未导出符号地址
static unsigned long cfi_lookup_name(const char *name)
{
    struct kprobe kp = { .symbol_name = name };
    unsigned long addr;
    if (register_kprobe(&kp) < 0) return 0;
    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    return addr;
}

// 直接设置 tolerant
int *ptr = (int *)cfi_lookup_name("mce_tolerant");
*ptr = 3;
```

### 3.3 HCE3 FMA 框架集成

HCE3 内核额外提供了两个钩子：

| 钩子 | 调用时机 | CFI 用途 |
|------|---------|---------|
| `fma_mce_do_chain` | `do_machine_check()` 内部 | 早期 MCE 事件获取 |
| `mce_panic_chain` | `mce_panic()` 执行前 | 最后防线日志记录 |

CFI 通过 kprobe 运行时探测这些符号，有则注册，无则跳过，保持对 upstream 内核的兼容。

### 3.4 Lockup 检测（独立于内核 watchdog）

抑制 `softlockup_panic` 和 `hardlockup_panic` 后，内核只记录 lockup 不采取行动。CFI 自建检测并触发隔离：

```
┌─────────────────────────────────────────────────────────┐
│  Per-CPU hrtimer (4s)         Per-CPU kthread (4s)      │
│  ┌─────────────────┐         ┌─────────────────┐       │
│  │ atomic_inc(hb)  │         │ update jiffies  │       │
│  │ check sched_ts  │──miss──>│ "softlockup!"   │       │
│  └─────────────────┘         └─────────────────┘       │
│           │                                             │
│           │ Global Monitor (5s, runs on healthy CPU)    │
│           │ ┌──────────────────────────┐                │
│           └>│ for_each_online_cpu:     │                │
│             │   if hb stale 30s:       │                │
│             │     "hardlockup!"        │                │
│             └──────────────────────────┘                │
└─────────────────────────────────────────────────────────┘
```

### 3.5 跨 CPU 隔离调度

故障 CPU 可能无法处理自己的 workqueue（hardlockup），因此隔离工作必须在健康 CPU 上执行：

```c
void cfi_begin_isolation(unsigned int cpu, bool urgent)
{
    if (urgent && raw_smp_processor_id() == cpu) {
        target = cpumask_any_but(cpu_online_mask, cpu);
        queue_work_on(target, system_wq, &ci->offline_work);
    } else {
        schedule_work(&ci->offline_work);
    }
}
```

---

## 4. 状态机

```
           ┌──────────────────────────────────────────┐
           │                                          │
           v                                          │
    ┌──────────┐   CE >= thresh   ┌───────────┐      │
    │  ONLINE  │ ───────────────> │ DEGRADED  │      │
    └──────────┘                  └───────────┘      │
         │                              │            │
         │  UCE >= thresh               │ UCE >= thresh
         │  or Lockup                   │            │
         v                              v            │
    ┌────────────┐               ┌────────────┐     │
    │ ISOLATING  │               │ ISOLATING  │     │
    └────────────┘               └────────────┘     │
         │                              │            │
    ┌────┴────┐                    ┌────┴────┐      │
    v         v                    v         v      │
┌────────┐ ┌────────┐        ┌────────┐ ┌────────┐ │
│ISOLATED│ │ FAILED │        │ISOLATED│ │ FAILED │ │
└────────┘ └────────┘        └────────┘ └────────┘ │
    │                              │                │
    │          unisolate           │                │
    └──────────────────────────────┴────────────────┘
```

**Lockup 特殊路径**：跳过阈值逻辑，单次事件立即触发 ISOLATING。

---

## 5. 模块组成

| 文件 | 职责 | 行数 |
|------|------|------|
| `core/cfi_main.c` | 模块入口、参数、init/exit 编排 | ~240 |
| `core/cfi_core.c` | 错误计数、状态机、阈值判断 | ~200 |
| `core/cfi_hotplug.c` | CPU offline/online、daemon 协议 | ~260 |
| `core/cfi_panic_suppress.c` | panic 拦截、tolerant 设置、HCE3 适配 | ~270 |
| `core/cfi_lockup.c` | 独立 lockup 检测 | ~250 |
| `core/cfi_netlink.c` | Generic netlink 通信 | ~270 |
| `core/cfi_sysfs.c` | sysfs 接口 | ~280 |
| `core/cfi_debugfs.c` | 软件错误注入 | ~295 |
| `arch/x86/cfi_x86.c` | MCE decode chain + FMA | ~220 |
| `arch/x86/cfi_x86_cache.c` | MCA error code 分类 | ~170 |
| `arch/arm64/cfi_arm64.c` | GHES/APEI tracepoint | ~230 |
| `arch/arm64/cfi_arm64_cache.c` | ARM RAS 错误分类 | ~155 |

---

## 6. 接口设计

### 6.1 模块参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `ce_threshold` | 10 | CE 计数达到此值 → DEGRADED |
| `uce_threshold` | 1 | UCE 计数达到此值 → ISOLATING |
| `window_secs` | 3600 | 滑动窗口时长（秒） |
| `auto_isolate` | Y | 是否自动隔离 |
| `defer_to_daemon` | Y | 是否等 daemon ACK 再下线 |
| `defer_timeout_ms` | 30000 | daemon ACK 超时（毫秒） |
| `mce_tolerant` | 3 | MCE tolerant 目标值 |
| `lockup_thresh` | 30 | lockup 检测阈值（秒） |

### 6.2 Sysfs

```
/sys/kernel/cfi/                      # 全局配置
    ce_threshold, uce_threshold, window_secs, auto_isolate, version

/sys/devices/system/cpu/cpuX/cfi/     # Per-CPU 状态
    state, ce_count, uce_count, ce_total, error_types, user_pinned
```

### 6.3 Generic Netlink (family: "CFI")

| 方向 | 命令 | 用途 |
|------|------|------|
| K→U | ERROR_EVENT | 新错误记录 |
| K→U | STATE_CHANGE | CPU 状态变迁 |
| U→K | GET_STATUS | 查询 CPU 状态 |
| U→K | SET_POLICY | 动态调阈值 |
| U→K | ISOLATE | 手动隔离 |
| U→K | UNISOLATE | 恢复上线 |
| U→K | ACK_ISOLATE | Daemon 确认已迁移 VM |

### 6.4 Debugfs 注入

```bash
echo "cpu=4 type=cache_l2 severity=ucr" > /sys/kernel/debug/cfi/inject
```

---

## 7. Daemon 协作协议

```
  Kernel (CFI)                         Daemon (cfid)
      │                                     │
      │──── STATE_CHANGE(ISOLATING) ───────>│
      │                                     │
      │     [daemon migrates VM vCPUs]      │
      │                                     │
      │<──── ACK_ISOLATE ──────────────────│
      │                                     │
      │  cancel timer, proceed to offline   │
      │                                     │
      │──── STATE_CHANGE(ISOLATED) ────────>│
      │                                     │
      │     [daemon reports to scheduler]   │
```

**超时机制**：如果 daemon 在 `defer_timeout_ms`（默认 30s）内未 ACK，模块强制执行 CPU 下线。

---

## 8. 安全性考量

| 风险 | 缓解措施 |
|------|---------|
| tolerant=3 降低 MCE 保护 | CFI 通过 decode chain 主动处理 fatal MCE，比 panic 更有针对性 |
| 故障 CPU 上执行的代码可能损坏 | 紧急隔离确保 work 在其他 CPU 执行 |
| 最后一个 CPU 不能隔离 | `cpumask_any_but()` 检查，拒绝自隔离 |
| 模块卸载时恢复状态 | `cfi_suppress_exit()` 恢复所有原始值 |
| 与 EDAC/rasdaemon 冲突 | 使用 NOTIFY_OK 不阻断其他 handler |

---

## 9. 测试验证

### 9.1 已验证

- [x] 模块加载/卸载
- [x] sysfs 参数读写
- [x] debugfs 软件注入 CE → DEGRADED
- [x] debugfs 软件注入 UCE → ISOLATING → ISOLATED
- [x] mce-inject CE 通路
- [x] 窗口过期重置
- [x] user_pinned 阻止隔离
- [x] auto_isolate=0 只通知

### 9.2 待验证（HCE3 物理机）

- [ ] kprobe 设置 mce_tolerant=3 成功
- [ ] hw 模式 UCE 注入不 panic
- [ ] UCE 触发 CPU 隔离
- [ ] 隔离后 VM 仅丢失一个、其余存活
- [ ] fma_mce_do_chain 通知正常接收
- [ ] daemon 协作流程端到端

---

## 10. 后续演进

| 阶段 | 内容 |
|------|------|
| v0.3 | 用户态 daemon (cfid) 实现 + VM vCPU 迁移 |
| v0.4 | SMT sibling 联动隔离、NUMA 感知 |
| v0.5 | 对接 OpenStack Nova / K8s 调度器 |
| v1.0 | 产品化：RPM 包、systemd 服务、监控大盘 |
