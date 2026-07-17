# RK3588-machine-learning-zhou

RK3588 嵌入式 Linux 学习笔记仓库，涵盖 U-Boot 启动流程、QEMU 仿真实验、RK3588 开发板适配等内容。

## 仓库内容

### 📌 U-Boot 启动流程学习（ARMv8 / AArch64）

位于 [`u-boot-notes/`](u-boot-notes/) 目录，包含：

- **注释版源码**（`annotated-src/`）：5 个核心文件带详细中文注释
  - `start.S` — ARMv8 复位启动汇编（PIE 重定位 / EL 配置 / GIC 初始化）
  - `board_f.c` — 第一阶段初始化（relocation 前，内存预留）
  - `board_r.c` — 第二阶段初始化（relocation 后，外设驱动）
  - `bootm.c` — 启动 Linux 内核（FDT/ATAGS 参数构造 + EL 切换跳转）
  - `main.c` — 命令处理主循环（autoboot / cli_loop / bootstd）
- **启动流程全解文档**（`docs/`）：Mermaid 流程图 + 完整函数调用链 + `start.S` 寄存器级逐条解读
- **Obsidian 学习笔记**（`obsidian-notes/`）：U-Boot 学习笔记、动手实验指南、RK3588 适配要点、ROS2-Gazebo 仿真方案等 7 篇

详细说明见 [u-boot-notes/README.md](u-boot-notes/README.md)

## 学习环境

- **宿主机**：Windows 11
- **虚拟机**：Ubuntu 20.04（VMware）
- **交叉编译工具链**：ARM GNU Toolchain 13.2.Rel1
- **仿真平台**：QEMU 4.2.1（arm64 / `qemu_arm64_defconfig`）
- **U-Boot 版本**：主线 2026.07
- **目标芯片**：Rockchip RK3588

## 学习路线

1. 🔰 **入门**：阅读 [U-Boot 启动流程全解文档](u-boot-notes/docs/U-Boot启动流程全解文档.md)，建立整体认知
2. 📖 **源码**：按 `start.S → board_f.c → board_r.c → main.c → bootm.c` 顺序阅读注释版源码
3. 🔧 **实验**：参照 [U-Boot 动手实验操作说明](u-boot-notes/obsidian-notes/U-Boot动手实验操作说明.md) 在 QEMU 上实践
4. 🎯 **进阶**：阅读 [U-Boot 学习笔记](u-boot-notes/obsidian-notes/U-Boot学习笔记.md) 了解 RK3588 适配要点

## 相关资源

- [U-Boot 官方仓库](https://github.com/u-boot/u-boot)
- [Rockchip Linux U-Boot](https://github.com/rockchip-linux/u-boot)
- [Rockchip Linux Kernel](https://github.com/rockchip-linux/kernel)
- [Rockchip rkbin（预编译固件）](https://github.com/rockchip-linux/rkbin)

## License

MIT
