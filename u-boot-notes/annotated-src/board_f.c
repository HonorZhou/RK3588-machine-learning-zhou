// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2011 The Chromium OS Authors.
 * (C) Copyright 2002-2006
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2002
 * Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Marius Groeger <mgroeger@sysgo.de>
 */

#include <config.h>
#include <bloblist.h>
#include <bootstage.h>
#include <clock_legacy.h>
#include <console.h>
#include <cpu.h>
#include <cpu_func.h>
#include <cyclic.h>
#include <display_options.h>
#include <dm.h>
#include <env.h>
#include <env_internal.h>
#include <event.h>
#include <fdtdec.h>
#include <fs.h>
#include <hang.h>
#include <i2c.h>
#include <init.h>
#include <initcall.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <memtop.h>
#include <os.h>
#include <post.h>
#include <relocate.h>
#include <serial.h>
#include <spl.h>
#include <sysreset.h>
#include <time.h>
#include <timer.h>
#include <trace.h>
#include <upl.h>
#include <video.h>
#include <watchdog.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/sections.h>
#include <dm/root.h>
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/sizes.h>

/*
 * =========================================================================
 * board_f.c —— U-Boot relocation(重定位)之前的"第一阶段"初始化
 * -------------------------------------------------------------------------
 * 本文件实现 board_init_f()，它是 U-Boot 从汇编(_main / crt0.S)进入 C 语言
 * 后的第一个大函数。它唯一的核心就是按顺序调用 initcall_run_f() 里的一长串
 * INITCALL，完成：
 *   - 早期内存分配器(early malloc，即 malloc_f)；
 *   - 串口 / 控制台 / 环境初始化；
 *   - DRAM 探测(dram_init)；
 *   - 从内存顶部向下预留 relocation 所需的各种区域(uboot 自身、malloc、bd、
 *     gd、fdt、bootstage、bloblist、栈等)；
 *   - 计算 relocation 偏移并把 FDT/bootstage/bloblist 拷贝到新位置。
 * 在非 ARM 架构上，最后由 jump_to_copy -> relocate_code 把自身搬到 RAM；
 * 在 ARM 上这一步由 crt0.S 完成 relocation，随后调用 board_init_r()。
 *
 * 关键全局变量：
 *   gd        —— global data 指针(被固定在某寄存器，如 aarch64 的 x18)，贯穿
 *               整个 U-Boot 生命周期，保存 ram_top/relocaddr/bd/fdt_blob 等。
 *   bd        —— board info 结构(gd->bd)，保存内存 bank、机器号等信息。
 *   mon_len   —— U-Boot 映像(代码+数据+BSS)总长度，用于预留自身空间。
 *   relocaddr —— 当前预留的 relocation 目标地址(从 RAM 顶向下递减)。
 *   start_addr_sp —— 预留完成后最终栈指针位置。
 * =========================================================================
 */

DECLARE_GLOBAL_DATA_PTR;

/*
 * Why is gd allocated a register? Prior to reloc it might be better to
 * just pass it around to each function in this file?
 *
 * After reloc one could argue that it is hardly used and doesn't need
 * to be in a register. Or if it is it should perhaps hold pointers to all
 * global data for all modules, so that post-reloc we can avoid the massive
 * literal pool we get on ARM. Or perhaps just encourage each module to use
 * a structure...
 */

#if defined(CONFIG_WATCHDOG) || defined(CONFIG_HW_WATCHDOG)
static int init_func_watchdog_init(void)
{
	/* 看门狗初始化 INITCALL：开启硬件看门狗(仅部分架构支持)并喂一次狗 */
# if defined(CONFIG_HW_WATCHDOG) && \
	(defined(CONFIG_M68K) || defined(CONFIG_MICROBLAZE) || \
	defined(CONFIG_SH) || \
	defined(CONFIG_DESIGNWARE_WATCHDOG) || \
	defined(CONFIG_IMX_WATCHDOG))
	hw_watchdog_init();
	puts("       Watchdog enabled\n");
# endif
	schedule();	/* 调用调度钩子(会触发 watchdog 喂狗)，防止复位 */

	return 0;
}

int init_func_watchdog_reset(void)
{
	/* 看门狗复位 INITCALL：在较长初始化期间周期性喂狗，避免被看门狗复位 */
	schedule();

	return 0;
}
#endif /* CONFIG_WATCHDOG */

__weak void board_add_ram_info(int use_default)
{
	/* 弱符号：平台可覆盖，用于在打印 DRAM 配置时补充自定义信息(默认什么都不做) */
	/* please define platform specific board_add_ram_info() */
}

static int init_baud_rate(void)
{
	/* 初始化波特率：从环境变量 "baudrate" 读取，缺省使用 CONFIG_BAUDRATE */
	gd->baudrate = env_get_ulong("baudrate", 10, CONFIG_BAUDRATE);
	return 0;
}

static int display_text_info(void)
{
	/* 调试阶段打印 U-Boot 代码段与 BSS 的地址范围 */
#if !defined(CONFIG_SANDBOX) && !defined(CONFIG_EFI_APP)
	ulong bss_start, bss_end, text_base;

	bss_start = (ulong)__bss_start;
	bss_end = (ulong)__bss_end;

#ifdef CONFIG_TEXT_BASE
	text_base = CONFIG_TEXT_BASE;
#else
	text_base = CONFIG_SYS_MONITOR_BASE;
#endif

	debug("U-Boot code: %08lX -> %08lX  BSS: -> %08lX\n",
	      text_base, bss_start, bss_end);
#endif

	return 0;
}

#ifdef CONFIG_SYSRESET
static int print_resetinfo(void)
{
	/* 打印复位原因：遍历 sysreset 设备，读出并拼接复位状态字符串输出 */
	struct udevice *dev;
	char status[256];
	bool status_printed = false;
	int ret;

	/*
	 * Not all boards have sysreset drivers available during early
	 * boot, so don't fail if one can't be found.
	 */
	for (ret = uclass_first_device_check(UCLASS_SYSRESET, &dev); dev;
	     ret = uclass_next_device_check(&dev)) {
		if (ret) {
			debug("%s: %s sysreset device (error: %d)\n",
			      __func__, dev->name, ret);
			continue;
		}

		if (!sysreset_get_status(dev, status, sizeof(status))) {
			printf("%s%s", status_printed ? " " : "", status);
			status_printed = true;
		}
	}
	if (status_printed)
		printf("\n");

	return 0;
}
#endif

#if defined(CONFIG_DISPLAY_CPUINFO) && CONFIG_IS_ENABLED(CPU)
static int print_cpuinfo(void)
{
	/* 打印 CPU 描述信息(型号 / 频率等) */
	struct udevice *dev;
	char desc[512];
	int ret;

	dev = cpu_get_current_dev();
	if (!dev) {
		debug("%s: Could not get CPU device\n",
		      __func__);
		return -ENODEV;
	}

	ret = cpu_get_desc(dev, desc, sizeof(desc));
	if (ret) {
		debug("%s: Could not get CPU description (err = %d)\n",
		      dev->name, ret);
		return ret;
	}

	printf("CPU:   %s\n", desc);

	return 0;
}
#endif

static int announce_dram_init(void)
{
	/* 在探测 DRAM 前打印 "DRAM:  " 提示符，后面由 show_dram_config 补充容量 */
	puts("DRAM:  ");
	return 0;
}

/*
 * From input size calculate its nearest rounded unit scale (multiply of 2^10)
 * and value in calculated unit scale multiplied by 10 (as fractional fixed
 * point number with one decimal digit), which is human natural format,
 * same what uses print_size() function for displaying. Mathematically it is:
 * round_nearest(val * 2^scale) = size * 10; where: 10 <= val < 10240.
 *
 * For example for size=87654321 we calculate scale=20 and val=836 which means
 * that input has natural human format 83.6 M (mega = 2^20).
 */
#define compute_size_scale_val(size, scale, val) do { \
	scale = ilog2(size) / 10 * 10; \
	val = (10 * size + ((1ULL << scale) >> 1)) >> scale; \
	if (val == 10240) { val = 10; scale += 10; } \
} while (0)

/*
 * Check if the sizes in their natural units written in decimal format with
 * one fraction number are same.
 */
static int sizes_near(unsigned long long size1, unsigned long long size2)
{
	/* 比较两个内存大小在"人类可读格式(一位小数)"下是否相等；
	 * 用于检测 gd->ram_size 与所有 DRAM bank 总和是否一致。 */
	unsigned int size1_scale, size1_val, size2_scale, size2_val;

	compute_size_scale_val(size1, size1_scale, size1_val);
	compute_size_scale_val(size2, size2_scale, size2_val);

	return size1_scale == size2_scale && size1_val == size2_val;
}

static int show_dram_config(void)
{
	/* 汇总并打印各 DRAM bank 与总容量；若与 gd->ram_size 不一致则附带 (total ...) */
	unsigned long long size;
	int i;

	debug("\nRAM Configuration:\n");
	for (i = size = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		size += gd->dram[i].size;
		debug("Bank #%d: %llx ", i,
		      (unsigned long long)(gd->dram[i].start));
#ifdef DEBUG
		print_size(gd->dram[i].size, "\n");
#endif
	}
	debug("\nDRAM:  ");

	print_size(gd->ram_size, "");
	if (!sizes_near(gd->ram_size, size)) {
		printf(" (total ");
		print_size(size, ")");
	}
	board_add_ram_info(0);
	putc('\n');

	return 0;
}

__weak int dram_init_banksize(void)
{
	/* 弱符号：初始化 gd->dram[] 各 bank 的起始地址与大小(默认单 bank：ram_base + 有效内存大小) */
	gd->dram[0].start = gd->ram_base;
	gd->dram[0].size = get_effective_memsize();

	return 0;
}

#if CONFIG_IS_ENABLED(SYS_I2C_LEGACY)
static int init_func_i2c(void)
{
	/* 初始化 I2C 控制器并打印 "I2C: ready" */
	puts("I2C:   ");
	i2c_init_all();
	puts("ready\n");
	return 0;
}
#endif

static int setup_mon_len(void)
{
	/* 计算 gd->mon_len：U-Boot 监视器(代码 + 数据 + BSS)的总长度，
	 * 后续 reserve_uboot 用它从 RAM 顶向下预留自身 relocate 空间。 */
#if defined(CONFIG_ARCH_NEXELL)
	gd->mon_len = (ulong)__bss_end - (ulong)__image_copy_start;
#elif defined(__ARM__) || defined(__MICROBLAZE__)
	gd->mon_len = (ulong)__bss_end - (ulong)_start;
#elif defined(CONFIG_SANDBOX) && !defined(__riscv)
	gd->mon_len = (ulong)_end - (ulong)_init;
#elif defined(CONFIG_SANDBOX)
	/* gcc does not provide _init in crti.o on RISC-V */
	gd->mon_len = 0;
#elif defined(CONFIG_EFI_APP)
	gd->mon_len = (ulong)_end - (ulong)_init;
#elif defined(CONFIG_NIOS2) || defined(CONFIG_XTENSA)
	gd->mon_len = CONFIG_SYS_MONITOR_LEN;
#elif defined(CONFIG_SH) || defined(CONFIG_RISCV)
	gd->mon_len = (ulong)(__bss_end) - (ulong)(_start);
#elif defined(CONFIG_SYS_MONITOR_BASE)
	/* TODO: use (ulong)__bss_end - (ulong)__text_start; ? */
	gd->mon_len = (ulong)__bss_end - CONFIG_SYS_MONITOR_BASE;
#endif
	return 0;
}

static int setup_spl_handoff(void)
{
	/* 若存在 SPL handoff bloblist，取出并保存到 gd->spl_handoff 供后续阶段使用 */
#if CONFIG_IS_ENABLED(HANDOFF)
	gd->spl_handoff = bloblist_find(BLOBLISTT_U_BOOT_SPL_HANDOFF,
					sizeof(struct spl_handoff));
	debug("Found SPL hand-off info %p\n", gd->spl_handoff);
#endif

	return 0;
}

__weak int arch_cpu_init(void)
{
	/* 弱符号：架构相关 CPU 基础初始化(默认空，可由 SoC/板级代码覆盖) */
	return 0;
}

__weak int mach_cpu_init(void)
{
	/* 弱符号：SoC/机器相关 CPU 初始化(默认空) */
	return 0;
}

/* Get the top of usable RAM */
__weak phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	/* 弱符号：返回可用 RAM 顶地址。用于裁剪 32 位地址空间溢出等情况，
	 * 同时确保不会把预留做到 32 位回绕后的错误位置。 */
	if (CONFIG_IS_ENABLED(RELOC_ADDR_TOP))
		return gd->ram_top;

#if defined(CFG_SYS_SDRAM_BASE) && CFG_SYS_SDRAM_BASE > 0
	/*
	 * Detect whether we have so much RAM that it goes past the end of our
	 * 32-bit address space. If so, clip the usable RAM so it doesn't.
	 */
	if (gd->ram_top < CFG_SYS_SDRAM_BASE)
		/*
		 * Will wrap back to top of 32-bit space when reservations
		 * are made.
		 */
		return 0;
#endif
	return gd->ram_top;
}

__weak int arch_setup_dest_addr(void)
{
	/* 弱符号：架构可在此调整 relocation 目标地址(默认空) */
	return 0;
}

static int setup_ram_base(void)
{
	/* 设置 gd->ram_base：SDRAM 的物理起始地址 */
#ifdef CFG_SYS_SDRAM_BASE
	gd->ram_base = CFG_SYS_SDRAM_BASE;
#endif
	return 0;
}

static int setup_ram_config(void)
{
	/* 计算 gd->ram_top 与 gd->ram_size：可用 RAM 顶部地址与总大小，
	 * 并处理 SYS_MEM_TOP_HIDE 以隐藏一部分内存(内核也不会触碰)。 */
	debug("Monitor len: %08x\n", gd->mon_len);

	if (CONFIG_IS_ENABLED(RELOC_ADDR_TOP)) {
		int i;
		phys_addr_t top;

		gd->ram_size = 0;
		for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
			top = get_mem_top(gd->dram[i].start, gd->dram[i].size,
					  ALIGN(gd->mon_len, SZ_1M),
					  (void *)gd->fdt_blob);
			gd->ram_top = max(top, gd->ram_top);
			gd->ram_size += gd->dram[i].size;
		}
	} else {
		gd->ram_top = gd->ram_base + get_effective_memsize();
	}
	gd->ram_top = board_get_usable_ram_top(gd->mon_len);
	/*
	 * Subtract specified amount of memory to hide so that it won't
	 * get "touched" at all by U-Boot. By fixing up gd->ram_size
	 * the Linux kernel should now get passed the now "corrected"
	 * memory size and won't touch it either. This should work
	 * for arch/ppc and arch/powerpc. Only Linux board ports in
	 * arch/powerpc with bootwrapper support, that recalculate the
	 * memory size from the SDRAM controller setup will have to
	 * get fixed.
	 */
#if CONFIG_VAL(SYS_MEM_TOP_HIDE)
	gd->ram_top -= CONFIG_SYS_MEM_TOP_HIDE;
	gd->ram_size -= CONFIG_SYS_MEM_TOP_HIDE;
#endif

	debug("Ram top: %08llx\n", (unsigned long long)gd->ram_top);
	debug("Ram size: %08llx\n", (unsigned long long)gd->ram_size);

	return 0;
}

static int setup_dest_addr(void)
{
	/* 设定 relocation 目标地址：gd->relocaddr = gd->ram_top，
	 * 并记录初始值 gd->initial_relocaddr */
	int ret;

	gd->relocaddr = gd->ram_top;
	debug("Reloc addr: %08llX\n", (unsigned long long)gd->relocaddr);

	ret = arch_setup_dest_addr();
	if (ret)
		return ret;

	gd->initial_relocaddr = gd->relocaddr;
	return 0;
}

#ifdef CFG_PRAM
/* reserve protected RAM */
static int reserve_pram(void)
{
	/* 从内存顶向下预留"受保护 RAM"(pram，单位 kB)：U-Boot 与 Linux 都不会触碰，
	 * 常用于存放不易失日志等。 */
	ulong reg;

	reg = env_get_ulong("pram", 10, CFG_PRAM);
	gd->relocaddr -= (reg << 10);		/* size is in kB */
	debug("Reserving %ldk for protected RAM at %08lx\n", reg,
	      gd->relocaddr);
	return 0;
}
#endif /* CFG_PRAM */

/* Round memory pointer down to next 4 kB limit */
static int reserve_round_4k(void)
{
	/* 把 relocaddr 向下对齐到 4KB，保证后续预留区域都满足对齐要求 */
	gd->relocaddr &= ~(4096 - 1);
	return 0;
}

__weak int arch_reserve_mmu(void)
{
	/* 弱符号：架构预留 MMU 页表所需空间(默认空) */
	return 0;
}

static int reserve_video_from_videoblob(void)
{
	/* 若上一阶段(SPL)通过 bloblist 传来了视频帧缓冲，则把 relocaddr 下移到该 fb 之前，
	 * 避免后续预留覆盖已被占用的显存。 */
	if (IS_ENABLED(CONFIG_SPL_VIDEO_HANDOFF) && xpl_phase() > PHASE_SPL) {
		struct video_handoff *ho;
		int ret = 0;

		ho = bloblist_find(BLOBLISTT_U_BOOT_VIDEO, sizeof(*ho));
		if (!ho)
			return log_msg_ret("Missing video bloblist", -ENOENT);

		ret = video_reserve_from_bloblist(ho);
		if (ret)
			return log_msg_ret("Invalid Video handoff info", ret);

		/* Sanity check fb from blob is before current relocaddr */
		if (likely(gd->relocaddr > (unsigned long)ho->fb))
			gd->relocaddr = ho->fb;
	}

	return 0;
}

/*
 * Check if any bloblist received specifying reserved areas from previous stage and adjust
 * gd->relocaddr accordingly, so that we start reserving after pre-reserved areas
 * from previous stage.
 *
 * NOTE:
 * IT is recommended that all bloblists from previous stage are reserved from ram_top
 * as next stage will simply start reserving further regions after them.
 */
static int setup_relocaddr_from_bloblist(void)
{
	/* 处理来自前一阶段(SPL)的 bloblist 预留区域，调整 relocaddr 起点，
	 * 使后续预留不会覆盖前面阶段已预留的区域。 */
	reserve_video_from_videoblob();

	return 0;
}

static int reserve_video(void)
{
	/* 预留 LCD/显示帧缓冲(VIDEO)空间，并把 relocaddr 下移到帧缓冲之前 */
	if (CONFIG_IS_ENABLED(VIDEO)) {
		ulong addr;
		int ret;

		addr = gd->relocaddr;
		ret = video_reserve(&addr);
		if (ret)
			return ret;
		debug("Reserving %luk for video at: %08lx\n",
		      ((unsigned long)gd->relocaddr - addr) >> 10, addr);
		gd->relocaddr = addr;
	}

	return 0;
}

static int reserve_trace(void)
{
	/* 预留 trace 数据缓冲区(CONFIG_TRACE 时) */
#ifdef CONFIG_TRACE
	gd->relocaddr -= CONFIG_TRACE_BUFFER_SIZE;
	gd->trace_buff = map_sysmem(gd->relocaddr, CONFIG_TRACE_BUFFER_SIZE);
	debug("Reserving %luk for trace data at: %08lx\n",
	      (unsigned long)CONFIG_TRACE_BUFFER_SIZE >> 10, gd->relocaddr);
#endif

	return 0;
}

static int reserve_uboot(void)
{
	/* 预留 U-Boot 自身(代码 + 数据 + BSS)的 relocation 空间，并记录 start_addr_sp。
	 * 这是 relocation 前最关键的一步：从 RAM 顶扣掉 mon_len 并 4KB 对齐。 */
	/*
	 * This should be the first place GD_FLG_SKIP_RELOC is read from.
	 * Set GD_FLG_SKIP_RELOC flag if CONFIG_SKIP_RELOCATE is enabled.
	 */
	if (CONFIG_IS_ENABLED(SKIP_RELOCATE))
		gd->flags |= GD_FLG_SKIP_RELOC;

	if (!(gd->flags & GD_FLG_SKIP_RELOC) && !CONFIG_IS_ENABLED(SKIP_RELOCATE_CODE)) {
		/*
		 * reserve memory for U-Boot code, data & bss
		 * round down to next 4 kB limit
		 */
		gd->relocaddr -= gd->mon_len;	/* 从顶向下扣掉 mon_len */
		gd->relocaddr &= ~(4096 - 1);	/* 4KB 对齐 */
#if defined(CONFIG_E500) || defined(CONFIG_MIPS)
		/* round down to next 64 kB limit so that IVPR stays aligned */
		gd->relocaddr &= ~(65536 - 1);
#endif

		debug("Reserving %dk for U-Boot at: %08lx\n",
		      gd->mon_len >> 10, gd->relocaddr);
	}

	gd->start_addr_sp = gd->relocaddr;	/* 当前 relocaddr 作为新栈底起点(后续从它向下继续预留) */

	return 0;
}

/*
 * reserve after start_addr_sp the requested size and make the stack pointer
 * 16-byte aligned, this alignment is needed for cast on the reserved memory
 * ref = x86_64 ABI: https://reviews.llvm.org/D30049: 16 bytes
 *     = ARMv8 Instruction Set Overview: quad word, 16 bytes
 */
static unsigned long reserve_stack_aligned(size_t size)
{
	/* 工具函数：从 start_addr_sp 向下预留 size 字节并 16 字节对齐，返回新 sp 地址 */
	return ALIGN_DOWN(gd->start_addr_sp - size, 16);
}

#ifdef CONFIG_SYS_NONCACHED_MEMORY
static int reserve_noncached(void)
{
	/* 预留 noncached(不可缓存)内存区域，供 noncached_alloc() 使用。
	 * 注意：这里算出的 start_addr_sp 必须与 board_r.c:initr_malloc() 中
	 * 计算的 mem_malloc_start 一致。 */
	/*
	 * The value of gd->start_addr_sp must match the value of
	 * mem_malloc_start calculated in board_r.c:initr_malloc(), which is
	 * passed to dlmalloc.c:mem_malloc_init() and then used by
	 * cache.c:noncached_init()
	 *
	 * These calculations must match the code in cache.c:noncached_init()
	 */
	gd->start_addr_sp = ALIGN(gd->start_addr_sp, MMU_SECTION_SIZE) -
		MMU_SECTION_SIZE;
	gd->start_addr_sp -= ALIGN(CONFIG_SYS_NONCACHED_MEMORY,
				   MMU_SECTION_SIZE);
	debug("Reserving %dM for noncached_alloc() at: %08lx\n",
	      CONFIG_SYS_NONCACHED_MEMORY >> 20, gd->start_addr_sp);

	return 0;
}
#endif

/* reserve memory for malloc() area */
static int reserve_malloc(void)
{
	/* 预留 malloc 堆(TOTAL_MALLOC_LEN)，并连带预留 noncached 区域 */
	gd->start_addr_sp = reserve_stack_aligned(TOTAL_MALLOC_LEN);
	debug("Reserving %dk for malloc() at: %08lx\n",
	      TOTAL_MALLOC_LEN >> 10, gd->start_addr_sp);
#ifdef CONFIG_SYS_NONCACHED_MEMORY
	reserve_noncached();
#endif

	return 0;
}

/* (permanently) allocate a Board Info struct */
static int reserve_board(void)
{
	/* 预留并清零 bd_info(board info)结构，保存到 gd->bd */
	if (!gd->bd) {
		gd->start_addr_sp = reserve_stack_aligned(sizeof(struct bd_info));
		gd->bd = (struct bd_info *)map_sysmem(gd->start_addr_sp,
						      sizeof(struct bd_info));
		/* 注意：此处用字符字面量 '\0' 作为 memset 的填充值(会隐式提升为 int 0)，
		   功能上等价于清零；常见写法为 memset(gd->bd, 0, ...)，这里保留原样不作改动 */
		memset(gd->bd, '\0', sizeof(struct bd_info));
		debug("Reserving %zu Bytes for Board Info at: %08lx\n",
		      sizeof(struct bd_info), gd->start_addr_sp);
	}
	return 0;
}

static int reserve_global_data(void)
{
	/* 预留新的 gd(global data)结构，保存到 gd->new_gd，供 relocation 后使用 */
	gd->start_addr_sp = reserve_stack_aligned(sizeof(gd_t));
	gd->new_gd = (gd_t *)map_sysmem(gd->start_addr_sp, sizeof(gd_t));
	debug("Reserving %zu Bytes for Global Data at: %08lx\n",
	      sizeof(gd_t), gd->start_addr_sp);
	return 0;
}

static int reserve_fdt(void)
{
	/* 预留 FDT(device tree / DTB) relocate 目标空间(非 OF_EMBED 时) */
	if (!IS_ENABLED(CONFIG_OF_EMBED)) {
		/*
		 * If the device tree is sitting immediately above our image
		 * then we must relocate it. If it is embedded in the data
		 * section, then it will be relocated with other data.
		 */
		if (gd->fdt_blob) {
			gd->boardf->fdt_size =
				ALIGN(fdt_totalsize(gd->fdt_blob), 32);

			gd->start_addr_sp = reserve_stack_aligned(
				gd->boardf->fdt_size);
			gd->boardf->new_fdt = map_sysmem(gd->start_addr_sp,
							 gd->boardf->fdt_size);
			debug("Reserving %lu Bytes for FDT at: %08lx\n",
			      gd->boardf->fdt_size, gd->start_addr_sp);
		}
	}

	return 0;
}

static int reserve_bootstage(void)
{
	/* 预留 bootstage 启动计时数据的 relocate 空间(开启 CONFIG_BOOTSTAGE 时) */
#ifdef CONFIG_BOOTSTAGE
	int size = bootstage_get_size(true);

	gd->start_addr_sp = reserve_stack_aligned(size);
	gd->boardf->new_bootstage = map_sysmem(gd->start_addr_sp, size);
	debug("Reserving %#x Bytes for bootstage at: %08lx\n", size,
	      gd->start_addr_sp);
#endif

	return 0;
}

__weak int arch_reserve_stacks(void)
{
	/* 弱符号：架构预留栈 / IRQ 栈区域(如 ARM 在此预留 IRQ stack) */
	return 0;
}

static int reserve_stacks(void)
{
	/* 预留 16 字节对齐的最终栈空间，并交给架构代码调整 irq_sp 等 */
	/* make stack pointer 16-byte aligned */
	gd->start_addr_sp = reserve_stack_aligned(16);

	/*
	 * let the architecture-specific code tailor gd->start_addr_sp and
	 * gd->irq_sp
	 */
	return arch_reserve_stacks();
}

static int reserve_bloblist(void)
{
	/* 预留 bloblist(跨阶段传递数据的链表)的 relocate 空间，4KB 对齐 */
#ifdef CONFIG_BLOBLIST
	ulong size = bloblist_get_total_size();

	if (size < CONFIG_BLOBLIST_SIZE_RELOC)
		size = CONFIG_BLOBLIST_SIZE_RELOC;

	/* Align to a 4KB boundary for easier reading of addresses */
	gd->start_addr_sp = ALIGN_DOWN(gd->start_addr_sp - size, 0x1000);
	gd->boardf->new_bloblist = map_sysmem(gd->start_addr_sp, size);
#endif

	return 0;
}

static int display_new_sp(void)
{
	/* 调试打印 relocation 后的新栈指针位置 */
	debug("New Stack Pointer is: %08lx\n", gd->start_addr_sp);

	return 0;
}

__weak int arch_setup_bdinfo(void)
{
	/* 弱符号：架构可在此填充 bd_info 的架构相关字段(默认空) */
	return 0;
}

int setup_bdinfo(void)
{
	/* 调用 arch_setup_bdinfo() 完成 bd_info 初始化(架构相关) */
	return arch_setup_bdinfo();
}

#ifdef CONFIG_POST
static int init_post(void)
{
	/* 上电自检(POST)：运行 ROM 阶段的 POST 测试项 */
	post_bootmode_init();
	post_run(NULL, POST_ROM | post_bootmode_get(0));

	return 0;
}
#endif

static int reloc_fdt(void)
{
	/* 把 FDT 从旧位置拷贝到 reserve_fdt 预留的新位置，并更新 gd->fdt_blob 指向新副本 */
	if (!IS_ENABLED(CONFIG_OF_EMBED)) {
		if (gd->boardf->new_fdt) {
			memcpy(gd->boardf->new_fdt, gd->fdt_blob,
			       fdt_totalsize(gd->fdt_blob));
			gd->fdt_blob = gd->boardf->new_fdt;
		}
	}

	return 0;
}

static int reloc_bootstage(void)
{
	/* 把 bootstage 数据 relocate 到新位置(跳过 relocation 时直接返回) */
#ifdef CONFIG_BOOTSTAGE
	if (gd->flags & GD_FLG_SKIP_RELOC)
		return 0;
	if (gd->boardf->new_bootstage)
		bootstage_relocate(gd->boardf->new_bootstage);
#endif

	return 0;
}

static int reloc_bloblist(void)
{
	/* 把 bloblist 从旧位置迁移到新位置(若需要且大小匹配) */
#ifdef CONFIG_BLOBLIST
	/*
	 * Relocate only if we are supposed to send it
	 */
	if ((gd->flags & GD_FLG_SKIP_RELOC) &&
	    CONFIG_BLOBLIST_SIZE == CONFIG_BLOBLIST_SIZE_RELOC) {
		debug("Not relocating bloblist\n");
		return 0;
	}
	if (gd->boardf->new_bloblist) {
		ulong size = bloblist_get_total_size();

		if (size < CONFIG_BLOBLIST_SIZE_RELOC)
			size = CONFIG_BLOBLIST_SIZE_RELOC;

		debug("Copying bloblist from %p to %p, size %lx\n",
		      gd->bloblist, gd->boardf->new_bloblist, size);
		return bloblist_reloc(gd->boardf->new_bloblist, size);
	}
#endif

	return 0;
}

void mcheck_on_ramrelocation(size_t offset);
static int setup_reloc(void)
{
	/* 计算 relocation 偏移 gd->reloc_off，并把当前 gd 拷贝到 new_gd。
	 * reloc_off = 新地址 - 链接地址，之后所有绝对符号都需加上该偏移。 */
	if (!(gd->flags & GD_FLG_SKIP_RELOC)) {
#ifdef CONFIG_TEXT_BASE
#ifdef ARM
		gd->reloc_off = gd->relocaddr - (unsigned long)__image_copy_start;
#elif defined(CONFIG_MICROBLAZE)
		gd->reloc_off = gd->relocaddr - (u32)_start;
#elif defined(CONFIG_M68K)
		/*
		 * On all ColdFire arch cpu, monitor code starts always
		 * just after the default vector table location, so at 0x400
		 */
		gd->reloc_off = gd->relocaddr - (CONFIG_TEXT_BASE + 0x400);
#elif !defined(CONFIG_SANDBOX)
		gd->reloc_off = gd->relocaddr - CONFIG_TEXT_BASE;
#endif
#endif
	}

	memcpy(gd->new_gd, (char *)gd, sizeof(gd_t));	/* 把 gd 整体复制到 relocate 后的新位置 */

	if (gd->flags & GD_FLG_SKIP_RELOC) {
		debug("Skipping relocation due to flag\n");
	} else {
#ifdef MCHECK_HEAP_PROTECTION
		mcheck_on_ramrelocation(gd->reloc_off);
#endif
		debug("Relocation Offset is: %08lx\n", gd->reloc_off);
		debug("Relocating to %08lx, new gd at %08lx, sp at %08lx\n",
		      gd->relocaddr, (ulong)map_to_sysmem(gd->new_gd),
		      gd->start_addr_sp);
	}

	return 0;
}

#if CONFIG_IS_ENABLED(OF_BOARD_FIXUP)
static int fix_fdt(void)
{
	/* 调用板级 fix_fdt 钩子，对设备树做最后修正(OF_BOARD_FIXUP 时) */
	return board_fix_fdt((void *)gd->fdt_blob);
}
#endif

/* ARM calls relocate_code from its crt0.S */
#if !defined(CONFIG_ARM) && !defined(CONFIG_SANDBOX)

static int jump_to_copy(void)
{
	/* 非 ARM 架构：把 U-Boot 自身搬到 RAM(relocate_code)，随后跳到新地址运行。
	 * (ARM 由 crt0.S 完成 relocation，不走到这里。) */
	if (gd->flags & GD_FLG_SKIP_RELOC)
		return 0;
	/*
	 * x86 is special, but in a nice way. It uses a trampoline which
	 * enables the dcache if possible.
	 *
	 * For now, other archs use relocate_code(), which is implemented
	 * similarly for all archs. When we do generic relocation, hopefully
	 * we can make all archs enable the dcache prior to relocation.
	 */
#if defined(CONFIG_X86) || defined(CONFIG_ARC)
	/*
	 * SDRAM and console are now initialised. The final stack can now
	 * be setup in SDRAM. Code execution will continue in Flash, but
	 * with the stack in SDRAM and Global Data in temporary memory
	 * (CPU cache)
	 */
	arch_setup_gd(gd->new_gd);
# if CONFIG_IS_ENABLED(X86_64)
		board_init_f_r_trampoline64(gd->new_gd, gd->start_addr_sp);
# else
		board_init_f_r_trampoline(gd->start_addr_sp);
# endif
#else
	relocate_code(gd->start_addr_sp, gd->new_gd, gd->relocaddr);	/* 实际搬移代码并跳转到 RAM 中新地址 */
#endif

	return 0;
}
#endif

/* Record the board_init_f() bootstage (after arch_cpu_init()) */
static int initf_bootstage(void)
{
	/* 初始化 bootstage 启动计时框架，并标记 board_init_f 阶段已到达 */
	bool from_spl = IS_ENABLED(CONFIG_SPL_BOOTSTAGE) &&
			IS_ENABLED(CONFIG_BOOTSTAGE_STASH);
	int ret;

	ret = bootstage_init(!from_spl);
	if (ret)
		return ret;
	if (from_spl) {
		ret = bootstage_unstash_default();
		if (ret && ret != -ENOENT) {
			debug("Failed to unstash bootstage: err=%d\n", ret);
			return ret;
		}
	}

	bootstage_mark_name(BOOTSTAGE_ID_START_UBOOT_F, "board_init_f");

	return 0;
}

static int initf_dm(void)
{
	/* 早期设备模型(DM)初始化：扫描设备树并 probe 早期设备(如串口)，
	 * 为后续 serial_init / console_init_f 提供设备支持。 */
	int ret;

	if (!CONFIG_IS_ENABLED(SYS_MALLOC_F))
		return 0;	/* 没有 early malloc 则无法初始化 DM，直接返回 */

	bootstage_start(BOOTSTAGE_ID_ACCUM_DM_F, "dm_f");

	/*
	 * If SKIP_EARLY_DM is set then we just create an empty device
	 * model, the serial port will still be bound later through
	 * serial_find_console_or_panic() via /chosen/stdout-path
	 */
	if (!CONFIG_IS_ENABLED(SKIP_EARLY_DM))
		ret = dm_init_and_scan(true);	/* 初始化 DM 并扫描全部早期设备 */
	else
		ret = dm_init(false);	/* 仅创建空 DM 根，稍后由串口绑定 */
	if (ret)
		return ret;

	ret = dm_autoprobe();
	if (ret)
		return ret;
	bootstage_accum(BOOTSTAGE_ID_ACCUM_DM_F);

	if (IS_ENABLED(CONFIG_TIMER_EARLY)) {
		ret = dm_timer_init();
		if (ret)
			return ret;
	}

	return 0;
}

/* Architecture-specific memory reservation */
__weak int reserve_arch(void)
{
	/* 弱符号：架构特定的内存预留(默认空) */
	return 0;
}

__weak int checkcpu(void)
{
	/* 弱符号：检查 / 打印 CPU 信息(默认空) */
	return 0;
}

__weak int clear_bss(void)
{
	/* 弱符号：清 BSS。多数架构在 crt0 中完成，这里默认空。 */
	return 0;
}

static int initf_upl(void)
{
	/* 若启用了 UPL(Universal Payload)且由前一阶段设置了 GD_FLG_UPL，
	 * 读取并保存 UPL handoff 数据到 gd。 */
	struct upl *upl;
	int ret;

	if (!IS_ENABLED(CONFIG_UPL_IN) || !(gd->flags & GD_FLG_UPL))
		return 0;

	upl = malloc(sizeof(struct upl));
	if (upl)
		ret = upl_read_handoff(upl, oftree_default());
	if (ret) {
		printf("UPL handoff: read failure (err=%dE)\n", ret);
		return ret;
	}
	gd_set_upl(upl);

	return 0;
}

static void initcall_run_f(void)
{
	/*
	 * board_init_f 的核心：按固定顺序调用全部 INITCALL。
	 * 顺序非常关键，不得随意调整：
	 *  1) 先建立 early malloc / DM / 串口 / 控制台 / 环境；
	 *  2) 打印版本信息、CPU 信息；
	 *  3) announce_dram_init + dram_init 探测 DRAM；
	 *  4) setup_ram_* 确定 RAM 顶/总大小与 relocaddr；
	 *  5) 从 RAM 顶向下依次预留 pram/video/trace/uboot/malloc/bd/gd/fdt/
	 *     bootstage/bloblist/stack 等区域；
	 *  6) reloc_fdt / reloc_bootstage / reloc_bloblist / setup_reloc 完成数据搬迁与偏移计算；
	 *  7) 非 ARM 架构最后通过 jump_to_copy 完成自身搬迁。
	 * 注意：ARM 架构不调用 jump_to_copy(由 crt0.S 完成 relocation 后进入 board_init_r)。
	 */
	INITCALL(setup_mon_len);
#if CONFIG_IS_ENABLED(OF_CONTROL)
	INITCALL(fdtdec_setup);
#endif
#if CONFIG_IS_ENABLED(TRACE_EARLY)
	INITCALL(trace_early_init);
#endif
	INITCALL(initf_malloc);
	INITCALL(initf_upl);
	INITCALL(log_init);
	INITCALL(initf_bootstage); /* uses its own timer, so does not need DM */
	INITCALL(event_init);
#if CONFIG_IS_ENABLED(BLOBLIST)
	INITCALL(bloblist_init);
#endif
	INITCALL(setup_spl_handoff);
#if CONFIG_IS_ENABLED(CONSOLE_RECORD_INIT_F)
	INITCALL(console_record_init);
#endif
	INITCALL_EVT(EVT_FSP_INIT_F);
	INITCALL(arch_cpu_init);	/* basic arch cpu dependent setup */
	INITCALL(mach_cpu_init);	/* SoC/machine dependent CPU setup */
	INITCALL(initf_dm);
#if CONFIG_IS_ENABLED(BOARD_EARLY_INIT_F)
	INITCALL(board_early_init_f);
#endif
#if defined(CONFIG_PPC) || defined(CONFIG_SYS_FSL_CLK) || defined(CONFIG_M68K)
	/* get CPU and bus clocks according to the environment variable */
	INITCALL(get_clocks);		/* get CPU and bus clocks (etc.) */
#endif
#if !defined(CONFIG_M68K) || (defined(CONFIG_M68K) && !defined(CONFIG_MCFTMR))
	INITCALL(timer_init);		/* initialize timer */
#endif
#if CONFIG_IS_ENABLED(BOARD_POSTCLK_INIT)
	INITCALL(board_postclk_init);
#endif
	INITCALL(env_init);		/* initialize environment */
	INITCALL(init_baud_rate);	/* initialze baudrate settings */
	INITCALL(serial_init);		/* serial communications setup */
	INITCALL(console_init_f);	/* stage 1 init of console */
	INITCALL(display_options);	/* say that we are here */
	INITCALL(display_text_info);	/* show debugging info if required */
	INITCALL(checkcpu);
#if CONFIG_IS_ENABLED(SYSRESET)
	INITCALL(print_resetinfo);
#endif
	/* display cpu info (and speed) */
#if CONFIG_IS_ENABLED(DISPLAY_CPUINFO)
	INITCALL(print_cpuinfo);
#endif
#if CONFIG_IS_ENABLED(DTB_RESELECT)
	INITCALL(embedded_dtb_select);
#endif
#if CONFIG_IS_ENABLED(DISPLAY_BOARDINFO)
	INITCALL(show_board_info);
#endif
	WATCHDOG_INIT();
	INITCALL_EVT(EVT_MISC_INIT_F);
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(SYS_I2C_LEGACY)
	INITCALL(init_func_i2c);
#endif
	INITCALL(announce_dram_init);
	INITCALL(dram_init);		/* configure available RAM banks */
#if CONFIG_IS_ENABLED(POST)
	INITCALL(post_init_f);
#endif
	WATCHDOG_RESET();
#if defined(CFG_SYS_DRAM_TEST)
	INITCALL(testdram);
#endif /* CFG_SYS_DRAM_TEST */
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(POST)
	INITCALL(init_post);
#endif
	WATCHDOG_RESET();
	/*
	 * Now that we have DRAM mapped and working, we can
	 * relocate the code and continue running from DRAM.
	 *
	 * Reserve memory at end of RAM for (top down in that order):
	 *  - area that won't get touched by U-Boot and Linux (optional)
	 *  - kernel log buffer
	 *  - protected RAM
	 *  - LCD framebuffer
	 *  - monitor code
	 *  - board info struct
	 */
	INITCALL(setup_ram_base);
	INITCALL(dram_init_banksize);
	INITCALL(setup_ram_config);
	INITCALL(setup_dest_addr);
#if CONFIG_IS_ENABLED(OF_BOARD_FIXUP) && \
    !CONFIG_IS_ENABLED(OF_INITIAL_DTB_READONLY)
	INITCALL(fix_fdt);
#endif
#ifdef CFG_PRAM
	INITCALL(reserve_pram);
#endif
	INITCALL(reserve_round_4k);
	INITCALL(setup_relocaddr_from_bloblist);
	INITCALL(arch_reserve_mmu);
	INITCALL(reserve_video);
	INITCALL(reserve_trace);
	INITCALL(reserve_uboot);
	INITCALL(reserve_malloc);
	INITCALL(reserve_board);
	INITCALL(reserve_global_data);
	INITCALL(reserve_fdt);
#if CONFIG_IS_ENABLED(OF_BOARD_FIXUP) && \
    CONFIG_IS_ENABLED(OF_INITIAL_DTB_READONLY)
	INITCALL(reloc_fdt);
	INITCALL(fix_fdt);
#endif
	INITCALL(reserve_bootstage);
	INITCALL(reserve_bloblist);
	INITCALL(reserve_arch);
	INITCALL(reserve_stacks);
	INITCALL(show_dram_config);
	WATCHDOG_RESET();
	INITCALL(setup_bdinfo);
	INITCALL(display_new_sp);
	WATCHDOG_RESET();
#if !CONFIG_IS_ENABLED(OF_BOARD_FIXUP) || \
    !CONFIG_IS_ENABLED(INITIAL_DTB_READONLY)
	INITCALL(reloc_fdt);
#endif
	INITCALL(reloc_bootstage);
	INITCALL(reloc_bloblist);
	INITCALL(setup_reloc);
#if CONFIG_IS_ENABLED(X86) || CONFIG_IS_ENABLED(ARC)
	INITCALL(copy_uboot_to_ram);
	INITCALL(do_elf_reloc_fixups);
#endif
	INITCALL(clear_bss);
	/*
	 * Deregister all cyclic functions before relocation, so that
	 * gd->cyclic_list does not contain any references to pre-relocation
	 * devices. Drivers will register their cyclic functions anew when the
	 * devices are probed again.
	 *
	 * This should happen as late as possible so that the window where a
	 * watchdog device is not serviced is as small as possible.
	 */
	INITCALL(cyclic_unregister_all);
#if !CONFIG_IS_ENABLED(ARM) && !CONFIG_IS_ENABLED(SANDBOX)
	INITCALL(jump_to_copy);
#endif
}

static int show_memory_layout(void)
{
	/* 调试辅助函数：打印当前内存布局关键信息(GD 地址、RAM 基址/大小、
	 * 栈、malloc 区、reloc 地址等)。由用户添加，用于排查 relocation 内存规划。 */
	// printf("\n=== Memory Layout ===\n");
	// printf("  TEXT_BASE:    0x%08lx\n", (ulong)CONFIG_SYS_TEXT_BASE);
	// printf("  RAM Start:    0x%08lx\n", (ulong)CONFIG_SYS_SDRAM_BASE);
	// printf("  RAM Size:     0x%08lx\n", (ulong)CONFIG_SYS_SDRAM_SIZE);
	// printf("  Stack:        0x%08lx\n", (ulong)gd->start_addr_sp);
	// printf("  Malloc Area:  0x%08lx - 0x%08lx\n",
	//        (ulong)gd->malloc_base,
	//        (ulong)(gd->malloc_base + CONFIG_SYS_MALLOC_LEN));
	// printf("======================\n\n");
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

void board_init_f(ulong boot_flags)
{
	/* relocation 之前的"第一阶段"初始化总入口(ARM 上由 crt0.S 的 _main 调用)。
	 * 职责：清控制台标志、建立 board_f 临时结构、调用 initcall_run_f() 完成早期初始化
	 * 与内存预留；在 ARM 上 initcall_run_f 不搬迁自身(由 crt0.S 完成 relocation)，
	 * 之后直接走到下面的 show_memory_layout(); 非 ARM 架构则 jump_to_copy 不返回。
	 * 参数 boot_flags：由 _main 传入的初始 gd->flags。 */

	printf("[TRACE] board_init_f entered\n");

	struct board_f boardf;

	gd->flags = boot_flags;
	gd->flags &= ~GD_FLG_HAVE_CONSOLE;
	gd->boardf = &boardf;

	initcall_run_f();

#if !defined(CONFIG_ARM) && !defined(CONFIG_SANDBOX) && \
		!defined(CONFIG_EFI_APP) && !CONFIG_IS_ENABLED(X86_64) && \
		!defined(CONFIG_ARC)
	/* NOTREACHED - jump_to_copy() does not return */
	hang();
#endif

show_memory_layout();

}

#if defined(CONFIG_X86) || defined(CONFIG_ARC)
/*
 * For now this code is only used on x86.
 *
 * Run init functions which are run when U-Boot is executing from Flash with a
 * semi-limited 'C' environment.
 * The following limitations must be considered when implementing an
 * '_f_r' function:
 *  - 'static' variables are read-only
 *  - Global Data (gd->xxx) is read/write
 *
 * The '_f_r' sequence must, as a minimum, copy U-Boot to RAM (if
 * supported).  It _should_, if possible, copy global data to RAM and
 * initialise the CPU caches (to speed up the relocation process)
 *
 * NOTE: At present only x86 uses this route, but it is intended that
 * all archs will move to this when generic relocation is implemented.
 */
static void initcall_run_f_r(void)
{
	/* x86/ARC 专用：从 Flash 执行到 RAM 之间的过渡期初始化(目前仅 init_cache_f_r) */
#if !CONFIG_IS_ENABLED(X86_64)
	INITCALL(init_cache_f_r);
#endif
}

void board_init_f_r(void)
{
	/* x86/ARC 专用：Flash 中的最后一步。完成过渡初始化、标记串口/日志不可用，
	 * 然后把执行权转到 RAM 中 board_init_r() 的 relocate 后副本。 */
	initcall_run_f_r();

	/*
	 * The pre-relocation drivers may be using memory that has now gone
	 * away. Mark serial as unavailable - this will fall back to the debug
	 * UART if available.
	 *
	 * Do the same with log drivers since the memory may not be available.
	 */
	gd->flags &= ~(GD_FLG_SERIAL_READY | GD_FLG_LOG_READY);
#ifdef CONFIG_TIMER
	gd->timer = NULL;
#endif

	/*
	 * U-Boot has been copied into SDRAM, the BSS has been cleared etc.
	 * Transfer execution from Flash to RAM by calculating the address
	 * of the in-RAM copy of board_init_r() and calling it
	 */
	(board_init_r + gd->reloc_off)((gd_t *)gd, gd->relocaddr);

	/* NOTREACHED - board_init_r() does not return */
	hang();
}
#endif /* CONFIG_X86 */
