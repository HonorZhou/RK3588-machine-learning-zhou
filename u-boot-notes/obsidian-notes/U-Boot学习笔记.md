---
title: U-Boot 学习笔记
author: QClaw
date: 2026-07-13
type: 技术
tags: [U-Boot, QEMU, ARM64, RK3588, 交叉编译, 嵌入式Linux, Bootloader, menuconfig, BOOTDELAY, SYS_PROMPT]
source: conversation
---

# U-Boot 学习笔记

> 持续更新的 U-Boot 学习笔记，涵盖环境搭建、QEMU 仿真、源码修改、RK3588 板级适配等内容。

## 目录

- [[#1. 环境搭建|1. 环境搭建]]
- [[#2. QEMU 编译运行|2. QEMU 编译运行]]
- [[#3. 踩坑记录|3. 踩坑记录]]
- [[#4. U-Boot 命令行操作|4. U-Boot 命令行操作]]
- [[#5. 深入理解：改源码实验|5. 深入理解：改源码实验]]
- [[#6. menuconfig 修改配置参数|6. menuconfig 修改配置参数]]
- [[#7. QEMU 仿真能做什么不能做什么|7. QEMU 仿真能做什么不能做什么]]
- [[#8. RK3588 自制开发板 U-Boot 适配指南|8. RK3588 自制开发板 U-Boot 适配指南]]
- [[#9. 源码阅读路线|9. 源码阅读路线]]
- [[#10. 完整实战流程记录|10. 完整实战流程记录]]
- [[#11. 相关笔记|11. 相关笔记]]
- [[#12. 新版 U-Boot API 变化踩坑|12. 新版 U-Boot API 变化踩坑]]
- [[#13. 在 U-Boot 中运行程序|13. 在 U-Boot 中运行程序]]
- [[#14. Rockchip SDK U-Boot 编译实战（RK3588 真板）|14. Rockchip SDK U-Boot 编译实战（RK3588 真板）]]

---

## 1. 环境搭建

### 1.1 系统环境

- Ubuntu 20.04（虚拟机）
- QEMU 4.2.1
- U-Boot 主线源码

### 1.2 安装依赖

```bash
sudo apt install bison flex libgnutls28-dev
```

### 1.3 安装交叉编译器（GCC 10+）

Ubuntu 20.04 自带 gcc-9 不满足要求，需通过 PPA 安装 gcc-10：

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install gcc-10-aarch64-linux-gnu
```

> **备选**：下载 ARM 官方预编译工具链（GCC 13.2）
> ```bash
> wget https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz
> tar xf arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz
> export PATH=~/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH
> ```

---

## 2. QEMU 编译运行

### 2.1 完整流程

```bash
# 1. 配置（选择 QEMU ARM64 虚拟平台）
make qemu_arm64_defconfig

# 2. 编译
make CROSS_COMPILE=aarch64-linux-gnu-gcc-10- -j$(nproc)

# 3. 运行
qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -bios u-boot.bin 2>&1 | tee uboot.log
```

### 2.2 三步流程说明

| 步骤  | 命令                                         | 作用                           |
| --- | ------------------------------------------ | ---------------------------- |
| 配置  | `make qemu_arm64_defconfig`                | 生成 `.config`，选定 QEMU virt 平台 |
| 编译  | `make CROSS_COMPILE=... -j$(nproc)`        | 交叉编译生成 `u-boot.bin`          |
| 运行  | `qemu-system-aarch64 ... -bios u-boot.bin` | 在 QEMU 中启动 U-Boot            |

### 2.3 退出 QEMU

`Ctrl+A` 松开再按 `X`

---

## 3. 踩坑记录

### 坑1：缺少 bison

**报错**：
```
/bin/sh: 1: bison: not found
```

**解决**：`sudo apt install bison flex`

### 坑2：GCC 版本太低（< 10）

**报错**：
```
*** Your GCC is older than 10.0 and is not supported
```

**解决**：通过 PPA 安装 gcc-10（见上方 1.3 节）

### 坑3：缺少 gnutls 开发头文件

**报错**：
```
tools/mkeficapsule.c:20:10: fatal error: gnutls/gnutls.h: 没有那个文件或目录
```

**解决**：`sudo apt install libgnutls28-dev`

### 坑4：QEMU 启动后光标闪无输出

**现象**：`-nographic` 启动后只有光标闪，无 U-Boot 输出。

**原因**：
1. 虚拟机里 QEMU 启动慢，需等 30-60 秒
2. QEMU 4.2.1 的 `-nographic` 和 `-serial stdio` 冲突
3. 虚拟机终端可能没正确处理串口输出

**解决**：
```bash
qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -bios u-boot.bin 2>&1 | tee uboot.log
```

### 坑5：apt 源没有新版交叉编译器

Ubuntu 20.04 默认源最高 gcc-9，需 `add-apt-repository ppa:ubuntu-toolchain-r/test` 后才能装 gcc-10+。

---

## 4. U-Boot 命令行操作

启动后看到 `=>` 提示符：

```bash
help                        # 查看所有命令
printenv                    # 查看环境变量
bdinfo                      # 查看板子信息（CPU、内存等）
version                     # 查看 U-Boot 版本
setenv bootdelay 10         # 设置环境变量
saveenv                     # 保存环境变量
md.b 0x40000000 0x100       # 查看内存
reset                       # 重启
```

---

## 5. 深入理解：改源码实验

### 5.1 第一层：改配置（不改代码）

```bash
make menuconfig
```

可改项：
- `CONFIG_BOOTDELAY` — 启动倒计时秒数
- `CONFIG_SYS_PROMPT` — 把 `=>` 改成自定义提示符
- 禁用某个命令（如 `CONFIG_CMD_BOOTM=n`）

### 5.2 第二层：修改启动 banner

> ⚠️ 旧版 U-Boot（2019 前）在 `common/board_f.c` 里有 `display_banner()` 函数，新版（2026.07）已移除，需用以下方法定位。

#### 定位 banner 打印位置

```bash
# 第一步：搜 version_string（banner 打印的就是这个变量）
grep -rn "version_string" --include="*.c" --include="*.h"

# 关键线索：
# common/version.c:14  → version_string 的定义
# lib/display_options.c:24 → 打印 version_string 的地方
```

#### version_string 的构造链（`common/version.c`）

```c
// common/version.c
#define U_BOOT_VERSION_STRING U_BOOT_VERSION " (" U_BOOT_DATE " - " \
    U_BOOT_TIME " " U_BOOT_TZ ")" CONFIG_IDENT_STRING

const char version_string[] = U_BOOT_VERSION_STRING;
```

各宏来源：

| 宏 | 值示例 | 来源 |
|---|---|---|
| `U_BOOT_VERSION` | `U-Boot 2026.07-00730-g6741b0dfb41d` | 编译时自动生成 |
| `U_BOOT_DATE` | `Jul 14 2026` | 编译时间 |
| `U_BOOT_TIME` | `17:44:01` | 编译时间 |
| `U_BOOT_TZ` | `+0800` | 时区 |
| `CONFIG_IDENT_STRING` | ``（默认空） | menuconfig 可设 |

拼出来：`U-Boot 2026.07-00730-g6741b0dfb41d (Jul 14 2026 - 17:44:01 +0800)`

#### 修改方式一：改版本文字（`common/version.c`）

```c
// 在 version_string 后面追加自定义文字
const char version_string[] = U_BOOT_VERSION_STRING " - My Custom Board";
```

#### 修改方式二：改 banner 显示逻辑（`lib/display_options.c`，更灵活）

```c
// lib/display_options.c → display_options_get_banner_priv() 函数
char *display_options_get_banner_priv(bool newlines, const char *build_tag,
                                      char *buf, int size)
{
    int len;

    len = snprintf(buf, size, "%s%s", newlines ? "\n\n" : "",
                   version_string);
    /* === 自定义 banner === */
    if (len < size - 50)
        len += snprintf(buf + len, size - len,
                        "\n=== Hello, I modified U-Boot! ===");
    /* === 自定义 banner 结束 === */

    if (build_tag && len < size)
        len += snprintf(buf + len, size - len, ", Build: %s",
                        build_tag);
    // ... 后续代码不变
    return buf;
}
```

#### 编译验证

```bash
make CROSS_COMPILE=aarch64-none-linux-gnu- -j$(nproc)
qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -bios u-boot.bin 2>&1 | tee uboot.log
```

启动效果：
```
U-Boot 2026.07-00730-g6741b0dfb41d (Jul 14 2026 - 17:44:01 +0800)

=== Hello, I modified U-Boot! ===

DRAM: 128 MiB
...
```

#### 版本差异速查

| U-Boot 版本 | banner 位置 | 函数名 |
|---|---|---|
| 旧版（~2019） | `common/board_f.c` | `display_banner()` |
| 新版（2021+） | `common/board_r.c` | `initr_banner()` 或类似 |
| 2026.07（当前） | `lib/display_options.c` | `display_options_get_banner_priv()` |

### 5.3 第三层：添加自定义命令

1. 新建 `cmd/hello.c`：

```c
#include <common.h>
#include <command.h>

static int do_hello(struct cmd_tbl *cmdtp, int flag, int argc,
                    char *const argv[])
{
    printf("Hello from my custom command!\n");
    if (argc > 1) {
        printf("You said: %s\n", argv[1]);
    }
    return 0;
}

U_BOOT_CMD(
    hello, CONFIG_SYS_MAXARGS, 1, do_hello,
    "print hello message",
    "hello [name] - print a greeting message"
);
```

2. 在 `cmd/Makefile` 添加：`obj-$(CONFIG_CMD_HELLO) += hello.o`
3. 在 `cmd/Kconfig` 添加配置项
4. `make menuconfig` 启用 `CMD_HELLO`
5. 编译运行，输入 `hello` 或 `hello world`

### 5.4 第四层：跟踪启动流程

在关键函数加 printf 追踪启动顺序：

```c
// common/board_f.c
void board_init_f(ulong boot_flags) {
    printf("[TRACE] board_init_f entered\n");
    // ...
}

// common/board_r.c
void board_init_r(gd_t *new_gd, ulong dest_addr) {
    printf("[TRACE] board_init_r entered\n");
    // ...
}
```

启动流程概览：
```
arch/arm/cpu/armv8/start.S  → 汇编入口，初始化 CPU
  → board_init_f()           → 早期初始化（内存、时钟）
  → relocate_code()          → 重定位到 RAM 高地址
  → board_init_r()           → 后期初始化（设备、环境变量）
    → run_main_loop()        → 进入主循环
```

### 5.5 建议的实验路线

| 天数 | 实验 | 目的 |
|------|------|------|
| 第1天 | 改配置（menuconfig） | 理解配置系统 |
| 第2天 | 改 banner + 加 hello 命令 | 理解命令注册机制 |
| 第3天 | 加启动流程打印 | 理解 init sequence |
| 第4天 | 读 main.c + board_r.c | 理解主循环 |
| 第5天 | 读 bootm.c | 理解如何跳转到 Linux 内核 |

---

## 6. menuconfig 修改配置参数

### 6.1 三种修改方式

#### 方式一：menuconfig 菜单界面（推荐）

```bash
make menuconfig
```

菜单操作：

| 按键 | 功能 |
|------|------|
| ↑↓ | 上下移动 |
| Enter | 进入子菜单 / 确认 |
| Esc×2 | 返回上级 |
| / | 搜索（输入 BOOTDELAY 或 SYS_PROMPT 直接定位） |
| Y | 选中 |
| N | 取消 |

**修改 BOOTDELAY**：
- 按 `/` 搜索 `BOOTDELAY`
- 跳到 → `Boot Options` --->
- 进入 → `bootdelay in seconds before autobooting (CONFIG_BOOTDELAY)`
- 改成想要的秒数（0 = 不等待直接启动，5 = 等 5 秒）

**修改 SYS_PROMPT**：
- 按 `/` 搜索 `SYS_PROMPT`
- 跳到 → `Command line interface` --->
- 进入 → `Shell Prompt (CONFIG_SYS_PROMPT)`
- 改成想要的提示符，如 `myboard=> `

改完后 `Esc×2` 返回主菜单，再 `Esc×2` 保存退出选 Yes。

#### 方式二：直接编辑 .config 文件（快速测试）

```bash
cp .config .config.bak
# 编辑 .config，找到这两行修改：
# CONFIG_BOOTDELAY=2
# CONFIG_SYS_PROMPT="> "
make  # 重新编译
```

> ⚠️ 不推荐：`make menuconfig` 或 `make xxx_defconfig` 会覆盖你的修改

#### 方式三：在 defconfig 文件中修改（最持久，可提交 git）

```bash
# 编辑对应的 defconfig
vim configs/qemu_arm64_defconfig    # QEMU 仿真
# 或
vim configs/rock5b-rk3588_defconfig # RK3588 真机

# 添加或修改：
CONFIG_BOOTDELAY=5
CONFIG_SYS_PROMPT="myboard=> "

# 重新加载配置并编译
make qemu_arm64_defconfig
make CROSS_COMPILE=aarch64-none-linux-gnu- -j$(nproc)
```

### 6.2 三种方式对比

| 方式 | 优点 | 缺点 | 适用场景 |
|------|------|------|----------|
| **menuconfig** | 可视化、不会漏依赖 | 需要终端环境 | 日常调试，推荐 |
| **改 .config** | 快速 | 容易被覆盖 | 临时测试 |
| **改 defconfig** | 持久、可提交 git | 需要知道放哪个文件 | 正式项目、团队协作 |

### 6.3 验证修改生效

编译完成后用 QEMU 启动验证：
```bash
qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -bios u-boot.bin 2>&1 | tee uboot.log
```

- 启动倒计时变成你设的秒数
- 提示符变成你设的字符串

---

## 7. QEMU 仿真能做什么不能做什么

### 核心理解

```
QEMU 模拟的是 "行为" 不是 "硬件"

真实硬件：  写寄存器 → 硬件电路响应 → 物理效果
QEMU：     写寄存器 → QEMU 代码判断 → 模拟输出（可能忽略）
```

### ✅ 改了有效果的

| 改什么 | 效果 | 验证方式 |
|--------|------|----------|
| 启动 banner / printf | 启动时看到文字 | 看输出 |
| 环境变量 | 倒计时/启动命令变化 | `printenv` |
| 自定义命令 | `help` 多出命令 | 输入试试 |
| 内存配置 | `bdinfo` 显示变化 | `bdinfo` |
| 设备树（DTB） | 识别的设备变化 | `fdt print` |

### ⚠️ 有效果但受限的

| 改什么 | 限制 |
|--------|------|
| 串口配置 | QEMU `virt` 只模拟 PL011 UART |
| CPU 频率 | QEMU 不模拟真实时钟 |
| 网卡驱动 | QEMU 用 virtio-net，改真实网卡没反应 |

### ❌ 完全没效果的

| 改什么 | 原因 |
|--------|------|
| **电源管理（PMIC）** | QEMU 没模拟 PMIC 芯片 |
| 温度传感器 | QEMU 不模拟温控 |
| GPIO 实际电平 | QEMU GPIO 是虚拟的 |
| 时钟树（PLL） | QEMU 不模拟真实时钟硬件 |
| DDR 时序 | QEMU 内存直接分配，无真实 DDR 控制器 |
| 看门狗 | QEMU 有简单模拟但不真实 |

### QEMU 里值得做的实验

```bash
# 1. 改 RAM 大小，看 bdinfo 变化
# 2. 改启动命令
=> setenv bootcmd "echo my boot command; help"
=> saveenv
=> reset
# 3. 修改设备树，添加/删除节点
# 4. 改串口输出内容（drivers/serial/serial_pl01x.c）
```

---

## 8. RK3588 自制开发板 U-Boot 适配指南

### 7.1 启动配置（BOOT_SEL）

```
BOOT_SEL[1:0] = 00 → USB 启动
BOOT_SEL[1:0] = 01 → SPI NOR Flash
BOOT_SEL[1:0] = 10 → eMMC
BOOT_SEL[1:0] = 11 → SD Card
```

硬件上接好上下拉电阻，U-Boot 里对应配置启动介质。

### 7.2 DDR 配置（最关键）

需要修改的文件：
```
arch/arm/mach-rockchip/rk3588/
  ├── sdram.c          ← DDR 初始化
  ├── sdram_params.c   ← DDR 时序参数（重点）
  └── ddr_cfg.c        ← DDR 配置选择
```

需要根据 DDR 颗粒确定：
- 容量（4GB/8GB/16GB/32GB）
- 类型（LPDDR4X/LPDDR5/DDR4）
- 厂商参数（时序、电压、频率）
- 位宽（32bit/64bit）
- 是否多颗并联

> ⚠️ 参数错了 → 板子直接启动不了，连 U-Boot 都跑不到

### 7.3 电源管理（PMIC）

RK3588 供电需求：
| 供电 | 电压 | 说明 |
|------|------|------|
| VDD_CORE | 0.8-1.1V | CPU 核心，动态调压 |
| VDD_LOGIC | 0.9V | 逻辑供电 |
| VDD_DDR | 1.1V | DDR 供电 |
| VCCIO | 1.8V/3.3V | IO 供电 |

常见 PMIC 方案：
- RK806-1（瑞芯微自家，最常用）
- 外挂多路 DC-DC（TPS 系列）

U-Boot 需要：I2C 驱动 + PMIC 驱动 + 上电时序代码

### 7.4 时钟配置

```
24MHz 晶振 → PLL → 各模块时钟
```

需要配置：CPU 频率、DDR 频率、外设时钟
驱动文件：`drivers/clk/rockchip/clk_rk3588.c`

### 7.5 板级适配文件

新建自己的板子：
```
board/rockchip/my_rk3588_board/
  ├── Makefile
  ├── my_rk3588_board.c    ← 板级初始化代码
  └── sys_otp.c            ← OTP（如果有）
```

关键初始化函数：
```c
int board_init(void)           // 通用初始化
int board_late_init(void)      // 后期初始化
int dram_init(void)            // DDR 初始化
int board_early_init_f(void)   // 早期初始化
```

### 7.6 设备树

新建 `arch/arm/dts/rk3588-my-board.dts`，描述：
- CPU 类型和频率
- DDR 配置
- UART（调试串口）
- I2C（PMIC 接口）
- eMMC / SD 卡
- 网卡、USB、PCIe
- GPIO 按键/LED
- 启动介质配置

在 `arch/arm/dts/Makefile` 注册：
```makefile
dtb-$(CONFIG_TARGET_MY_RK3588_BOARD) += rk3588-my-board.dtb
```

### 7.7 Kconfig 配置

```
config TARGET_MY_RK3588_BOARD
    bool "My RK3588 Board"
    select ROCKCHIP_RK3588
    select SUPPORT_SPL
    help
      My custom RK3588 board.
```

### 7.8 驱动适配优先级

| 优先级 | 驱动 | 说明 |
|--------|------|------|
| 🔴 第一 | DDR 驱动 + 时序参数 | 没它启动不了 |
| 🔴 第一 | PMIC 驱动 + 上电时序 | 没它启动不了 |
| 🔴 第一 | 时钟驱动 | 没它启动不了 |
| 🔴 第一 | 串口驱动 | 调试用 |
| 🟡 第二 | eMMC / SD 卡 | 基本功能 |
| 🟡 第二 | 网卡驱动 | 基本功能 |
| 🟡 第二 | USB 驱动 | 基本功能 |
| 🟢 第三 | PCIe | 外设 |
| 🟢 第三 | 显示（HDMI/MIPI） | 外设 |
| 🟢 第三 | GPIO / LED / 按键 | 外设 |

### 7.9 完整适配流程

```
1. 硬件设计 → 确定 DDR/PMIC/启动介质/调试串口
2. SPL 适配 → DDR 初始化 + PMIC 上电时序 + 基本时钟
3. U-Boot 主阶段 → 板级初始化 + 设备树 + 外设驱动
4. 验证 → 串口日志 → U-Boot 命令行 → 读 eMMC/SD → 加载内核
```

### 7.10 给初次设计 RK3588 板子的建议

1. 先买现成开发板（如 Orange Pi 5 Plus）读完 U-Boot 源码
2. 在 QEMU 上做代码实验（改命令、改打印）
3. 在真板上验证 DDR/PMIC/时钟配置
4. 再画自己的板子，大部分代码可复用
5. 自制板 vs 现成板最大区别：DDR 走线、PMIC 选型、电源分配

---

## 9. 源码阅读路线

按从易到难顺序：

| 顺序 | 文件 | 作用 |
|------|------|------|
| 1 | `common/main.c` | 主循环，读命令行 |
| 2 | `common/board_f.c` | 早期初始化 |
| 3 | `common/board_r.c` | 后期初始化 |
| 4 | `cmd/` 目录 | 各种命令实现 |
| 5 | `arch/arm/cpu/armv8/start.S` | 汇编入口 |
| 6 | `arch/arm/lib/bootm.c` | 启动内核流程 |
| 7 | `drivers/` | 各种驱动 |

> 逐文件精读指南见 **U-Boot 源码精读专题**：[[04-技术/嵌入式Linux/U-Boot源码阅读/U-Boot启动流程总览|U-Boot 启动流程总览]]（含 board_f.c / board_r.c / bootm.c 等 8 篇）。

---

## 10. 完整实战流程记录

### 10.1 环境

- 宿主机：Windows + QQ 虚拟机 Ubuntu 20.04
- U-Boot 源码：主线 `~/u-boot`（2026.07 版本）
- 交叉编译器：Arm GNU Toolchain 13.2.rel1
- QEMU：4.2.1

### 10.2 完整流程（从编译到启动）

```bash
# ========== 1. 加载交叉编译工具链到 PATH ==========
export PATH=/home/z/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH

# 验证工具链
aarch64-none-linux-gnu-gcc --version
# 输出：aarch64-none-linux-gnu-gcc (Arm GNU Toolchain 13.2.rel1) 13.2.1 20231009

# ========== 2. 加载默认配置 ==========
make qemu_arm64_defconfig
# 输出：# configuration written to .config

# ========== 3. 编译 ==========
make CROSS_COMPILE=aarch64-none-linux-gnu- -j$(nproc)
# 输出末尾：
#   LD u-boot
#   OBJCOPY u-boot-nodtb.bin
#   COPY u-boot.bin
#   OFCHK .config

# ========== 4. QEMU 启动 ==========
qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -bios u-boot.bin 2>&1 | tee uboot.log
```

### 10.3 启动输出解读

```
U-Boot 2026.07-00730-g6741b0dfb41d (Jul 14 2026 - 17:44:01 +0800)

DRAM: 128 MiB                          ← QEMU 分配 128MB 内存
using memory 0x4644e000-0x4748e000 for malloc()
Core: 51 devices, 14 uclasses, devicetree: board
Flash: 64 MiB
*** Warning - bad CRC, using default environment   ← 首次启动正常警告（没有 saveenv 过）

In: serial,usbkbd
Out: serial,vidconsole
Err: serial,vidconsole
No USB controllers found                ← QEMU 没模拟 USB 控制器，正常
Net: eth0: virtio-net#32               ← QEMU 虚拟网卡

Hit any key to stop autoboot: 0         ← 倒计时（默认 2 秒，可改 CONFIG_BOOTDELAY）

Scanning for bootflows in all bootdevs  ← 扫描启动设备
Scanning global bootmeth 'efi_mgr':     ← EFI 管理器
Cannot persist EFI variables without system partition
Missing TPMv2 device for EFI_TCG_PROTOCOL
Missing RNG device for EFI_RNG_PROTOCOL

Scanning bootdev 'fw-cfg@9020000.bootdev':
fatal: no kernel available               ← 没有内核镜像，正常（我们只编译了 U-Boot）

Scanning bootdev 'virtio-net#32.bootdev':
BOOTP broadcast 1
DHCP client bound to address 10.0.2.15 (2 ms)  ← QEMU 内置 DHCP 分配 IP
*** Warning: no boot file name; using '0A00020F.img'
TFTP from server 10.0.2.2; our IP address is 10.0.2.15
Filename '0A00020F.img'.
Loading: *
TFTP error: 'Access violation' (2)      ← 没有TFTP服务器，正常
Not retrying...
No more bootdevs
(0 bootflows, 0 valid)                  ← 没找到可启动的内核

=> QEMU: Terminated                     ← Ctrl+A X 退出
```

### 10.4 关键说明

| 现象 | 原因 | 是否正常 |
|------|------|----------|
| `bad CRC` 警告 | 首次启动没 saveenv 过 | ✅ 正常 |
| `No USB controllers found` | QEMU 没模拟 USB | ✅ 正常 |
| `Missing TPMv2/RNG device` | EFI 相关，QEMU 没模拟 | ✅ 正常 |
| `no kernel available` | 只编译了 U-Boot，没有 Linux 内核 | ✅ 正常 |
| `TFTP error: Access violation` | 没有 TFTP 服务器 | ✅ 正常 |
| `=>` 提示符出现后立刻退出 | 0 秒倒计时 + 无启动项 | ✅ 正常 |

### 10.5 如果要让 U-Boot 停在命令行

```bash
# 方法1：启动时按任意键（在 "Hit any key to stop autoboot" 出现时）
# 方法2：改 CONFIG_BOOTDELAY 为更大值
# 方法3：在 QEMU 启动后快速按键
```

> 💡 踩坑提示：如果 `make menuconfig` 提示 `ncurses not found`，执行 `sudo apt install libncurses5-dev libncursesw5-dev`

---

## 11. 相关笔记

- [[04-技术/嵌入式Linux/U-Boot-ARM64启动流程白话解读-20260721|U-Boot ARM64 启动流程白话解读]] — 从上电到跳转内核的完整流程
- [[04-技术/嵌入式Linux/Buildroot系统构建与RK3588三板迁移方案|Buildroot 系统构建与 RK3588 三板迁移方案]] — QEMU→OPi 5B→ROCK 5C→定制板四阶段方案
- [[RK3588-LinuxUBoot开源项目学习指南-20260710]]
- [[RK3588-嵌入式Linux学习路径与机器狗部署答疑-20260710]]
- [[RK3588-自制开发板硬件设计指南-20260710]]
- [[RK3588-仿真模拟学习方案-20260710]]

### U-Boot 源码精读专题（TRAE 整理，2026-07-20 ingest）

> 以下 8 篇为聚焦具体源码文件的精读笔记，与本章形成互补（本章重"路线/实战"，专题重"逐文件精读"）。

- [[04-技术/嵌入式Linux/U-Boot源码阅读/U-Boot启动流程总览|U-Boot 启动流程总览]]（专题索引）
- [[04-技术/嵌入式Linux/U-Boot源码阅读/preboot与autoboot|preboot 与 autoboot]]
- [[04-技术/嵌入式Linux/U-Boot源码阅读/U-Boot引导后是否还存在|U-Boot 引导后是否还存在]]
- [[04-技术/嵌入式Linux/U-Boot源码阅读/INITCALL机制|INITCALL 机制]]
- [[04-技术/嵌入式Linux/U-Boot源码阅读/board_f.c阅读指南|board_f.c 阅读指南]]
- [[04-技术/嵌入式Linux/U-Boot源码阅读/board_r.c阅读指南|board_r.c 阅读指南]]
- [[04-技术/嵌入式Linux/U-Boot源码阅读/bootm.c阅读指南|bootm.c 阅读指南]]
- [[04-技术/嵌入式Linux/U-Boot源码阅读/CPU架构速查|CPU 架构速查]]

---

## 更新日志

- 2026-07-13：创建笔记，整合 QEMU 编译踩坑记录、源码修改实验、QEMU 仿真能力边界、RK3588 板级适配指南
- 2026-07-14：新增 menuconfig 修改配置参数章节（BOOTDELAY/SYS_PROMPT 三种修改方式对比）；新增完整实战流程记录（工具链加载→编译→QEMU启动→输出解读）
- 2026-07-15：更新§5.2 修改启动 banner——新版 U-Boot（2026.07）已移除 `display_banner()`，改为定位 `lib/display_options.c` 的 `display_options_get_banner_priv()` 和 `common/version.c` 的 `version_string`，补充版本差异速查表
- 2026-07-15：新增§12 新版 API 变化踩坑——`common.h` 移除、`init_sequence_f[]` 消失、`CONFIG_SYS_*`→`CFG_SYS_*` 改名、`gd` 结构体替代方案、条件编译陷阱

---

## 12. 新版 U-Boot API 变化踩坑

> 基于 2026.07 主线版本实际操作验证。

### 12.1 `common.h` 头文件移除

**现象**：编译自定义命令时 `#include <common.h>` 报错 "没有那个文件或目录"。

**原因**：新版 U-Boot 废弃了万能头文件 `common.h`，所需声明已分散到各专用头文件中。

**解决**：命令文件中只需 `#include <command.h>`，它包含了 `struct cmd_tbl`、`printf`、`U_BOOT_CMD` 等命令开发所需的一切。

```c
// 旧
#include <common.h>
#include <command.h>

// 新：只需要一行
#include <command.h>
```

### 12.2 `init_sequence_f[]` 数组消失

**现象**：在 `common/board_f.c` 中搜索不到 `init_sequence_f` 数组。

**原因**：新版 U-Boot 重构了 `board_init_f` 的初始化流程，不再使用函数指针数组，改为直接调用或使用 `initcall_run_list` 机制。

**解决**：直接在 `board_init_f()` 函数体末尾（`return` 之前）调用自定义函数即可，无需注册到数组中。

### 12.3 `CONFIG_SYS_*` 宏改名

**现象**：编译时 `CONFIG_SYS_TEXT_BASE`、`CONFIG_SYS_SDRAM_BASE`、`CONFIG_SYS_SDRAM_SIZE` 等宏未定义，编译器报错。

**原因**：新版 U-Boot 将 `CONFIG_SYS_*` 宏统一重命名为 `CFG_SYS_*`。编译器错误提示中已有明确说明。此外，QEMU virt 平台可能根本不定义这些宏——因为 virt 平台没有固定的物理地址。

**解决**：改用 `gd`（global_data）结构体中的运行时字段，不依赖编译期宏：

```c
static int show_memory_layout(void)
{
    printf("\n=== Memory Layout ===\n");
    printf("  GD address:   0x%08lx\n", (ulong)gd);
    printf("  RAM Base:     0x%08lx\n", (ulong)gd->ram_base);
    printf("  RAM Size:     0x%08lx\n", (ulong)gd->ram_size);
    printf("  Stack:        0x%08lx\n", (ulong)gd->start_addr_sp);
    printf("  Malloc Base:  0x%08lx\n", (ulong)gd->malloc_base);
    printf("  Malloc Limit: 0x%08lx\n", (ulong)gd->malloc_limit);
    printf("  Reloc Addr:   0x%08lx\n", (ulong)gd->relocaddr);
    printf("======================\n\n");
    return 0;
}
```

**关键 `gd` 字段一览**：

| 字段 | 类型 | 含义 |
|------|------|------|
| `gd->ram_base` | `unsigned long` | U-Boot 使用的 RAM 起始地址 |
| `gd->ram_size` | `phys_size_t` | RAM 总大小（字节） |
| `gd->start_addr_sp` | `unsigned long` | 初始栈指针地址 |
| `gd->malloc_base` | `unsigned long` | early malloc 区域起始地址 |
| `gd->malloc_limit` | `unsigned int` | early malloc 区域最大大小 |
| `gd->relocaddr` | `unsigned long` | U-Boot 重定位后的起始地址 |

### 12.4 `printf` 格式串参数不匹配

**现象**：代码写成 `printf("xxx: 0x%08lx - 0x%08lx\n", gd->malloc_base + CONFIG_SYS_MALLOC_LEN)`，两个占位符 `%lx` 只传了一个参数。

**解决**：每个 `%lx` 必须对应一个独立参数，不能把计算表达式直接嵌进去。正确写法：

```c
printf("Malloc Area: 0x%08lx - 0x%08lx\n",
       (ulong)gd->malloc_base,
       (ulong)(gd->malloc_base + gd->malloc_limit));
```

### 12.5 `#if defined(CONFIG_X86)` 条件编译陷阱

**现象**：`show_memory_layout()` 写好了但编译后不生效，无法在 QEMU 中看到输出。

**原因**：函数被放在了 `#if defined(CONFIG_X86) || defined(CONFIG_ARC)` 编译条件块内。ARM 平台（aarch64）不会编译这段代码。

**解决**：确保自定义函数写在条件编译块之外（`#endif` 之后），让它对所有架构都可见。

```c
#if defined(CONFIG_X86) || defined(CONFIG_ARC)
    // ... x86/ARC 专用代码 ...
#endif  // ← 在这里结束条件编译

// 自定义函数放在 #endif 之后，对所有架构可见
static int show_memory_layout(void)
{
    // ...
}
```

### 12.6 踩坑心得总结

| 坑 | 根因 | 通用原则 |
|---|------|----------|
| common.h 移除 | API 演进，不再提供万能头文件 | 只 include 实际需要的头文件 |
| init_sequence_f 消失 | 架构重构，废弃函数指针数组 | 直接插入调用代码，不依赖旧的数据结构 |
| CONFIG_SYS_* 未定义 | 宏重命名 + virt 平台无固定地址 | 优先使用运行时 gd 结构体 |
| printf 参数不匹配 | C 语言基础错误 | 一个占位符一个参数 |
| 条件编译陷阱 | 架构判断失误 | 注意 `CONFIG_X86` 只对 x86 生效 |

---

## 13. 在 U-Boot 中运行程序

> 能否在 U-Boot 中运行自定义程序？能。能否写摄像头采集程序？理论上能但极不推荐。本节详细分析。

### 13.1 U-Boot 运行程序的四种方式

#### 方式一：Standalone 应用（`go` 命令）

U-Boot 提供 standalone API，允许编译独立小程序在 U-Boot 环境中直接运行，不需要 Linux 内核。

**源码自带示例**：
```
examples/standalone/
  ├── hello_world.c      ← 最简单的示例
  ├── stub_functions.c   ← U-Boot API 桩函数
  └── Makefile
```

**编译**：
```bash
make CROSS_COMPILE=aarch64-none-linux-gnu- -j$(nproc) examples
```

**运行**：
```bash
# 加载到内存（TFTP 或 SD 卡）
=> tftp 0x40000000 hello_world.bin
# 或
=> fatload mmc 0 0x40000000 hello_world.bin

# 执行
=> go 0x40000000
```

**standalone 程序能用什么**：

| 能力 | 说明 |
|------|------|
| printf 输出 | 通过 U-Boot 串口 |
| 内存读写 | 直接操作物理地址 |
| U-Boot API 调用 | 通过 `exports.h` 导出的函数表（getenv/setenv/stdio 等）|
| ❌ Linux 系统调用 | 没有 OS，无 syscall |
| ❌ 标准C库 | malloc/printf 是 U-Boot 提供的简化版 |

#### 方式二：自定义命令（编译进 U-Boot）

之前已实现过——写 `cmd/hello.c` 注册自定义命令。本质上是"在 U-Boot 里运行程序"，只是程序被编译进了 U-Boot 本体。详见 §5.3。

#### 方式三：`bootm` / `booti` 启动 Linux 内核

不算"在 U-Boot 里运行"，而是 U-Boot 加载并跳转到 Linux 内核。但可以加载任何符合 ARM64 Image 格式的程序：
```bash
=> booti 0x40000000 - 0x42000000   # kernel - dtb
```

#### 方式四：脚本方式（`source` 命令）

U-Boot 支持运行脚本，相当于批处理程序：
```bash
# 加载脚本到内存
=> fatload mmc 0 0x40000000 boot.scr

# 执行脚本
=> source 0x40000000
```

脚本内容示例：
```bash
# boot.scr
echo "Running my script..."
setenv bootargs console=ttyAMA0
load mmc 0 0x40000000 Image
booti 0x40000000
```

#### 四种方式对比

| 方式 | 复杂度 | 能力 | 适用场景 |
|------|--------|------|----------|
| **standalone (`go`)** | 中 | 直接操作硬件，有 U-Boot API | 硬件测试、裸机程序 |
| **自定义命令** | 低 | U-Boot 内部功能 | 扩展 U-Boot 功能 |
| **`bootm`/`booti`** | 高 | 完整 OS | 启动 Linux |
| **脚本 (`source`)** | 低 | U-Boot 命令组合 | 自动化启动流程 |

### 13.2 在 U-Boot 中写摄像头程序的可行性

#### 结论：理论上能，实际上极不推荐

U-Boot 的 `drivers/video/` 目录只有**输出类**驱动（LCD/HDMI/eDP 显示面板），**没有摄像头输入驱动**。

| 方向 | Linux 内核 | U-Boot |
|------|-----------|--------|
| 显示输出 | `drivers/gpu/drm/`、`drivers/video/fbdev/` | `drivers/video/` ✅ 有 |
| 摄像头输入 | `drivers/media/usb/uvc/`、`drivers/media/platform/` | ❌ **没有** |
| USB UVC | `drivers/media/usb/uvc/uvc_driver.c` | ❌ **没有** |
| V4L2 框架 | `drivers/media/v4l2-core/` | ❌ **没有** |

U-Boot 定位是 Bootloader，只需要显示 logo/菜单，不需要图像采集。

#### 如果硬要写 USB 摄像头程序，需要从零实现：

```
1. USB Host 驱动        ← U-Boot 有（drivers/usb/host/）
2. USB 设备枚举          ← U-Boot 有（但只支持存储/网络/键盘类）
3. UVC 协议栈            ← U-Boot 没有，需自己写
   - 视频流探测/协商
   - 等时传输（isochronous transfer）
   - MJPEG/YUYV 解码
4. 帧缓冲显示            ← U-Boot 有（drivers/video/）
```

其中 **UVC 协议栈**是最难的部分——Linux 内核的 UVC 驱动有几千行代码，U-Boot 里完全没有。

#### 方案对比

| 方案 | 难度 | 推荐度 |
|------|------|--------|
| U-Boot 里写摄像头驱动 | 🔴 极高，需自己实现 UVC 协议栈 | ❌ 不推荐 |
| U-Boot 里写非 USB 摄像头驱动（MIPI/并行接口） | 🟡 高，但可行 | ⚠️ 看具体硬件 |
| U-Boot standby + `go` 跳转裸机程序 | 🟡 中 | ✅ 可行 |
| 进 Linux 内核后用 V4L2 | 🟢 低 | ✅✅ 强烈推荐 |
| U-Boot 只做显示，Linux 做采集 | 🟢 低 | ✅✅ 标准做法 |

#### 正确路线

```
U-Boot 负责启动 → 加载 Linux 内核 → 在 Linux 中用 V4L2/UVC 驱动操作摄像头
```

进 Linux 后一行命令就能拍照：
```bash
v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=1
```

> 💡 如果目的是学习裸机摄像头，可以用 U-Boot 的 `go` 命令跳转到自己写的裸机程序，但这与"在 U-Boot 中运行"已不同——U-Boot 只负责启动，程序独立运行在裸金属上。

---

## 14. Rockchip SDK U-Boot 编译实战（RK3588 真板）

> 2026-07-22：使用 Rockchip 官方 SDK U-Boot 成功编译 RK3588 烧录镜像。

### 14.1 主线 U-Boot vs Rockchip SDK U-Boot

| 对比项 | 主线 U-Boot | Rockchip SDK U-Boot |
|--------|------------|---------------------|
| 仓库 | `github.com/u-boot/u-boot` | `github.com/rockchip-linux/u-boot` |
| 目录结构 | `arch/arm64/` | `arch/arm/`（含 armv8 子目录）|
| ARCH 参数 | `ARCH=arm64` | **`ARCH=arm`** |
| defconfig | `evb-rk3588_defconfig` | **`rk3588_defconfig`** |
| BL31/TPL | 不需要 | 需要（rkbin 提供）|
| 适用场景 | QEMU 仿真验证 | **真板烧录** |

⚠️ **关键区别**：Rockchip SDK U-Boot 虽然编译 64 位代码，但 `ARCH=arm`，不要用 `ARCH=arm64`，否则符号链接 `arch/arm64/include/asm/arch` 创建失败。

### 14.2 环境准备

```bash
# 1. 克隆 rkbin（Rockchip 预编译固件）
cd ~
git clone https://github.com/rockchip-linux/rkbin.git

# 2. 克隆 Rockchip SDK U-Boot
git clone --depth=1 https://github.com/rockchip-linux/u-boot.git u-boot-rk
cd u-boot-rk

# 3. 安装依赖
sudo apt install device-tree-compiler -y
```

rkbin 中 RK3588 关键固件：
- `rk3588_bl31_v1.54.elf` — BL31（ARM Trusted Firmware）
- `rk3588_ddr_1.15.bin` — DDR 初始化固件
- `rk3588_spl_loader_v1.xx.bin` — SPL loader

### 14.3 编译流程

```bash
# 配置
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- rk3588_defconfig

# 编译
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- \
  BL31=../rkbin/bin/rk35/rk3588_bl31_v1.54.elf \
  ROCKCHIP_TPL=../rkbin/bin/rk35/rk3588_ddr_1.15.bin \
  -j$(nproc)
```

### 14.4 生成 u-boot.itb 和 idbloader.img

`make` 默认只生成 `u-boot.bin` 和 `spl/u-boot-spl.bin`，还需额外步骤生成烧录镜像。

#### 问题1：bl31.elf 找不到

`make_fit_atf.sh` 脚本在当前目录找 `./bl31.elf`，不是用 BL31 参数路径。需要手动复制：

```bash
cp ../rkbin/bin/rk35/rk3588_bl31_v1.54.elf bl31.elf
```

#### 问题2：tee.bin 找不到

`make_fit_atf.sh` 还需要 `tee.bin`（OP-TEE 固件）。rkbin 里没有 RK3588 的 tee，创建空文件跳过：

```bash
touch tee.bin
```

#### 生成 u-boot.itb

```bash
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- \
  BL31=../rkbin/bin/rk35/rk3588_bl31_v1.54.elf \
  u-boot.itb
```

#### 生成 idbloader.img

```bash
tools/mkimage -n rk3588 -T rksd \
  -d tpl/u-boot-tpl.bin:spl/u-boot-spl.bin \
  idbloader.img
```

### 14.5 烧录产物

| 文件 | 大小 | 用途 | 烧录位置 |
|------|------|------|----------|
| `idbloader.img` | 320K | DDR init + SPL | SD 卡 `seek=64`（0x40 扇区，32KB）|
| `u-boot.itb` | 1.6M | U-Boot 主体 + BL31 | SD 卡 `seek=16384`（0x4000 扇区，8MB）|
| `u-boot.bin` | 1.4M | 原始镜像 | 备用 |

### 14.6 烧录到 SD 卡

```bash
# 假设 SD 卡为 /dev/sdb
sudo dd if=idbloader.img of=/dev/sdb seek=64
sudo dd if=u-boot.itb of=/dev/sdb seek=16384
sync
```

⚠️ RK3588 串口波特率为 **1500000**，不是 115200。

### 14.7 三块板子适配

| 板子 | defconfig | 备注 |
|------|-----------|------|
| EVB/自制板 | `rk3588_defconfig` | 通用配置，外设可能不全但能启动 |
| ROCK 5C | `rock-5c-rk3588s_defconfig`（主线）| SDK 中可用 `rk3588_defconfig` |
| Orange Pi 5B | `orangepi-5-rk3588s_defconfig`（主线）| SDK 中可用 `rk3588_defconfig` |

### 14.8 踩坑总结

| 问题 | 原因 | 解决 |
|------|------|------|
| `arch/arm64/` 不存在 | Rockchip SDK 用 `arch/arm/`，不是 `arch/arm64/` | 用 `ARCH=arm` |
| `rk3588-evb1-v10_defconfig` 找不到 | SDK 名字是 `rk3588_defconfig` | 用正确名字 |
| `bl31.elf` 找不到 | 脚本在当前目录找，不是用 BL31 参数 | 手动 `cp` 复制 |
| `tee.bin` 找不到 | rkbin 无 RK3588 tee | `touch tee.bin` 空文件 |
| `dtc: not found` | 未安装设备树编译器 | `apt install device-tree-compiler` |
| git clone 不完整 | 磁盘空间 93% 满 | 删旧目录 + `--depth=1` |

### 14.9 相关链接

- [[Buildroot系统构建与RK3588三板迁移方案|Buildroot 系统构建与 RK3588 三板迁移方案]]
- [[U-Boot-ARM64启动流程白话解读-20260721|U-Boot ARM64 启动流程白话解读]]
- [[U-Boot-ARM64启动流程全解文档-20260721|U-Boot ARM64 启动流程全解文档]]
- [[嵌入式存储与内存概念辨析-20260721|嵌入式存储与内存概念辨析]]

---

## 更新日志

- 2026-07-22：新增§14 Rockchip SDK U-Boot 编译实战——主线 vs SDK 区别（ARCH=arm、defconfig 名字不同）、完整编译流程、bl31.elf/tee.bin/dtc 踩坑、idbloader.img 和 u-boot.itb 生成方法、SD 卡烧录命令
- 2026-07-21：新增§13 在 U-Boot 中运行程序——四种运行方式（standalone/go/脚本/bootm）、摄像头程序可行性分析（U-Boot 无摄像头驱动框架，需进 Linux 用 V4L2）
