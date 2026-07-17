# U-Boot 学习笔记仓库

本仓库记录 U-Boot（ARMv8 / AArch64）启动流程的学习过程，包含注释版源码、启动流程全解文档及 Obsidian 学习笔记。

## 目录结构

```
u-boot-notes/
├── README.md                          ← 本文件
├── docs/
│   └── U-Boot启动流程全解文档.md       ← 启动流程图 + 函数调用链 + start.S 寄存器级逐条解读
├── annotated-src/                     ← 带详细中文注释的 U-Boot 源码
│   ├── start.S                        ← ARMv8 复位启动汇编（PIE 重定位 / EL 配置 / GIC 初始化）
│   ├── board_f.c                      ← 第一阶段初始化（relocation 前，内存预留）
│   ├── board_r.c                      ← 第二阶段初始化（relocation 后，外设驱动）
│   ├── bootm.c                        ← ARM/ARM64 启动 Linux 内核（FDT/ATAGS 参数构造 + EL 切换跳转）
│   └── main.c                         ← 命令处理主循环（autoboot / cli_loop / bootstd）
└── obsidian-notes/                    ← Obsidian 学习笔记
    ├── U-Boot学习笔记.md              ← 完整学习笔记（环境搭建 / QEMU 编译 / 踩坑 / 源码修改 / RK3588 适配）
    ├── U-Boot动手实验操作说明.md       ← 四层实验指南（配置 → 源码 → 启动流程 → 源码阅读）
    ├── RK3588-LinuxUBoot开源项目学习指南-20260710.md
    ├── RK3588-仿真模拟学习方案-20260710.md
    ├── RK3588-嵌入式Linux学习路径与机器狗部署答疑-20260710.md
    ├── RK3588-自制开发板硬件设计指南-20260710.md
    └── ROS2-Gazebo仿真环境学习笔记.md
```

## 启动流程概览

```
上电/复位
  └→ start.S: _start → reset → save_boot_params → PIE 重定位 → EL 配置 → apply_core_errata → lowlevel_init(GIC) → master_cpu → _main
      └→ board_f.c: board_init_f() → initcall_run_f() → 早期初始化 + DRAM 探测 + 内存预留 + relocation 偏移计算
          └→ board_r.c: board_init_r() → initcall_run_r() → Cache/DM/环境/存储/控制台初始化 → run_main_loop()
              └→ main.c: main_loop() → autoboot 倒计时 → bootcmd 或 cli_loop
                  └→ bootm.c: do_bootm_linux() → boot_prep_linux(FDT/ATAGS) → boot_jump_linux(EL 切换 → 内核入口)
```

## 源码版本

- U-Boot 主线版本：2026.07
- 目标架构：AArch64 (ARMv8)
- 仿真平台：QEMU arm64 (qemu_arm64_defconfig)

## 学习路线

1. **入门**：阅读 `docs/U-Boot启动流程全解文档.md`，理解整体启动流程
2. **源码**：按 `start.S → board_f.c → board_r.c → main.c → bootm.c` 顺序阅读注释版源码
3. **实验**：参照 `obsidian-notes/U-Boot动手实验操作说明.md` 在 QEMU 上动手实践
4. **进阶**：阅读 `obsidian-notes/U-Boot学习笔记.md` 了解 RK3588 适配要点

## 相关资源

- [U-Boot 官方仓库](https://github.com/u-boot/u-boot)
- [Rockchip Linux U-Boot](https://github.com/rockchip-linux/u-boot)
- [Rockchip Linux Kernel](https://github.com/rockchip-linux/kernel)
- [Rockchip rkbin](https://github.com/rockchip-linux/rkbin)

---

*Generated: 2026-07-17*
