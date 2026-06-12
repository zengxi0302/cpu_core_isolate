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
