# CPU Fault Isolation (CFI) - 编译、故障注入与测试验证指南

## 目录

1. [开发环境与限制说明](#1-开发环境与限制说明)
2. [编译环境搭建](#2-编译环境搭建)
3. [内核模块编译](#3-内核模块编译)
4. [模块加载与基础验证](#4-模块加载与基础验证)
5. [故障注入方法详解](#5-故障注入方法详解)
6. [测试验证流程](#6-测试验证流程)
7. [测试矩阵](#7-测试矩阵)
8. [常见问题排查](#8-常见问题排查)
9. [内存故障域（MFI）测试](#9-内存故障域mfi测试)

---

## 1. 开发环境与限制说明

### 当前限制

本项目的内核模块 **无法在 macOS 上编译和测试**，原因：

- Linux 内核模块 (`.ko`) 只能在 Linux 环境下编译，依赖 Linux kernel headers
- 故障注入 (MCE inject / EINJ) 需要运行在 Linux 物理机或支持嵌套虚拟化的 VM 上
- x86 MCA/MCE 测试需要 x86_64 Linux 环境
- ARM64 RAS 测试需要 aarch64 Linux 环境（最佳为鲲鹏物理机）

### 所需测试环境

| 环境 | 用途 | 最低要求 |
|------|------|----------|
| **openEuler 2403 SP3 x86_64 物理机** | x86 MCA/MCE 全功能测试 | 多核 Intel/AMD 服务器，支持 ACPI EINJ |
| **openEuler 2403 SP3 aarch64 物理机** | ARM64 RAS 全功能测试 | 鲲鹏 920/930 服务器 |
| **openEuler 2403 SP3 x86_64 虚拟机** | 编译 + 基础功能测试 | 4 vCPU / 8GB RAM / 40GB 磁盘 |
| **openEuler 2403 SP3 aarch64 虚拟机** | 编译 + 基础功能测试 | 4 vCPU / 8GB RAM / 40GB 磁盘 |

> **推荐**：初始开发使用 x86_64 虚拟机编译和做 debugfs 软件注入测试；
> 硬件级故障注入在物理机上进行。

---

## 2. 编译环境搭建

### 2.1 openEuler 2403 SP3 虚拟机创建 (在 macOS 上)

#### 方法 A: 使用 UTM/QEMU (推荐用于 macOS)

```bash
# 下载 openEuler 2403 SP3 ISO
# x86_64:
wget https://repo.openeuler.org/openEuler-24.03-LTS-SP3/ISO/x86_64/openEuler-24.03-LTS-SP3-x86_64-dvd.iso

# aarch64 (如果 macOS 是 ARM 架构):
wget https://repo.openeuler.org/openEuler-24.03-LTS-SP3/ISO/aarch64/openEuler-24.03-LTS-SP3-aarch64-dvd.iso
```

在 UTM 中创建虚拟机：
- CPU: 4 cores
- 内存: 8GB
- 磁盘: 40GB
- 网络: Shared/Bridged

#### 方法 B: 使用远程 Linux 服务器

如果有远程 openEuler 服务器，直接 SSH 连接：
```bash
ssh root@<server-ip>
cat /etc/openEuler-release    # 确认版本
uname -r                       # 确认内核版本 (应为 6.6.x)
uname -m                       # 确认架构
```

### 2.2 安装编译依赖

```bash
# openEuler 2403 SP3 使用 dnf 包管理器
sudo dnf update -y

# 内核模块编译必需
sudo dnf install -y \
    kernel-devel \
    kernel-headers \
    gcc \
    make \
    elfutils-libelf-devel

# 验证 kernel-devel 安装
ls /lib/modules/$(uname -r)/build/
# 应该看到 Makefile, include/, scripts/ 等

# 可选：用于后续用户态组件
sudo dnf install -y \
    sqlite-devel \
    libvirt-devel \
    json-c-devel

# 可选：用于测试和调试
sudo dnf install -y \
    rasdaemon \
    edac-utils \
    strace \
    perf
```

### 2.3 验证编译环境

```bash
# 确认 GCC 版本
gcc --version

# 确认内核版本与 kernel-devel 匹配
uname -r
rpm -q kernel-devel
# 两者的版本号应该一致

# 如果不一致，安装匹配的 kernel-devel：
sudo dnf install -y kernel-devel-$(uname -r)

# 测试编译环境 (用一个最小的 hello-world 模块)
mkdir -p /tmp/test_module && cat > /tmp/test_module/hello.c << 'EOF'
#include <linux/module.h>
#include <linux/kernel.h>
static int __init hello_init(void) { pr_info("hello\n"); return 0; }
static void __exit hello_exit(void) { pr_info("bye\n"); }
module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
EOF

cat > /tmp/test_module/Makefile << 'EOF'
obj-m += hello.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
EOF

cd /tmp/test_module && make
# 如果看到 hello.ko 生成，编译环境正常
ls -la hello.ko

# 测试加载
sudo insmod hello.ko
dmesg | tail -1     # 应显示 "hello"
sudo rmmod hello
dmesg | tail -1     # 应显示 "bye"

# 清理
cd / && rm -rf /tmp/test_module
```

---

## 3. 内核模块编译

### 3.1 获取源码

```bash
# 方法1: 从开发机拷贝 (推荐)
scp -r user@mac-host:~/zengxi/cpu_core_isolate /root/

# 方法2: 如果使用 git
git clone <repo-url> /root/cpu_core_isolate

cd /root/cpu_core_isolate
```

### 3.2 编译

```bash
cd /root/cpu_core_isolate/kernel

# 编译（自动检测当前运行内核）
make

# 指定特定内核源码路径编译（交叉编译或版本不匹配时）
# make KDIR=/usr/src/kernels/6.6.0-xxx.oe2403sp3.x86_64

# 成功后应看到:
ls -la cpu_fault_isolate.ko
# -rw-r--r-- 1 root root XXXXX ... cpu_fault_isolate.ko

# 查看模块信息
modinfo cpu_fault_isolate.ko
```

预期 `modinfo` 输出：
```
filename:       /root/cpu_core_isolate/kernel/cpu_fault_isolate.ko
version:        0.1.0
description:    CPU Core/Cache Fault Isolation for improved system reliability
author:         openEuler Community
license:        GPL
parm:           ce_threshold:Corrected errors per window to enter DEGRADED state (default: 10) (uint)
parm:           uce_threshold:Uncorrected errors per window to trigger isolation (default: 1) (uint)
parm:           window_secs:Error counting window duration in seconds (default: 3600) (uint)
parm:           auto_isolate:Automatically offline CPUs that exceed error threshold (default: Y) (bool)
parm:           defer_to_daemon:Wait for userspace daemon ACK before CPU offline (default: Y) (bool)
parm:           defer_timeout_ms:Max time to wait for daemon ACK in milliseconds (default: 30000) (uint)
```

### 3.3 编译错误排查

#### 错误: "No rule to make target 'modules'"

```bash
# kernel-devel 未安装或版本不匹配
rpm -q kernel-devel
uname -r
# 确保版本一致，如不一致：
sudo dnf install -y kernel-devel-$(uname -r)
```

#### 错误: "mce_register_decode_chain" undeclared (x86)

```bash
# 确认内核配置中启用了 MCE 支持
grep CONFIG_X86_MCE /boot/config-$(uname -r)
# 应显示: CONFIG_X86_MCE=y

# 确认符号已导出
grep mce_register_decode_chain /lib/modules/$(uname -r)/build/Module.symvers
```

#### 错误: ARM64 tracepoint 相关 (aarch64)

```bash
# 确认 GHES 和 RAS 支持已启用
grep -E "CONFIG_ACPI_APEI_GHES|CONFIG_RAS" /boot/config-$(uname -r)
# 应显示:
# CONFIG_ACPI_APEI_GHES=y
# CONFIG_RAS=y
```

### 3.4 交叉编译 (可选)

如果需要在 x86_64 上为 aarch64 编译（或反之）：

```bash
# 安装交叉编译工具链
sudo dnf install -y gcc-aarch64-linux-gnu

# 交叉编译
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     KDIR=/path/to/aarch64-kernel-source
```

---

## 4. 模块加载与基础验证

### 4.1 加载模块

```bash
# 加载（使用默认参数）
sudo insmod cpu_fault_isolate.ko

# 加载（自定义参数：降低阈值便于测试）
sudo insmod cpu_fault_isolate.ko \
    ce_threshold=3 \
    uce_threshold=1 \
    window_secs=60 \
    auto_isolate=1 \
    defer_to_daemon=0

# 检查加载结果
dmesg | grep "cpu_fault_isolate"
# 预期输出:
# cpu_fault_isolate: initializing (ce_thresh=3 uce_thresh=1 window=60s)
# cpu_fault_isolate: registering x86 MCA/MCE decode chain handler  [x86]
#   或
# cpu_fault_isolate: registering ARM64 RAS tracepoint probes        [arm64]
# cpu_fault_isolate: initialized successfully

# 确认模块已加载
lsmod | grep cpu_fault_isolate
```

### 4.2 检查 sysfs 接口

```bash
# 全局配置
cat /sys/kernel/cfi/version          # 0.1.0
cat /sys/kernel/cfi/ce_threshold     # 3 (或加载时设置的值)
cat /sys/kernel/cfi/uce_threshold    # 1
cat /sys/kernel/cfi/window_secs      # 60
cat /sys/kernel/cfi/auto_isolate     # 1

# Per-CPU 状态 (以 CPU 0 为例)
cat /sys/devices/system/cpu/cpu0/cfi/state        # online
cat /sys/devices/system/cpu/cpu0/cfi/ce_count      # 0
cat /sys/devices/system/cpu/cpu0/cfi/uce_count     # 0
cat /sys/devices/system/cpu/cpu0/cfi/ce_total      # 0
cat /sys/devices/system/cpu/cpu0/cfi/error_types   # 0x0
cat /sys/devices/system/cpu/cpu0/cfi/user_pinned   # 0

# 批量查看所有 CPU 状态
for cpu in /sys/devices/system/cpu/cpu[0-9]*/cfi/state; do
    echo "$(dirname $(dirname $cpu))/$(basename $(dirname $cpu)): $(cat $cpu)"
done
```

### 4.3 检查 netlink 接口

```bash
# 确认 generic netlink family 已注册
genl-ctrl-list 2>/dev/null | grep CFI
# 或者:
cat /proc/net/netlink | head

# 如果 genl-ctrl-list 不可用，用 python 检测:
python3 -c "
import socket, struct
# Check if CFI genl family is registered
s = socket.socket(socket.AF_NETLINK, socket.SOCK_DGRAM, 16)  # NETLINK_GENERIC
print('Netlink socket created successfully')
s.close()
"
```

### 4.4 运行时参数修改

```bash
# 通过 sysfs 调整参数 (无需重新加载模块)
echo 5 | sudo tee /sys/kernel/cfi/ce_threshold
echo 2 | sudo tee /sys/kernel/cfi/uce_threshold
echo 120 | sudo tee /sys/kernel/cfi/window_secs

# 锁定特定 CPU 不被自动隔离
echo 1 | sudo tee /sys/devices/system/cpu/cpu0/cfi/user_pinned

# 通过 module_param 也可以调整 (等效)
echo 5 | sudo tee /sys/module/cpu_fault_isolate/parameters/ce_threshold
```

### 4.5 卸载模块

```bash
sudo rmmod cpu_fault_isolate
dmesg | tail -3
# 预期:
# cpu_fault_isolate: unloading
# cpu_fault_isolate: unregistered x86 MCA/MCE handler
# cpu_fault_isolate: unloaded
```

---

## 5. 故障注入方法详解

本模块支持多种故障注入方式，从最安全（软件模拟）到最真实（硬件注入）：

### 5.1 方法一: debugfs 软件注入 (推荐首选)

**适用**: 所有环境（虚拟机、物理机、x86、ARM64）
**风险**: 无（纯软件模拟，不触发真实硬件错误）
**限制**: 不经过真实硬件错误路径，只测试模块内部逻辑

> debugfs 注入接口已实现（`kernel/core/cfi_debugfs.c`），同时支持
> CPU 域和内存域（见第 9 章）。

使用方式：

```bash
# 挂载 debugfs (通常已自动挂载)
mount -t debugfs none /sys/kernel/debug 2>/dev/null

# 注入单个 CE 错误到 CPU 4 的 L2 cache
echo "cpu=4 type=cache_l2 severity=ce" | sudo tee /sys/kernel/debug/cfi/inject

# 注入 UCE 错误到 CPU 4
echo "cpu=4 type=cache_l2 severity=ucr" | sudo tee /sys/kernel/debug/cfi/inject

# 批量注入（触发阈值）
for i in $(seq 1 5); do
    echo "cpu=4 type=cache_l2 severity=ce" | sudo tee /sys/kernel/debug/cfi/inject
    sleep 0.1
done

# 支持的 type 值:
#   cache_l1d, cache_l1i, cache_l2, cache_l3,
#   tlb, bus, internal, generic_core
#
# 支持的 severity 值:
#   ce    - Corrected Error (纠正错误)
#   ucr   - Uncorrected Recoverable (不可纠正但可恢复)
#   ucf   - Uncorrected Fatal (不可纠正致命)
```

### 5.2 方法二: ACPI EINJ (Error INJection)

**适用**: 物理机 + 支持 EINJ 的固件 (BIOS/UEFI)
**风险**: 中等（注入真实硬件错误，可能导致系统不稳定）
**限制**: 需要固件支持，不是所有服务器都启用了 EINJ

#### 5.2.1 检查 EINJ 支持

```bash
# 检查内核配置
grep CONFIG_ACPI_APEI_EINJ /boot/config-$(uname -r)
# 需要: CONFIG_ACPI_APEI_EINJ=y 或 CONFIG_ACPI_APEI_EINJ=m

# 如果是模块，加载它
sudo modprobe einj

# 检查 EINJ 接口是否可用
ls /sys/kernel/debug/apei/einj/
# 应该看到:
#   available_error_type
#   error_inject
#   error_type
#   flags
#   notrigger
#   param1   (physical address)
#   param2   (mask)
#   param3   (APIC ID for processor errors)
#   param4   (PCIe segment/bus/dev/func)

# 查看可注入的错误类型
cat /sys/kernel/debug/apei/einj/available_error_type
# 典型输出:
# 0x00000001  Processor Correctable
# 0x00000002  Processor Uncorrectable non-fatal
# 0x00000004  Processor Uncorrectable fatal
# 0x00000008  Memory Correctable
# 0x00000010  Memory Uncorrectable non-fatal
# 0x00000020  Memory Uncorrectable fatal
# ...
```

#### 5.2.2 注入处理器纠正错误 (CE)

```bash
# 注入 Processor Correctable 错误
echo 0x00000001 | sudo tee /sys/kernel/debug/apei/einj/error_type

# 可选：指定目标 APIC ID (定向到特定 CPU)
# 获取 CPU 的 APIC ID:
APIC_ID=$(rdmsr -p 4 0x802 2>/dev/null || \
          grep "apicid" /proc/cpuinfo | sed -n '5p' | awk '{print $3}')
echo "0x${APIC_ID}" | sudo tee /sys/kernel/debug/apei/einj/param3
echo 0x4 | sudo tee /sys/kernel/debug/apei/einj/flags  # bit 2 = use param3

# 触发注入
echo 1 | sudo tee /sys/kernel/debug/apei/einj/error_inject

# 检查结果
dmesg | tail -20
cat /sys/devices/system/cpu/cpu4/cfi/state
cat /sys/devices/system/cpu/cpu4/cfi/ce_count
```

#### 5.2.3 注入处理器不可纠正错误 (UCE)

```bash
# ⚠️ 警告: UCE 可能导致系统 panic，在测试环境中操作！
# 建议先保存工作，或在一次性测试 VM 中操作

# 注入 Processor Uncorrectable non-fatal
echo 0x00000002 | sudo tee /sys/kernel/debug/apei/einj/error_type
echo 1 | sudo tee /sys/kernel/debug/apei/einj/error_inject

# 检查是否触发了隔离
dmesg | grep "cpu_fault_isolate"
cat /sys/devices/system/cpu/cpu*/cfi/state | grep -v online
```

#### 5.2.4 鲲鹏服务器 EINJ 注意事项

```bash
# 鲲鹏 920/930 BIOS 中需要启用 EINJ:
# BIOS Setup -> Advanced -> ACPI Settings -> APEI Support -> Enabled
# BIOS Setup -> Advanced -> ACPI Settings -> EINJ Support -> Enabled

# 鲲鹏特有: HiSilicon vendor-specific 错误可能需要通过
# BMC (iBMC/eSight) 工具注入，而非标准 EINJ

# 检查鲲鹏 EINJ 支持:
dmesg | grep -i "einj\|apei"
cat /sys/firmware/acpi/tables/EINJ 2>/dev/null | hexdump -C | head
```

### 5.3 方法三: mce-inject 工具 (仅 x86)

**适用**: x86_64 物理机或启用了 MCE 的虚拟机
**风险**: 低到中等（注入到 MCE 子系统，不是真实硬件错误）
**限制**: 仅 x86，需要 `CONFIG_X86_MCE_INJECT`

#### 5.3.1 安装和配置

```bash
# 安装 mce-inject
sudo dnf install -y mce-inject 2>/dev/null || \
    sudo yum install -y mcelog 2>/dev/null

# 如果包管理器中没有，从源码编译:
git clone https://git.kernel.org/pub/scm/utils/cpu/mce/mce-inject.git
cd mce-inject
make
sudo make install

# 确认内核支持
grep CONFIG_X86_MCE_INJECT /boot/config-$(uname -r)
# 如果是模块: sudo modprobe mce-inject

# 检查注入接口
ls /dev/mcelog 2>/dev/null || ls /sys/kernel/debug/mce-inject 2>/dev/null
```

#### 5.3.2 注入 L2 Cache CE (x86)

创建注入描述文件:

```bash
cat > /tmp/l2_cache_ce.mce << 'EOF'
# L2 Cache Corrected Error on CPU 4
# MCA error code 0x000E = L2 cache, compound error
CPU 4
BANK 1
STATUS Val CE MiscV 0x000E
ADDR 0x0
MISC 0x0
EOF

# 注入
sudo mce-inject /tmp/l2_cache_ce.mce

# 检查
dmesg | tail -10
cat /sys/devices/system/cpu/cpu4/cfi/state
cat /sys/devices/system/cpu/cpu4/cfi/ce_count
```

#### 5.3.3 注入 L1D Cache CE

```bash
cat > /tmp/l1d_cache_ce.mce << 'EOF'
# L1 Data Cache Corrected Error on CPU 4
# MCA error code 0x000D = L1 cache
CPU 4
BANK 0
STATUS Val CE 0x000D
EOF

sudo mce-inject /tmp/l1d_cache_ce.mce
```

#### 5.3.4 注入 L3 Cache UCE (默认仅记账，不隔离)

```bash
cat > /tmp/l3_cache_uce.mce << 'EOF'
# L3 Cache Uncorrected Error on CPU 4
# MCA error code 0x000F = L3/generic cache
# UC bit set = uncorrected
# 在 Intel Skylake-SP/Cascade Lake-SP 上 L3 错误来自 CHA bank (9+);
# bank 3 = MLC (L2) 与 L3 errcode 语义不一致, hw 路径会被拒。
CPU 4
BANK 9
STATUS Val UC EN MiscV AddrV 0x000F
ADDR 0xdeadbeef000
MISC 0x0
EOF

# L3 是 socket 共享缓存。默认 isolate_on_l3_uce=0 时, CFI 仅记账并
# 发 netlink, **不隔离 CPU** (隔离单核解决不了共享 LLC 的问题, 应由
# userspace daemon 做 hwpoison / socket-level drain)。
sudo mce-inject /tmp/l3_cache_uce.mce

# 要恢复早期"L3 UCE 一律隔离上报核"的行为:
echo 1 > /sys/kernel/cfi/isolate_on_l3_uce
sudo mce-inject /tmp/l3_cache_uce.mce
echo 0 > /sys/kernel/cfi/isolate_on_l3_uce
```

#### 5.3.5 批量注入触发阈值

```bash
# 创建 L2 CE 注入文件
cat > /tmp/l2_ce_batch.mce << 'EOF'
CPU 4
BANK 1
STATUS Val CE 0x000E
EOF

# 连续注入 (假设 ce_threshold=3)
for i in 1 2 3 4; do
    echo "--- Injection $i ---"
    sudo mce-inject /tmp/l2_ce_batch.mce
    sleep 0.5
    cat /sys/devices/system/cpu/cpu4/cfi/state
    cat /sys/devices/system/cpu/cpu4/cfi/ce_count
done
# 第3次注入后 state 应变为 "degraded"
```

### 5.4 方法四: 内核 MCE inject debugfs (仅 x86)

**适用**: x86_64，需要 `CONFIG_X86_MCE_INJECT=y/m`
**风险**: 低

```bash
# 加载 mce-inject 内核模块
sudo modprobe mce-inject

# 通过 debugfs 注入
# 格式: 写入 /sys/kernel/debug/mce-inject
# (具体格式取决于内核版本)

# 查看接口
ls /sys/kernel/debug/mce-inject/
```

### 5.5 方法五: rasdaemon + ras-mc-ctl (辅助)

```bash
# 安装 rasdaemon
sudo dnf install -y rasdaemon
sudo systemctl start rasdaemon

# 检查当前 RAS 错误记录
sudo ras-mc-ctl --summary
sudo ras-mc-ctl --errors

# rasdaemon 本身不做注入，但用于验证错误记录是否正确
# 与 CFI 模块共存，可以对比两者的错误记录
```

### 5.6 各注入方法对比

| 方法 | 架构 | 环境要求 | 真实度 | 安全性 | 覆盖范围 |
|------|------|----------|--------|--------|----------|
| **debugfs 软件注入** | x86 + ARM64 | 任意 Linux | ★☆☆ | ★★★ | 仅测试模块内部逻辑 |
| **ACPI EINJ** | x86 + ARM64 | 物理机 + 固件支持 | ★★★ | ★★☆ | 完整硬件错误路径 |
| **mce-inject** | 仅 x86 | 物理机/VM | ★★☆ | ★★★ | MCE 子系统路径 |
| **MCE debugfs** | 仅 x86 | CONFIG 支持 | ★★☆ | ★★★ | MCE 子系统路径 |
| **BMC/iBMC 工具** | 鲲鹏 | 物理机 + BMC | ★★★ | ★☆☆ | 鲲鹏全路径 |

---

## 6. 测试验证流程

### 6.1 Level 1: 模块基础功能测试 (虚拟机即可)

这一层测试不需要真实硬件错误，用 debugfs 软件注入即可。

#### 测试 1.1: 模块加载/卸载

```bash
#!/bin/bash
# test_load_unload.sh
set -e

echo "=== Test 1.1: Module load/unload ==="

# 加载
sudo insmod cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 \
    window_secs=60 defer_to_daemon=0
sleep 1

# 验证加载
lsmod | grep -q cpu_fault_isolate || { echo "FAIL: module not loaded"; exit 1; }
dmesg | tail -5 | grep -q "initialized successfully" || { echo "FAIL: init message"; exit 1; }

# 验证 sysfs
[[ -f /sys/kernel/cfi/version ]] || { echo "FAIL: global sysfs missing"; exit 1; }
[[ -f /sys/devices/system/cpu/cpu0/cfi/state ]] || { echo "FAIL: per-cpu sysfs missing"; exit 1; }

# 验证初始状态
STATE=$(cat /sys/devices/system/cpu/cpu0/cfi/state)
[[ "$STATE" == "online" ]] || { echo "FAIL: initial state=$STATE, expected=online"; exit 1; }

# 卸载
sudo rmmod cpu_fault_isolate
sleep 1

# 验证卸载
lsmod | grep -q cpu_fault_isolate && { echo "FAIL: module still loaded"; exit 1; }
[[ ! -f /sys/kernel/cfi/version ]] || { echo "FAIL: sysfs not cleaned up"; exit 1; }

echo "PASS"
```

#### 测试 1.2: sysfs 参数读写

```bash
#!/bin/bash
# test_sysfs_params.sh
set -e

echo "=== Test 1.2: Sysfs parameter read/write ==="

sudo insmod cpu_fault_isolate.ko ce_threshold=10 defer_to_daemon=0

# 读取默认值
[[ $(cat /sys/kernel/cfi/ce_threshold) == "10" ]] || { echo "FAIL: ce_threshold default"; exit 1; }

# 修改参数
echo 20 | sudo tee /sys/kernel/cfi/ce_threshold > /dev/null
[[ $(cat /sys/kernel/cfi/ce_threshold) == "20" ]] || { echo "FAIL: ce_threshold write"; exit 1; }

# user_pinned 测试
echo 1 | sudo tee /sys/devices/system/cpu/cpu0/cfi/user_pinned > /dev/null
[[ $(cat /sys/devices/system/cpu/cpu0/cfi/user_pinned) == "1" ]] || { echo "FAIL: user_pinned"; exit 1; }
echo 0 | sudo tee /sys/devices/system/cpu/cpu0/cfi/user_pinned > /dev/null

sudo rmmod cpu_fault_isolate
echo "PASS"
```

#### 测试 1.3: CE 错误计数和窗口 (需要 debugfs inject)

```bash
#!/bin/bash
# test_ce_counting.sh
echo "=== Test 1.3: CE error counting ==="

sudo insmod cpu_fault_isolate.ko ce_threshold=5 uce_threshold=1 \
    window_secs=60 defer_to_daemon=0

TARGET_CPU=1  # 不要用 CPU 0

# 注入 3 次 CE
for i in 1 2 3; do
    echo "cpu=$TARGET_CPU type=cache_l2 severity=ce" | \
        sudo tee /sys/kernel/debug/cfi/inject > /dev/null
done

# 验证计数
CE_COUNT=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/ce_count)
[[ "$CE_COUNT" == "3" ]] || { echo "FAIL: ce_count=$CE_COUNT, expected=3"; }

# 验证状态仍为 online (阈值是 5)
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/state)
[[ "$STATE" == "online" ]] || { echo "FAIL: state=$STATE, expected=online"; }

# 再注入 2 次达到阈值
for i in 1 2; do
    echo "cpu=$TARGET_CPU type=cache_l2 severity=ce" | \
        sudo tee /sys/kernel/debug/cfi/inject > /dev/null
done

# 验证状态变为 degraded
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/state)
[[ "$STATE" == "degraded" ]] || { echo "FAIL: state=$STATE, expected=degraded"; }

CE_COUNT=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/ce_count)
[[ "$CE_COUNT" == "5" ]] || { echo "FAIL: ce_count=$CE_COUNT, expected=5"; }

echo "CE counting: PASS"

sudo rmmod cpu_fault_isolate
```

#### 测试 1.4: UCE 触发自动隔离 (需要 debugfs inject)

```bash
#!/bin/bash
# test_uce_isolation.sh
echo "=== Test 1.4: UCE triggers CPU isolation ==="

# 确保有足够 CPU (至少 4 个)
NCPU=$(nproc)
if [[ $NCPU -lt 4 ]]; then
    echo "SKIP: need at least 4 CPUs, have $NCPU"
    exit 0
fi

TARGET_CPU=2  # 选一个非关键 CPU

sudo insmod cpu_fault_isolate.ko uce_threshold=1 defer_to_daemon=0

# 确认 CPU 在线
[[ $(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/online) == "1" ]] || \
    { echo "FAIL: CPU $TARGET_CPU not online"; exit 1; }

# 注入 UCE
echo "cpu=$TARGET_CPU type=cache_l2 severity=ucr" | \
    sudo tee /sys/kernel/debug/cfi/inject > /dev/null

sleep 2  # 等待 workqueue 处理

# 验证 CPU 被隔离
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/state)
ONLINE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/online)

echo "CPU $TARGET_CPU state=$STATE online=$ONLINE"

[[ "$STATE" == "isolated" ]] || { echo "FAIL: state=$STATE, expected=isolated"; }
[[ "$ONLINE" == "0" ]] || { echo "FAIL: cpu still online"; }

# 恢复: 手动 bring back online
echo 1 | sudo tee /sys/devices/system/cpu/cpu${TARGET_CPU}/online > /dev/null
sleep 1

echo "UCE isolation: PASS"

sudo rmmod cpu_fault_isolate
```

### 6.2 Level 2: MCE/RAS 通路测试 (物理机或支持 MCE 的 VM)

这一层测试经过真实的硬件错误报告路径。

#### 测试 2.1: x86 MCE 通路 (mce-inject)

```bash
#!/bin/bash
# test_mce_path.sh (仅 x86)
echo "=== Test 2.1: x86 MCE decode chain path ==="

sudo insmod cpu_fault_isolate.ko ce_threshold=3 defer_to_daemon=0

TARGET_CPU=1

# 创建 MCE 注入文件: L2 Cache CE
cat > /tmp/test_l2_ce.mce << EOF
CPU $TARGET_CPU
BANK 1
STATUS Val CE 0x000E
EOF

# 注入
sudo mce-inject /tmp/test_l2_ce.mce
sleep 1

# 验证模块收到了错误
dmesg | grep "cpu_fault_isolate.*cpu${TARGET_CPU}.*MCE"
CE_COUNT=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/ce_count)
echo "CE count after mce-inject: $CE_COUNT"
[[ "$CE_COUNT" -ge 1 ]] || { echo "FAIL: MCE not received by CFI module"; }

# 验证错误类型
TYPES=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/error_types)
echo "Error types: $TYPES"
# 0x04 = CFI_ERR_CACHE_L2

echo "MCE path: PASS"
sudo rmmod cpu_fault_isolate
rm /tmp/test_l2_ce.mce
```

#### 测试 2.2: x86 多种 Cache Level 分类

```bash
#!/bin/bash
# test_cache_classification.sh (仅 x86)
echo "=== Test 2.2: Cache error classification ==="

sudo insmod cpu_fault_isolate.ko ce_threshold=100 defer_to_daemon=0

# L1D cache error (0x000D, TT=01 data)
cat > /tmp/l1d.mce << 'EOF'
CPU 1
BANK 0
STATUS Val CE 0x0015
EOF

# L1I cache error (0x000D, TT=00 instr)  
cat > /tmp/l1i.mce << 'EOF'
CPU 1
BANK 0
STATUS Val CE 0x000D
EOF

# L2 cache error (0x000E)
cat > /tmp/l2.mce << 'EOF'
CPU 1
BANK 1
STATUS Val CE 0x000E
EOF

# L3 cache error (0x000F)
cat > /tmp/l3.mce << 'EOF'
CPU 1
BANK 3
STATUS Val CE 0x000F
EOF

for f in /tmp/l1d.mce /tmp/l1i.mce /tmp/l2.mce /tmp/l3.mce; do
    echo "Injecting: $f"
    sudo mce-inject "$f"
    sleep 0.5
done

# 检查 error_types 包含了所有 cache level
TYPES=$(cat /sys/devices/system/cpu/cpu1/cfi/error_types)
echo "Error types bitmask: $TYPES"
# 期望包含: L1D(0x01) + L1I(0x02) + L2(0x04) + L3(0x08) = 0xf

CE_TOTAL=$(cat /sys/devices/system/cpu/cpu1/cfi/ce_total)
echo "Total CE: $CE_TOTAL"

sudo rmmod cpu_fault_isolate
rm /tmp/l1d.mce /tmp/l1i.mce /tmp/l2.mce /tmp/l3.mce
```

### 6.3 Level 3: EINJ 硬件级测试 (物理机)

```bash
#!/bin/bash
# test_einj_processor.sh (物理机，需要 EINJ 支持)
echo "=== Test 3.1: EINJ Processor CE ==="

# 前置检查
if [[ ! -d /sys/kernel/debug/apei/einj ]]; then
    sudo modprobe einj 2>/dev/null
fi
if [[ ! -d /sys/kernel/debug/apei/einj ]]; then
    echo "SKIP: EINJ not available"
    exit 0
fi

sudo insmod cpu_fault_isolate.ko ce_threshold=5 defer_to_daemon=0

# 注入 Processor Correctable
echo 0x00000001 | sudo tee /sys/kernel/debug/apei/einj/error_type > /dev/null
echo 1 | sudo tee /sys/kernel/debug/apei/einj/error_inject > /dev/null
sleep 2

# 检查 dmesg 和 CFI 状态
dmesg | tail -20 | grep -E "mce|cpu_fault_isolate|GHES"

# 至少应该有一个 CPU 的 CE 计数增加了
TOTAL_CE=0
for cpu in /sys/devices/system/cpu/cpu[0-9]*/cfi/ce_count; do
    COUNT=$(cat $cpu)
    if [[ $COUNT -gt 0 ]]; then
        CPU_NUM=$(echo $cpu | grep -oP 'cpu\K[0-9]+')
        echo "CPU $CPU_NUM: CE=$COUNT"
        TOTAL_CE=$((TOTAL_CE + COUNT))
    fi
done

echo "Total CE across all CPUs: $TOTAL_CE"
[[ $TOTAL_CE -gt 0 ]] && echo "EINJ CE: PASS" || echo "EINJ CE: FAIL (no errors received)"

sudo rmmod cpu_fault_isolate
```

### 6.4 Level 4: CPU 隔离与恢复完整流程

```bash
#!/bin/bash
# test_full_isolation_recovery.sh
echo "=== Test 4.1: Full isolation and recovery cycle ==="

NCPU=$(nproc)
if [[ $NCPU -lt 4 ]]; then
    echo "SKIP: need at least 4 CPUs"
    exit 0
fi

TARGET_CPU=3

sudo insmod cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 \
    window_secs=60 defer_to_daemon=0

echo "--- Phase 1: CE accumulation -> DEGRADED ---"
for i in 1 2 3; do
    echo "cpu=$TARGET_CPU type=cache_l2 severity=ce" | \
        sudo tee /sys/kernel/debug/cfi/inject > /dev/null
done
sleep 1
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/state)
echo "After 3 CE: state=$STATE"
[[ "$STATE" == "degraded" ]] || echo "UNEXPECTED: expected degraded"

echo "--- Phase 2: UCE -> ISOLATING -> ISOLATED ---"
echo "cpu=$TARGET_CPU type=cache_l2 severity=ucr" | \
    sudo tee /sys/kernel/debug/cfi/inject > /dev/null
sleep 3
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/state)
ONLINE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/online)
echo "After UCE: state=$STATE online=$ONLINE"
[[ "$STATE" == "isolated" ]] || echo "UNEXPECTED: expected isolated"
[[ "$ONLINE" == "0" ]] || echo "UNEXPECTED: expected offline"

echo "--- Phase 3: Recovery -> ONLINE ---"
# 模拟运维恢复操作
echo 1 | sudo tee /sys/devices/system/cpu/cpu${TARGET_CPU}/online > /dev/null
sleep 2
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/state)
ONLINE=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/online)
echo "After recovery: state=$STATE online=$ONLINE"

echo "--- Phase 4: Verify counters persist ---"
CE_TOTAL=$(cat /sys/devices/system/cpu/cpu${TARGET_CPU}/cfi/ce_total)
echo "Lifetime CE total: $CE_TOTAL"

echo "=== Full cycle complete ==="
sudo rmmod cpu_fault_isolate
```

### 6.5 Level 5: 并发和边界条件

```bash
#!/bin/bash
# test_edge_cases.sh
echo "=== Test 5: Edge cases ==="

NCPU=$(nproc)
sudo insmod cpu_fault_isolate.ko ce_threshold=3 uce_threshold=1 \
    window_secs=60 defer_to_daemon=0

echo "--- Test 5.1: user_pinned blocks isolation ---"
TARGET=1
echo 1 | sudo tee /sys/devices/system/cpu/cpu${TARGET}/cfi/user_pinned > /dev/null
echo "cpu=$TARGET type=cache_l2 severity=ucr" | \
    sudo tee /sys/kernel/debug/cfi/inject > /dev/null
sleep 2
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET}/cfi/state)
ONLINE=$(cat /sys/devices/system/cpu/cpu${TARGET}/online)
echo "Pinned CPU after UCE: state=$STATE online=$ONLINE"
[[ "$ONLINE" == "1" ]] && echo "PASS: pinned CPU not isolated" || echo "FAIL"
echo 0 | sudo tee /sys/devices/system/cpu/cpu${TARGET}/cfi/user_pinned > /dev/null

echo "--- Test 5.2: Window expiry resets counters ---"
# 设置 window_secs=2 for quick test
echo 2 | sudo tee /sys/kernel/cfi/window_secs > /dev/null
TARGET=2
echo "cpu=$TARGET type=cache_l2 severity=ce" | \
    sudo tee /sys/kernel/debug/cfi/inject > /dev/null
CE1=$(cat /sys/devices/system/cpu/cpu${TARGET}/cfi/ce_count)
echo "CE count before window expiry: $CE1"
sleep 3
echo "cpu=$TARGET type=cache_l2 severity=ce" | \
    sudo tee /sys/kernel/debug/cfi/inject > /dev/null
CE2=$(cat /sys/devices/system/cpu/cpu${TARGET}/cfi/ce_count)
echo "CE count after window expiry: $CE2"
# CE2 应该是 1 (窗口重置后重新计数), 不是 2
[[ "$CE2" == "1" ]] && echo "PASS: window reset" || echo "CHECK: ce=$CE2"

echo "--- Test 5.3: auto_isolate=0 only notifies ---"
echo 0 | sudo tee /sys/kernel/cfi/auto_isolate > /dev/null
echo 60 | sudo tee /sys/kernel/cfi/window_secs > /dev/null
TARGET=2
echo "cpu=$TARGET type=cache_l2 severity=ucr" | \
    sudo tee /sys/kernel/debug/cfi/inject > /dev/null
sleep 2
STATE=$(cat /sys/devices/system/cpu/cpu${TARGET}/cfi/state)
ONLINE=$(cat /sys/devices/system/cpu/cpu${TARGET}/online)
echo "auto_isolate=0 after UCE: state=$STATE online=$ONLINE"
[[ "$ONLINE" == "1" ]] && echo "PASS" || echo "FAIL"

sudo rmmod cpu_fault_isolate
echo "=== Edge cases complete ==="
```

---

## 7. 测试矩阵

### 7.1 功能测试矩阵

| # | 测试项 | 注入方式 | x86 虚拟机 | x86 物理机 | ARM64 虚拟机 | ARM64 物理机 |
|---|--------|----------|:--:|:--:|:--:|:--:|
| 1.1 | 模块加载/卸载 | 无 | ✅ | ✅ | ✅ | ✅ |
| 1.2 | sysfs 参数读写 | 无 | ✅ | ✅ | ✅ | ✅ |
| 1.3 | CE 错误计数 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 1.4 | UCE 触发隔离 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 1.5 | 窗口过期重置 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 1.6 | user_pinned 阻止隔离 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 1.7 | auto_isolate=0 只通知 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 2.1 | MCE decode chain 接收 | mce-inject | ✅* | ✅ | - | - |
| 2.2 | Cache level 分类 | mce-inject | ✅* | ✅ | - | - |
| 2.3 | ARM RAS tracepoint 接收 | - | - | - | ✅** | ✅ |
| 3.1 | EINJ Processor CE | EINJ | ❌ | ✅ | ❌ | ✅ |
| 3.2 | EINJ Processor UCE | EINJ | ❌ | ✅ | ❌ | ✅ |
| 3.3 | 鲲鹏 L3/HHA 错误 | EINJ/BMC | - | - | ❌ | ✅ |
| 4.1 | 完整隔离恢复循环 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 5.1 | 并发多 CPU 隔离 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 5.2 | CPU0 保护 | debugfs | ✅ | ✅ | ✅ | ✅ |
| 5.3 | netlink 事件广播 | debugfs | ✅ | ✅ | ✅ | ✅ |

**图例**: ✅ 可测 | ✅* 需要 CONFIG_X86_MCE_INJECT | ✅** 需要 GHES 模拟 | ❌ 环境不支持

### 7.2 环境准备清单

```
┌─────────────────────────────────────────────────────────┐
│ 最小可行测试环境 (虚拟机，覆盖 1.x + 4.x 测试)          │
│                                                         │
│  openEuler 2403 SP3 VM (x86_64 或 aarch64)             │
│  - 4+ vCPU                                              │
│  - kernel-devel 已安装                                   │
│  - debugfs 软件注入 (cfi_debugfs.c)                     │
│  覆盖率: ~60%                                           │
├─────────────────────────────────────────────────────────┤
│ 标准测试环境 (物理机，覆盖 1.x + 2.x + 4.x + 5.x)       │
│                                                         │
│  openEuler 2403 SP3 物理机                               │
│  - x86_64: Intel/AMD 多核服务器                          │
│  - aarch64: 鲲鹏 920/930 服务器                          │
│  - mce-inject 已安装 (x86)                              │
│  - BIOS 启用 EINJ (如支持)                               │
│  覆盖率: ~85%                                           │
├─────────────────────────────────────────────────────────┤
│ 完整测试环境 (物理机 + KVM 虚拟机)                        │
│                                                         │
│  在标准测试环境基础上:                                    │
│  - libvirt + QEMU/KVM 已安装                             │
│  - 运行至少 2 个测试 VM                                  │
│  - VM 有 vCPU 绑定配置                                   │
│  覆盖率: ~100%                                          │
└─────────────────────────────────────────────────────────┘
```


---

## 8. 常见问题排查

### 8.1 编译问题

**Q: `make` 报错 "No such file or directory: /lib/modules/.../build"**
```bash
# kernel-devel 未安装或版本不匹配
sudo dnf install -y kernel-devel-$(uname -r)
```

**Q: `make` 报 undefined reference to `mce_register_decode_chain`**
```bash
# 1. 确认是在 x86_64 上编译
uname -m   # 应为 x86_64

# 2. 确认内核启用了 MCE
grep CONFIG_X86_MCE /boot/config-$(uname -r)

# 3. 确认符号已导出
grep mce_register /lib/modules/$(uname -r)/build/Module.symvers
```

**Q: ARM64 编译报 `ras_event.h` 找不到**
```bash
# 确认 RAS 相关头文件存在
find /lib/modules/$(uname -r)/build/include -name "ras_event.h"
# 应在 include/ras/ras_event.h

# 如果不存在，可能内核未启用 CONFIG_RAS
grep CONFIG_RAS /boot/config-$(uname -r)
```

### 8.2 模块加载问题

**Q: `insmod` 报 "Invalid module format"**
```bash
# 模块是为不同内核版本编译的
modinfo cpu_fault_isolate.ko | grep vermagic
uname -r
# 两者必须匹配。重新编译模块。
```

**Q: `insmod` 报 "Unknown symbol"**
```bash
# 依赖的内核符号未导出
dmesg | tail -5
# 查看哪个符号缺失，检查内核配置
```

### 8.3 注入问题

**Q: mce-inject 报 "Operation not permitted"**
```bash
# 需要 root 权限
sudo mce-inject xxx.mce

# 如果仍然失败，检查 MCE inject 内核支持
grep MCE_INJECT /boot/config-$(uname -r)
sudo modprobe mce-inject
```

**Q: EINJ 目录不存在**
```bash
# 1. 加载 einj 模块
sudo modprobe einj

# 2. 如果模块不存在
grep EINJ /boot/config-$(uname -r)
# CONFIG_ACPI_APEI_EINJ=m 或 =y

# 3. 如果固件不支持 EINJ
dmesg | grep -i einj
# "EINJ: ACPI disabled" => BIOS 中未启用
# 需要在 BIOS 中开启 APEI/EINJ
```

**Q: 注入后 CFI 模块没有收到错误**
```bash
# 1. 检查 dmesg 是否有 MCE/RAS 相关日志
dmesg | grep -iE "mce|mca|ghes|ras|apei"

# 2. 确认模块的 notifier/tracepoint 已注册
dmesg | grep cpu_fault_isolate

# 3. x86: 确认 MCE decode chain 注册
# 加载模块时应该看到 "registering x86 MCA/MCE decode chain handler"

# 4. ARM64: 确认 tracepoint 注册
# 加载模块时应该看到 "registering ARM64 RAS tracepoint probes"
```

### 8.4 隔离问题

**Q: UCE 注入后 CPU 没有被隔离**
```bash
# 1. 检查 auto_isolate 是否开启
cat /sys/kernel/cfi/auto_isolate  # 应为 1

# 2. 检查 user_pinned
cat /sys/devices/system/cpu/cpuX/cfi/user_pinned  # 应为 0

# 3. 检查 defer_to_daemon
cat /sys/module/cpu_fault_isolate/parameters/defer_to_daemon
# 如果为 1，模块在等 daemon ACK（超时后才会强制隔离）

# 4. 检查 dmesg
dmesg | grep "cpu_fault_isolate"
```

**Q: `remove_cpu()` 失败**
```bash
# 某些 CPU 无法 offline
# 1. CPU 0 在某些配置下不允许 offline
grep HOTPLUG_CPU0 /boot/config-$(uname -r)

# 2. 如果是最后一个在线 CPU 会失败
nproc  # 确认有多个 CPU

# 3. 有 IRQ 无法迁移
dmesg | grep "cpu_fault_isolate.*failed"
cat /proc/interrupts  # 检查绑定到目标 CPU 的中断
```

---

## 9. 内存故障域（MFI）测试

v0.2 起模块包含内存故障域（设计见 `memory-fault-isolation-design.md`）。

### 9.1 加载参数与 sysfs

```bash
sudo insmod cpu_fault_isolate.ko \
    mem_enable=1 mem_pre_isolate=1 \
    page_ce_threshold=3 mem_window_secs=300 \
    dimm_ce_threshold=100 dimm_uce_threshold=3 \
    mem_triage=0          # Phase-2 甄别默认关闭

ls /sys/kernel/cfi/mem/
# enable pre_isolate page_ce_threshold window_secs
# dimm_ce_threshold dimm_uce_threshold triage stats dimms

cat /sys/kernel/cfi/mem/stats
# ce_total / uce_async / uce_consumed / pages_watched /
# pages_pre_offlined / pages_offlined / pages_failed /
# triage_saved / triage_panic / soft_offline_available
```

加载日志关注两行：

```
cpu_fault_isolate: mem: soft_offline_page resolved, pre-isolation available
cpu_fault_isolate: mem: memory fault domain active (...)
# 若检测到 RAS CEC:
cpu_fault_isolate: mem: RAS CEC detected, pre-isolation downgraded to accounting-only
```

### 9.2 软件注入（VM 即可）

```bash
# 编译用户态工具
make -C test/tools          # cfimon (netlink 监视) + ownpage (取靶页)

# 终端 1: 监听事件
sudo test/tools/cfimon

# 终端 2: CE 阈值 -> 预隔离（soft offline，无进程被杀）
sudo test/inject/mem_inject.sh --type ce --count 3

# 异步 UCE -> memory_failure 页隔离
sudo test/inject/mem_inject.sh --type uce_srao

# 消费型 UCE（用户态语义，由内核 rmap 杀属主，helper 进程收 SIGBUS）
sudo test/inject/mem_inject.sh --type uce_srar

# 查询统计（netlink 通道验证）
sudo test/tools/cfimon --mem-status
```

预期事件序列（cfimon 输出）：

```
[MEM_ERROR] pfn=0x... type=CE state=watched ce=1 ...
[MEM_ERROR] pfn=0x... type=CE state=pre_isolating ce=3 ...
[MEM_PAGE_OFFLINED] pfn=0x... state=offlined flags=0x2(PRE_ISOLATE)
```

### 9.3 Phase-2 甄别测试（物理机/牺牲 VM）

⚠️ **panic 判决会真的 panic**，仅在可丢弃的测试机上执行。

```bash
echo 1 > /sys/kernel/cfi/mem/triage

# 用户页 + RIPV=1 -> RECOVER（杀属主 + 页隔离 + MEM_VM_KILLED 事件）
echo "domain=mem pfn=<ownpage的pfn> type=uce_srar kernel=1" > \
    /sys/kernel/debug/cfi/inject

# RIPV=0 -> 受控 panic（kdump 验证现场保留）
echo "domain=mem pfn=<pfn> type=uce_srar kernel=1 ripv=0" > inject
```

纯决策逻辑有主机侧单元测试，不需要内核：

```bash
make -C test/unit run       # 18 个用例：甄别判决表 / 预隔离门控 / 窗口
```

### 9.4 EINJ 内存错误硬注入（物理机）

```bash
# 0x00000008 Memory Correctable        -> CE 记账（mc_event 通路）
# 0x00000010 Memory Uncorrectable non-fatal -> SRAO 页隔离，不 panic
# 0x00000020 Memory Uncorrectable fatal     -> 甄别/受控 panic 路径
echo 0x00000008 > /sys/kernel/debug/apei/einj/error_type
# param1/param2 可指定物理地址/掩码，定向到指定 VM 的内存
echo 1 > /sys/kernel/debug/apei/einj/error_inject
cat /sys/kernel/cfi/mem/stats
cat /sys/kernel/cfi/mem/dimms
```

### 9.5 MFI 测试矩阵

| # | 测试项 | 注入方式 | VM | 物理机 | 状态 |
|---|--------|----------|:--:|:--:|------|
| M1 | mem sysfs 读写 | 无 | ✅ | ✅ | 待跑（代码就绪） |
| M2 | CE 计数 + 窗口重置 | debugfs | ✅ | ✅ | 待跑 |
| M3 | CE 阈值 -> soft offline | debugfs | ✅ | ✅ | 待跑 |
| M4 | uce_srao -> memory_failure | debugfs | ✅ | ✅ | 待跑 |
| M5 | netlink MEM_* 事件 (cfimon) | debugfs | ✅ | ✅ | 待跑 |
| M6 | 甄别判决表 | 主机单测 | ✅ | ✅ | **已通过 (18/18)** |
| M7 | madvise(MADV_HWPOISON) 通路 | madvise | ✅ | ✅ | 待跑 |
| M8 | CEC 共存降级 | 无 | - | ✅ | 待跑 |
| M9 | EINJ Memory CE/UCE | EINJ | ❌ | ✅ | 待跑 |
| M10 | DIMM 阈值 -> MIGRATE_ADVISED | EINJ | ❌ | ✅ | 待跑 |
| M11 | 甄别 RECOVER/panic 实机验证 | debugfs/EINJ | ⚠️ | ✅ | 待跑 |

> 本仓库当前开发环境（容器，无法 insmod）已完成：模块对 6.8 头文件
> 零警告编译、用户态工具编译与基础行为、策略单元测试全绿。
> M1-M5/M7 需要一台可加载模块的 VM，M8-M11 需要物理机。

---

## 附录 A: debugfs 注入接口（已实现）

`kernel/core/cfi_debugfs.c` 已提供 `/sys/kernel/debug/cfi/inject`：

- CPU 域: `cpu=N type=<type> severity=<sev>` → `cfi_report_error()`
- 内存域: `domain=mem pfn=0xN type=<ce|uce_srao|uce_srar> [kernel=] [ripv=] [pcc=]`
  → `mfi_report_mem_error()` / `mfi_triage_kernel_uce()`
- `cat /sys/kernel/debug/cfi/help` 查看完整说明

---

## 附录 B: 自动化测试脚本汇总

所有测试脚本位于 `test/` 目录：

```
test/
├── inject/
│   └── inject_common.sh        # 通用注入工具 (已有)
├── unit/
│   ├── test_load_unload.sh     # 1.1 模块加载卸载
│   ├── test_sysfs_params.sh    # 1.2 sysfs 读写
│   ├── test_ce_counting.sh     # 1.3 CE 计数
│   └── test_uce_isolation.sh   # 1.4 UCE 隔离
├── integration/
│   ├── test_mce_path.sh        # 2.1 MCE 通路 (x86)
│   ├── test_cache_classify.sh  # 2.2 Cache 分类 (x86)
│   ├── test_einj.sh            # 3.1 EINJ 测试
│   ├── test_full_cycle.sh      # 4.1 完整隔离恢复
│   └── test_edge_cases.sh      # 5.x 边界条件
└── run_all.sh                  # 运行所有测试
```
