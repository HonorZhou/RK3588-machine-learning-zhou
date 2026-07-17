---
title: U-Boot 动手实验操作说明
author: 用户整理
date: 2026-07-17
type: 技术
tags: [U-Boot, QEMU, 嵌入式Linux, ARM, 实验指南]
source: 用户QQ发送
---

# U-Boot 动手实验操作说明

> 整理日期：2026-07-17
> 原文来源：`qume-uboot.md`

## 第一层：改配置（不改代码）

### 操作：`make menuconfig`

```bash
cd ~/u-boot
make menuconfig
```

`make menuconfig` 启动一个基于 ncurses 的终端图形化配置界面。它读取 `Kconfig` 文件（分布在不同子目录的配置描述文件），生成一个树状选项菜单。用户勾选或修改后，写入 `.config` 文件。

这个界面的三要素：

- **`Kconfig` 文件**：定义"有哪些选项、类型是什么、默认值是什么"。就像菜单的菜谱。
- **`.config` 文件**：记录"用户选了哪些"。是 `KEY=VALUE` 格式的纯文本。例如 `CONFIG_BOOTDELAY=10`。
- **`CONFIG_` 宏**：编译时，`.config` 中的值被转化为 C 预处理宏（写入 `include/generated/autoconf.h`），源码通过 `#ifdef CONFIG_XXX` 或 `CONFIG_XXX` 直接引用。

### 配置项解释

#### `CONFIG_BOOTDELAY`

启动倒计时秒数。U-Boot 启动后会显示 "Hit any key to stop autoboot"，等待这段时间。若超时无人按键，自动执行 `bootcmd` 指定的启动命令（通常是加载内核）。默认值通常是 2 秒，改成 10 秒给调试留更多时间。

实现位置在 `common/autoboot.c` 中，倒计时代码类似：

```c
for (i = CONFIG_BOOTDELAY; i > 0; i--) {
    printf("... %d ...", i);
    // 检测按键，有则跳出循环进入命令行
}
```

#### `CONFIG_SYS_PROMPT`

命令行提示符字符串。默认是 `=> `（带空格），改成 `myuboot> ` 后终端显示变为：

```
myuboot> help
```

这只是字符串替换，不影响任何功能，但能帮你确认"这是我编译的版本"。

#### `CONFIG_CMD_BOOTM=n`

关闭 `bootm` 命令的编译。`menuconfig` 中取消勾选后，`.config` 里变为 `# CONFIG_CMD_BOOTM is not set`，Makefile 中 `obj-$(CONFIG_CMD_BOOTM)` 展开为空字符串，`cmd/bootm.c` 不会被编译。

效果：编译后进入 U-Boot，输入 `bootm` 会提示 `Unknown command`。这就是 U-Boot 的命令剪裁机制——嵌入式设备存储有限，不需要的命令不编译进镜像。

### 操作：QEMU 运行

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -bios u-boot.bin 2>&1 | tee uboot.log
```

参数解释：

| 参数 | 含义 |
|------|------|
| `-machine virt` | 使用 QEMU 的通用 ARM 虚拟平台（不模拟真实板卡） |
| `-cpu cortex-a57` | 指定 CPU 型号为 ARMv8 架构的 Cortex-A57 |
| `-nographic` | 不使用图形窗口，串口输出直接显示在终端 |
| `-bios u-boot.bin` | 将 u-boot.bin 加载到固件地址，作为第一段启动代码 |
| `2>&1` | 把 stderr 重定向到 stdout |
| `\| tee uboot.log` | 同时输出到终端和保存到日志文件 |

核心要点：`-bios u-boot.bin` 让 QEMU 像真实硬件一样，把 U-Boot 二进制放在固件入口地址（ARM virt 平台默认是 `0x00000000`），CPU 上电后的第一条指令就从这里执行。

---

## 第二层：改源码

### 实验 1：修改启动 banner

```c
static int display_banner(void)
{
    printf("\n\n%s\n\n", version_string);
    printf("=== Hello, I modified U-Boot! ===\n\n");
    return 0;
}
```

`display_banner()` 是 U-Boot 启动后打印的第一行信息所在的函数。它在 `board_init_r()` 的初始化序列中被调用。`version_string` 包含了 U-Boot 版本号、编译时间和编译器信息。

这个实验的意义不在于"加一行打印"，而是让你找到这个函数的位置、理解它是 init sequence 的一部分、并亲手验证"改一个函数 → 重新编译 → 启动就能看到变化"的闭环。

> **⚠️ 版本差异（2026.07 版本）**：banner 打印已迁移至 `lib/display_options.c` 的 `display_options_get_banner_priv()` 函数。修改方式：改 `version_string` 定义或改 `display_options_get_banner_priv()`。

### 实验 2：加自定义命令

#### 2.1 命令处理函数 `do_hello()`

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
```

| 参数 | 含义 | 示例（输入 `hello world`） |
|------|------|--------------------------|
| `cmdtp` | 指向本条命令在命令表中的条目 | 不需要手动使用 |
| `flag` | 调用标志（如 `CMD_FLAG_REPEAT` 表示重复执行） | 一般忽略 |
| `argc` | 参数个数（含命令本身） | `2` |
| `argv` | 参数字符串数组 | `argv[0]="hello"`, `argv[1]="world"` |

返回值 `0` 表示成功，非零表示错误码。

#### 2.2 注册宏 `U_BOOT_CMD`

```c
U_BOOT_CMD(
    hello,                      /* 命令名：在终端输入的名字 */
    CONFIG_SYS_MAXARGS,         /* 最大参数数（平台定义，通常 16 或 32） */
    1,                          /* 是否可重复执行：1=输入空回车时重复上次命令 */
    do_hello,                   /* 回调函数指针 */
    "print hello message",      /* 简短帮助：输入 help 时显示 */
    "hello [name] - print a greeting message"  /* 详细帮助：输入 help hello 时显示 */
);
```

这个宏展开后做的事情：在编译期生成一个 `struct cmd_tbl` 结构体实例，并把它塞进一个**特殊的链接段**（`.u_boot_list_2_cmd_*`）。U-Boot 启动时扫描这个段，把里面所有命令条目挂载进命令哈希表——不需要任何手动的 `register_command()` 调用。

#### 2.3 Makefile 条件编译

```makefile
obj-$(CONFIG_CMD_HELLO) += hello.o
```

Kbuild 的语法。`$(CONFIG_CMD_HELLO)` 的值由 Kconfig 决定：

- 如果 `CONFIG_CMD_HELLO=y`，展开为 `obj-y += hello.o` → **编译** `hello.c`
- 如果 `CONFIG_CMD_HELLO=n` 或被注释，展开为 `obj-n` → **不编译**，hello 命令不存在于最终镜像

#### 2.4 Kconfig 配置项

```kconfig
config CMD_HELLO
    bool "hello command"
    default y
    help
      Print a hello message.
```

| 字段 | 含义 |
|------|------|
| `config CMD_HELLO` | 定义一个叫 `CMD_HELLO` 的配置项 |
| `bool "hello command"` | 布尔型选项，`make menuconfig` 时显示为 `[*] hello command` |
| `default y` | 默认启用。用户可手动关掉，或在 `defconfig` 里覆盖 |
| `help` | `menuconfig` 里按 `?` 时显示的说明文字 |

#### 2.5 整体链路

```
menuconfig 勾选 CMD_HELLO
    ↓
.config 中生成 CONFIG_CMD_HELLO=y
    ↓
Makefile: obj-y += hello.o → 编译 hello.c
    ↓
U_BOOT_CMD 宏在链接段注册命令条目
    ↓
U-Boot 启动时扫描链接段 → 命令可用
    ↓
终端输入 hello → do_hello() 被调用
```

### 实验 3：打印内存布局

```c
static int show_memory_layout(void)
{
    printf("\n=== Memory Layout ===\n");
    printf("  TEXT_BASE:    0x%08lx\n", (ulong)CONFIG_SYS_TEXT_BASE);
    printf("  RAM Start:    0x%08lx\n", (ulong)CONFIG_SYS_SDRAM_BASE);
    printf("  RAM Size:     0x%08lx\n", (ulong)CONFIG_SYS_SDRAM_SIZE);
    printf("  Stack:        0x%08lx\n", (ulong)gd->start_addr_sp);
    printf("  Malloc Area:  0x%08lx - 0x%08lx\n",
           (ulong)gd->malloc_base,
           (ulong)(gd->malloc_base + CONFIG_SYS_MALLOC_LEN));
    printf("======================\n\n");
    return 0;
}
```

各字段的含义：

| 打印项 | 来源 | 含义 |
|--------|------|------|
| `TEXT_BASE` | `CONFIG_SYS_TEXT_BASE`（Kconfig 配置） | U-Boot 代码段的链接起始地址。重定位前代码在此运行，重定位后代码被搬走 |
| `RAM Start` | `CONFIG_SYS_SDRAM_BASE`（板级 Kconfig） | 物理内存起始地址 |
| `RAM Size` | `CONFIG_SYS_SDRAM_SIZE` | 可用物理内存大小 |
| `Stack` | `gd->start_addr_sp` | 栈指针位置。在 `board_init_f()` 中设置，通常放在 RAM 顶部 |
| `Malloc Area` | `gd->malloc_base` + `CONFIG_SYS_MALLOC_LEN` | U-Boot 堆区（动态内存分配区域）的起止地址 |

**`gd`（global_data）** 是 U-Boot 的全局数据结构体（`struct global_data`），定义在 `include/asm-generic/global_data.h`。它是一个贯穿整个启动生命周期的大管家——从 `board_init_f` 到 `board_init_r`，再到命令行循环，所有模块都通过 `gd` 获取全局状态。在 ARM 平台上，`gd` 存放在专门的寄存器中（X18/TPIDR_EL1），避免重定位后指针失效。

---

## 第三层：理解启动流程

U-Boot 启动分两个阶段，每个阶段有一个 C 入口。在这里加 `printf` 是跟踪执行顺序最直观的方法。

### `board_init_f()` — 早期初始化

运行在**重定位之前**。此时 U-Boot 还在链接地址（通常是 Flash 或 ROM 的低地址）执行，RAM 可能还没初始化或只有一部分可用。

主要工作：初始化串口、时钟、DRAM 控制器，设置 `gd` 的基本字段，为后续的重定位准备环境。在这个阶段的函数内部，**不能用 `malloc`**（堆还没建立），**不能用全局变量**（重定位后地址会变化，BSS 段会丢失）。

### `relocate_code()` — 代码重定位

把 U-Boot 自身从当前位置（Flash/低地址）拷贝到 RAM 的高地址区域。同时调整：

- 代码段的绝对地址引用（GOT 表修复）
- `gd` 指针更新
- 栈指针重新指向新的位置

为什么要重定位？因为 U-Boot 可能从只读存储器（NOR Flash、QSPI）启动，执行速度慢；搬到 RAM 后运行速度大幅提升。另外，Linux 内核需要加载到 RAM 的特定位置，U-Boot 必须腾出低地址空间。

### `board_init_r()` — 后期初始化

重定位完成后执行。此时已在 RAM 中运行，堆已建立（`malloc` 可用），BSS 段已清零（全局变量可用）。

主要工作：初始化各种外设驱动（网卡、MMC、USB）、挂载文件系统、读取环境变量、建立命令表。最后调用 `run_main_loop()` 进入交互命令行或自动启动流程。

### 启动流程图

```
CPU 上电
  ↓
arch/arm/cpu/armv8/start.S    ← 汇编入口：设置异常向量、初始化 MMU/Cache
  ↓
board_init_f()                 ← C 语言入口：串口、DRAM、gd 初始化
  ↓
relocate_code()                ← 把自己搬到 RAM 高地址
  ↓
board_init_r()                 ← 外设驱动、环境变量、命令表
  ↓
run_main_loop()                ← 命令行循环 或 执行 bootcmd 自动启动内核
```

### 关键函数加追踪打印

```c
// common/board_f.c
void board_init_f(ulong boot_flags)
{
    printf("[TRACE] board_init_f entered\n");
    // ... 原有代码
}

// common/board_r.c
void board_init_r(gd_t *new_gd, ulong dest_addr)
{
    printf("[TRACE] board_init_r entered\n");
    // ... 原有代码
}
```

编译运行后效果：

```
[TRACE] board_init_f entered
[TRACE] board_init_r entered
U-Boot 2026.xx ...
```

---

## 第四层：读源码路线

| 顺序 | 文件 | 为什么从这里开始 |
|------|------|-----------------|
| 1 | `common/main.c` | 代码量少，逻辑清晰。只有两个函数：`run_main_loop()` 和命令行读取循环。这是 U-Boot 对用户最直观的入口 |
| 2 | `common/board_f.c` | 初始化序列的"前半场"。函数指针数组 `init_sequence_f[]` 定义了执行顺序，按索引顺序执行。先读这个数组再回头看各个函数 |
| 3 | `common/board_r.c` | 初始化序列的"后半场"。`init_sequence_r[]` 同样是指针数组。在这里你会看到命令表建立、网络栈初始化、`bootdelay` 倒计时等熟悉的功能 |
| 4 | `cmd/` 目录 | 各种命令的实现。挑你感兴趣的读（`bootm.c`、`md.c` 内存显示、`mmc.c` 等），每个命令都是独立模块 |
| 5 | `arch/arm/cpu/armv8/start.S` | 汇编入口。理解 `_start` 标号、异常向量表、`lowlevel_init`、MMU 设置。这是最底层，但代码量不大（几百行） |
| 6 | `arch/arm/lib/bootm.c` | 启动 Linux 内核的核心逻辑：设置 ATAGS/设备树、准备寄存器、`kernel_entry()` 跳转。理解从 U-Boot 到 Linux 的交接过程 |
| 7 | `drivers/` | 各种外设驱动，按需要查阅即可 |

**核心阅读技巧**：U-Boot 大量使用**函数指针数组**来组织初始化序列。`board_f.c` 和 `board_r.c` 的 `init_sequence` 数组就是典型——读懂数组里的函数名和执行顺序，就理解了整个启动流程，不必迷失在每个函数的实现细节里。

---

## 建议的实验路线

| 天数 | 内容 | 学什么 |
|------|------|--------|
| 第 1 天 | 改 `menuconfig` 配置 | 理解 Kconfig → `.config` → Makefile 的配置链条 |
| 第 2 天 | 改 banner + 加 `hello` 命令 | 源码级修改：命令注册（`U_BOOT_CMD` 宏 + 链接段）、init sequence、`gd_t` 全局数据结构 |
| 第 3 天 | 加启动流程打印 | SPL→U-Boot 的两阶段启动流程：`start.S` → `board_init_f` → `relocate_code` → `board_init_r` → `main_loop` |
| 第 4 天 | 读 `main.c` + `board_r.c` | 对照打印输出，理解代码和数据结构的对应关系 |
| 第 5 天 | 读 `bootm.c` | 终极目标：理解 U-Boot 如何把控制权交给 Linux 内核 |

建议严格按顺序走：

1. **改 `menuconfig`** → 理解"配置项如何影响编译结果"（这是后续一切实验的基础）
2. **加 `hello` 命令** → 理解命令注册机制和 `U_BOOT_CMD` 宏 + 链接段的玩法
3. **启动流程加打印** → 亲眼看到 `board_init_f → relocate → board_init_r → main_loop` 的执行顺序
4. **读 `main.c` + `board_r.c`** → 对照之前看到的打印输出，理解代码和数据结构的对应关系
5. **读 `bootm.c`** → 终极目标：理解 U-Boot 如何把控制权交给 Linux 内核
