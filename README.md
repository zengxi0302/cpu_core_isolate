# cpu_core_isolate

面向云宿主机的硬件故障自隔离内核模块（`cpu_fault_isolate.ko`），目标：
单个器件故障不再整机宕机，损失收敛到单核/单页/单 VM。

包含两个故障域：

- **CFI（CPU 域）**：Cache UCE / 软硬锁 → panic 抑制 + 故障核 offline +
  netlink 通知上层迁移 VM。设计见 `docs/design-document.md`。
  各类 UCE 的内核默认行为与 CFI 加载收益对照见
  `docs/uce-coverage-matrix.md`。
- **MFI（内存域）**：页级 CE 预隔离（soft offline）、异步 UCE 页隔离、
  DIMM 介质记账、内核态 UCE 落点甄别（Phase 2，灰度开关）。
  设计见 `docs/memory-fault-isolation-design.md`。

## 目录结构

```
kernel/          内核模块源码（core/ 公共域逻辑，arch/ 架构后端）
config/          cfid 守护进程配置与 systemd 单元
docs/            设计文档、编译与测试验证指南、汇报材料
test/inject/     故障注入脚本（debugfs / mce-inject / EINJ）
test/tools/      cfimon（netlink 监视器）、ownpage（注入靶页辅助）
test/unit/       主机侧策略单元测试（无需内核）
rpm/             RPM 打包
```

## 快速开始

```bash
# 编译内核模块（需要 kernel-devel）
make -C kernel

# 加载（测试用低阈值）
sudo insmod kernel/cpu_fault_isolate.ko ce_threshold=3 page_ce_threshold=3

# 用户态工具与单元测试
make -C test/tools
make -C test/unit run

# 软件注入冒烟
sudo test/inject/inject_common.sh --cpu 2 --type ce --level l2 --count 3
sudo test/inject/mem_inject.sh --type ce --count 3
```

完整的环境搭建、注入方法与测试矩阵见 `docs/build-test-guide.md`。

## 兼容性

openEuler 2403 LTS / HCE3 / Linux 6.6+（6.8 头文件零警告编译验证），
x86_64（Intel/AMD）与 aarch64（鲲鹏 920/930）。
