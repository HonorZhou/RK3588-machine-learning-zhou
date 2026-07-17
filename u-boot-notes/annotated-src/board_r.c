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
#include <api.h>
#include <bootstage.h>
#include <cpu_func.h>
#include <cyclic.h>
#include <display_options.h>
#include <exports.h>
#ifdef CONFIG_MTD_NOR_FLASH
#include <flash.h>
#endif
#include <hang.h>
#include <image.h>
#include <irq_func.h>
#include <lmb.h>
#include <log.h>
#include <net.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <u-boot/crc.h>
#include <binman.h>
#include <command.h>
#include <console.h>
#include <dm.h>
#include <efi_loader.h>
#include <env.h>
#include <env_internal.h>
#include <fdtdec.h>
#include <init.h>
#include <initcall.h>
#include <kgdb.h>
#include <irq_func.h>
#include <led.h>
#include <malloc.h>
#include <mapmem.h>
#include <miiphy.h>
#include <mmc.h>
#include <mux.h>
#include <nand.h>
#include <of_live.h>
#include <onenand_uboot.h>
#include <pvblock.h>
#include <scsi.h>
#include <serial.h>
#include <stdio_dev.h>
#include <timer.h>
#include <trace.h>
#include <watchdog.h>
#include <xen.h>
#include <asm/sections.h>
#include <dm/root.h>
#include <dm/ofnode.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <wdt.h>
#include <asm-generic/gpio.h>
#include <relocate.h>

/*
 * =========================================================================
 * board_r.c —— U-Boot relocation(重定位)之后的"第二阶段"初始化
 * -------------------------------------------------------------------------
 * 本文件实现 board_init_r()，在 U-Boot 自身已被搬到 RAM 顶部、gd 也复制到
 * 新位置后由 crt0.S(x86 由 board_init_f_r)调用。它唯一的核心是按固定顺序
 * 调用 initcall_run_r() 里的一长串 INITCALL，完成：
 *   - 标记 relocation 完成、开启 Cache、重映射全局数据(reloc_global_data)；
 *   - 初始化完整 malloc、设备模型(DM)全量扫描、OF_LIVE 设备树；
 *   - 板级/架构初始化(board_init)、串口控制台终态、环境重定位；
 *   - 各类存储(NOR/NAND/ONENAND/MMC)、网络、LED、看门狗等外设初始化；
 *   - 最后调用 run_main_loop() -> main_loop() 进入命令/启动主循环。
 * 关键全局变量：gd(已 relocate 到 RAM 顶附近的新副本)、bd(board info)。
 * =========================================================================
 */

DECLARE_GLOBAL_DATA_PTR;

ulong monitor_flash_len;

__weak int board_flash_wp_on(void)
{
	/* 弱符号：返回 1 表示 flash 处于写保护(默认不保护，返回 0)。
	 * 用于让 U-Boot 优雅跳过写保护的 flash 设备。 */
	/*
	 * Most flashes can't be detected when write protection is enabled,
	 * so provide a way to let U-Boot gracefully ignore write protected
	 * devices.
	 */
	return 0;
}

__weak int cpu_secondary_init_r(void)
{
	/* 弱符号：从核(slave)启动后的运行时初始化(默认空) */
	return 0;
}

static int initr_trace(void)
{
	/* 初始化 trace 子系统(开启 CONFIG_TRACE 时，使用 relocate 后的 trace 缓冲区) */
#ifdef CONFIG_TRACE
	trace_init(gd->trace_buff, CONFIG_TRACE_BUFFER_SIZE);
#endif

	return 0;
}

static int initr_reloc(void)
{
	/* 标记 relocation 完成：置 GD_FLG_RELOC 与 GD_FLG_FULL_MALLOC_INIT 标志。
	 * 此后完整 malloc 可用，代码已在 RAM 中运行。 */
	/* tell others: relocation done */
	gd->flags |= GD_FLG_RELOC | GD_FLG_FULL_MALLOC_INIT;

	return 0;
}

#if defined(CONFIG_ARM) || defined(CONFIG_RISCV)
/*
 * Some of these functions are needed purely because the functions they
 * call return void. If we change them to return 0, these stubs can go away.
 */
static int initr_caches(void)
{
	/* 开启指令/数据 Cache(ARM/RISC-V)。enable_caches() 内部会按需使能 MMU。 */
	/* Enable caches */
	enable_caches();
	return 0;
}
#endif

__weak int fixup_cpu(void)
{
	/* 弱符号：relocation 后修正 CPU 相关指针(默认空，PPC 用于更新 gd->arch.cpu) */
	return 0;
}

static int initr_reloc_global_data(void)
{
	/* relocation 后重映射/修正全局数据：计算 monitor_flash_len、迁移 env 地址、
	 * 处理 OF_EMBED 的 fdt_blob、EFI 运行时重定位，并设置各内存段的读写权限。 */
#ifdef __ARM__
	monitor_flash_len = _end - __image_copy_start;
#elif defined(CONFIG_RISCV)
	monitor_flash_len = (ulong)_end - (ulong)_start;
#elif !defined(CONFIG_SANDBOX) && !defined(CONFIG_NIOS2)
	monitor_flash_len = (ulong)__init_end - gd->relocaddr;
#endif
#if defined(CONFIG_MPC85xx) || defined(CONFIG_MPC86xx)
	/*
	 * The gd->cpu pointer is set to an address in flash before relocation.
	 * We need to update it to point to the same CPU entry in RAM.
	 * TODO: why not just add gd->reloc_ofs?
	 */
	gd->arch.cpu += gd->relocaddr - CONFIG_SYS_MONITOR_BASE;	/* 把 flash 中的 cpu 指针修正到 RAM 位置 */

	/*
	 * If we didn't know the cpu mask & # cores, we can save them of
	 * now rather than 'computing' them constantly
	 */
	fixup_cpu();
#endif
#ifdef CONFIG_ENV_RELOC_GD_ENV_ADDR
	/*
	 * Relocate the early env_addr pointer unless we know it is not inside
	 * the binary. Some systems need this and for the rest, it doesn't hurt.
	 */
	gd->env_addr += gd->reloc_off;	/* 把环境地址加上 reloc_off 迁移到 RAM */
#endif

	/*
	 * For CONFIG_OF_EMBED case the FDT is embedded into ELF, available by
	 * __dtb_dt_begin. After U-Boot ELF self-relocation to RAM top address
	 * it is worth to update fdt_blob in global_data
	 */
	if (IS_ENABLED(CONFIG_OF_EMBED))
		fdtdec_setup_embed();	/* 内嵌设备树时，更新 gd->fdt_blob 指向 relocate 后的副本 */

#ifdef CONFIG_EFI_LOADER
	/*
	 * On the ARM architecture gd is mapped to a fixed register (r9 or x18).
	 * As this register may be overwritten by an EFI payload we save it here
	 * and restore it on every callback entered.
	 */
	efi_save_gd();	/* 保存 gd(EFI 负载可能破坏固定寄存器) */

	if (!(gd->flags & GD_FLG_SKIP_RELOC))
		efi_runtime_relocate(gd->relocaddr, NULL);	/* EFI 运行时服务重定位 */

#endif
	/*
	 * We are done with all relocations change the permissions of the binary
	 * NOTE: __start_rodata etc are defined in arm64 linker scripts and
	 * sections.h. If you want to add support for your platform you need to
	 * add the symbols on your linker script, otherwise they will point to
	 * random addresses.
	 *
	 */
	if (IS_ENABLED(CONFIG_MMU_PGPROT)) {
		/* 为二进制各段设置 MMU 页表属性：rodata 只读、data 读写、text 只读可执行 */
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__start_rodata),
				 (size_t)(uintptr_t)(__end_rodata - __start_rodata),
				 MMU_ATTR_RO);
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__start_data),
				 (size_t)(uintptr_t)(__end_data - __start_data),
				 MMU_ATTR_RW);
		pgprot_set_attrs((phys_addr_t)(uintptr_t)(__text_start),
				 (size_t)(uintptr_t)(__text_end - __text_start),
				 MMU_ATTR_RX);
	}

	return 0;
}

__weak int arch_initr_trap(void)
{
	/* 弱符号：架构安装异常陷阱处理(默认空) */
	return 0;
}

#if defined(CONFIG_SYS_INIT_RAM_LOCK) && defined(CONFIG_E500)
static int initr_unlock_ram_in_cache(void)
{
	/* e500 专用：relocation 完成后解锁 D-Cache 中锁定的 RAM 区域 */
	unlock_ram_in_cache();	/* it's time to unlock D-cache in e500 */
	return 0;
}
#endif

static int initr_barrier(void)
{
	/* PPC 专用：插入同步屏障(确保前面的初始化对其他核可见) */
#ifdef CONFIG_PPC
	/* TODO: Can we not use dmb() macros for this? */
	asm("sync ; isync");
#endif
	return 0;
}

static int initr_malloc(void)
{
	/* 初始化完整 malloc 堆：紧邻 relocate 后的 U-Boot 映像下方。
	 * 注意：start 必须与 board_f.c:reserve_noncached() 中的计算一致。 */
	ulong start;

#if CONFIG_IS_ENABLED(SYS_MALLOC_F)
	debug("Pre-reloc malloc() used %#x bytes (%d KB)\n", gd->malloc_ptr,
	      gd->malloc_ptr / 1024);
#endif
	/* The malloc area is immediately below the monitor copy in DRAM */
	/*
	 * This value MUST match the value of gd->start_addr_sp in board_f.c:
	 * reserve_noncached().
	 */
	start = gd->relocaddr - TOTAL_MALLOC_LEN;
	gd_set_malloc_start(start);
	mem_malloc_init(start, TOTAL_MALLOC_LEN);
	return 0;
}

static int initr_of_live(void)
{
	/* 若启用 OF_LIVE，把扁平设备树 FDT 解析构建成 live tree(结构体树)，
	 * 供后续驱动以 ofnode 方式访问，性能更好。 */
	if (CONFIG_IS_ENABLED(OF_LIVE)) {
		int ret;

		bootstage_start(BOOTSTAGE_ID_ACCUM_OF_LIVE, "of_live");
		ret = of_live_build(gd->fdt_blob,
				    (struct device_node **)gd_of_root_ptr());
		bootstage_accum(BOOTSTAGE_ID_ACCUM_OF_LIVE);
		if (ret)
			return ret;
	}

	return 0;
}

#ifdef CONFIG_DM
static int initr_dm(void)
{
	/* 重建完整设备模型：丢弃 relocation 前的早期 DM，重新扫描设备树并 probe 全部设备。 */
	int ret;

	oftree_reset();

	/* Drop the pre-reloc driver model and start a new one */
	gd->dm_root = NULL;	/* 清空早期 DM 根，准备重建 */
#ifdef CONFIG_TIMER
	gd->timer = NULL;
#endif
	bootstage_start(BOOTSTAGE_ID_ACCUM_DM_R, "dm_r");
	ret = dm_init_and_scan(false);	/* false = 不限于 early 设备，扫描全部 */
	bootstage_accum(BOOTSTAGE_ID_ACCUM_DM_R);
	if (ret)
		return ret;

	return dm_autoprobe();
}
#endif

static int initr_dm_devices(void)
{
	/* 初始化 DM 相关的辅助设备：早期定时器、多路复用器(mux)默认状态等 */
	int ret;

	if (IS_ENABLED(CONFIG_TIMER_EARLY)) {
		ret = dm_timer_init();
		if (ret)
			return ret;
	}

	if (IS_ENABLED(CONFIG_MULTIPLEXER)) {
		/*
		 * Initialize the multiplexer controls to their default state.
		 * This must be done early as other drivers may unknowingly
		 * rely on it.
		 */
		ret = dm_mux_init();
		if (ret)
			return ret;
	}

	return 0;
}

static int initr_bootstage(void)
{
	/* 标记 board_init_r 阶段到达启动计时框架(bootstage) */
	bootstage_mark_name(BOOTSTAGE_ID_START_UBOOT_R, "board_init_r");

	return 0;
}

__weak int power_init_board(void)
{
	/* 弱符号：板级电源管理初始化(如 PMIC 配置，默认空) */
	return 0;
}

static int initr_announce(void)
{
	/* 调试打印：提示 U-Boot 现已在 RAM 中运行及其 reloc 地址 */
	debug("Now running in RAM - U-Boot at: %08lx\n", gd->relocaddr);
	return 0;
}

static int __maybe_unused initr_binman(void)
{
	/* 初始化 binman(若启用)，用于解析镜像布局。失败仅告警不致命。 */
	int ret;

	ret = binman_init();
	if (ret)
		printf("binman_init failed:%d\n", ret);

	return ret;
}

#if defined(CONFIG_MTD_NOR_FLASH)
__weak int is_flash_available(void)
{
	/* 弱符号：返回 1 表示 NOR flash 可用(默认可用) */
	return 1;
}

static int initr_flash(void)
{
	/* 初始化 NOR flash：探测容量、可选计算 CRC，并把信息写入 bd->bi_flash* */
	ulong flash_size = 0;
	struct bd_info *bd = gd->bd;

	if (!is_flash_available())
		return 0;

	puts("Flash: ");

	if (board_flash_wp_on())
		printf("Uninitialized - Write Protect On\n");
	else
		flash_size = flash_init();

	print_size(flash_size, "");
#ifdef CONFIG_SYS_FLASH_CHECKSUM
	/*
	 * Compute and print flash CRC if flashchecksum is set to 'y'
	 *
	 * NOTE: Maybe we should add some schedule()? XXX
	 */
	if (env_get_yesno("flashchecksum") == 1) {
		const uchar *flash_base = (const uchar *)CFG_SYS_FLASH_BASE;

		printf("  CRC: %08X", crc32(0,
					    flash_base,
					    flash_size));
	}
#endif /* CONFIG_SYS_FLASH_CHECKSUM */
	putc('\n');

	/* update start of FLASH memory    */
#ifdef CFG_SYS_FLASH_BASE
	bd->bi_flashstart = CFG_SYS_FLASH_BASE;
#endif
	/* size of FLASH memory (final value) */
	bd->bi_flashsize = flash_size;

#if defined(CONFIG_OXC) || defined(CONFIG_RMU)
	/* flash mapped at end of memory map */
	bd->bi_flashoffset = CONFIG_TEXT_BASE + flash_size;
#elif CONFIG_SYS_MONITOR_BASE == CFG_SYS_FLASH_BASE
	bd->bi_flashoffset = monitor_flash_len;	/* reserved area for monitor */
#endif
	return 0;
}
#endif

#ifdef CONFIG_CMD_NAND
/* go init the NAND */
static int initr_nand(void)
{
	/* 初始化 NAND flash 并打印容量 */
	puts("NAND:  ");
	nand_init();
	printf("%lu MiB\n", nand_size() / 1024);
	return 0;
}
#endif

#if defined(CONFIG_CMD_ONENAND)
/* go init the NAND */
static int initr_onenand(void)
{
	/* 初始化 OneNAND 闪存 */
	puts("NAND:  ");
	onenand_init();
	return 0;
}
#endif

#ifdef CONFIG_MMC
static int initr_mmc(void)
{
	/* 初始化 MMC/SD 控制器(打印 "MMC:  " 及检测到的设备) */
	puts("MMC:   ");
	mmc_initialize(gd->bd);
	return 0;
}
#endif

#ifdef CONFIG_PVBLOCK
static int initr_pvblock(void)
{
	/* 初始化 Xen PV block 虚拟块设备 */
	puts("PVBLOCK: ");
	pvblock_init();
	return 0;
}
#endif

/*
 * Tell if it's OK to load the environment early in boot.
 *
 * If CONFIG_OF_CONTROL is defined, we'll check with the FDT to see
 * if this is OK (defaulting to saying it's OK).
 *
 * NOTE: Loading the environment early can be a bad idea if security is
 *       important, since no verification is done on the environment.
 *
 * Return: 0 if environment should not be loaded, !=0 if it is ok to load
 */
static int should_load_env(void)
{
	/* 判断当前是否允许加载环境：设备树若配置 load-environment=0 则不允许，否则允许 */
	if (IS_ENABLED(CONFIG_OF_CONTROL))
		return ofnode_conf_read_int("load-environment", 1);

	return 1;
}

static int initr_env(void)
{
	/* 环境初始化：重定位环境(从存储或默认值)、从 FDT 导入环境覆盖、
	 * 设置 fdtcontroladdr，并从环境读取 loadaddr 到 image_load_addr。 */
	/* initialize environment */
	if (should_load_env())
		env_relocate();	/* 从持久存储(或默认)加载环境到内存 */
	else
		env_set_default(NULL, 0);	/* 不允许加载则使用编译期默认值 */

	env_import_fdt();	/* 把设备树中的 /config/environment 节点导入环境 */

	if (IS_ENABLED(CONFIG_OF_CONTROL))
		env_set_hex("fdtcontroladdr",
			    (unsigned long)map_to_sysmem(gd->fdt_blob));	/* 记录控制设备树地址到环境 */

	#if (IS_ENABLED(CONFIG_SAVE_PREV_BL_INITRAMFS_START_ADDR) || \
						IS_ENABLED(CONFIG_SAVE_PREV_BL_FDT_ADDR))
		save_prev_bl_data();	/* 保存前一启动阶段传入的 initramfs/fdt 地址 */
	#endif

	/* Initialize from environment */
	image_load_addr = env_get_ulong("loadaddr", 16, image_load_addr);

	return 0;
}

#ifdef CONFIG_SYS_MALLOC_BOOTPARAMS
static int initr_malloc_bootparams(void)
{
	/* 预留内核 boot parameters 内存，并把地址写入 bd->bi_boot_params */
	gd->bd->bi_boot_params = (ulong)malloc(CONFIG_SYS_BOOTPARAMS_LEN);
	if (!gd->bd->bi_boot_params) {
		puts("WARNING: Cannot allocate space for boot parameters\n");
		return -ENOMEM;
	}
	return 0;
}
#endif

static int initr_boot_led_blink(void)
{
	/* 启动 LED 闪烁(指示 U-Boot 正在运行) */
	led_boot_blink();

	return 0;
}

static int initr_boot_led_on(void)
{
	/* 启动 LED 常亮(表示启动流程即将交给 OS) */
	led_boot_on();

	return 0;
}

#if CONFIG_IS_ENABLED(NET)
static int initr_net(void)
{
	/* 初始化网络子系统(打印 "Net:  " 并探测以太网设备) */
	puts("Net:   ");
	eth_initialize();
#if defined(CONFIG_RESET_PHY_R)
	debug("Reset Ethernet PHY\n");
	reset_phy();	/* 必要时复位 PHY */
#endif
	return 0;
}
#endif

#ifdef CONFIG_POST
static int initr_post(void)
{
	/* 上电自检(POST)：运行 RAM 阶段测试项 */
	post_run(NULL, POST_RAM | post_bootmode_get(0));
	return 0;
}
#endif

#if defined(CFG_PRAM)
/*
 * Export available size of memory for Linux, taking into account the
 * protected RAM at top of memory
 */
int initr_mem(void)
{
	/* 把扣除受保护 RAM(pram)后的可用内存大小写入环境变量 "mem"，供内核使用 */
	ulong pram = 0;
	char memsz[32];

	pram = env_get_ulong("pram", 10, CFG_PRAM);
	sprintf(memsz, "%ldk", (long int)((gd->ram_size / 1024) - pram));
	env_set("mem", memsz);

	return 0;
}
#endif

static int initr_lmb(void)
{
	/* 初始化 LMB(Logical Memory Blocks)库，用于启动 OS 时管理/预留内存区域 */
	if (CONFIG_IS_ENABLED(LMB))
		return lmb_init();
	else
		return 0;
}

static int dm_announce(void)
{
	/* 打印 DM 统计信息：设备数、uclass 数、设备树来源等 */
	int device_count;
	int uclass_count;

	if (IS_ENABLED(CONFIG_DM)) {
		dm_get_stats(&device_count, &uclass_count);
		printf("Core:  %d devices, %d uclasses", device_count,
		       uclass_count);
		if (CONFIG_IS_ENABLED(OF_REAL))
			printf(", devicetree: %s", fdtdec_get_srcname());
		if (CONFIG_IS_ENABLED(UPL))
			printf(", universal payload active");
		printf("\n");
		if (IS_ENABLED(CONFIG_OF_HAS_PRIOR_STAGE) &&
		    (gd->fdt_src == FDTSRC_SEPARATE ||
		     gd->fdt_src == FDTSRC_EMBED)) {
			printf("Warning: Unexpected devicetree source (not from a prior stage)");
			printf("Warning: U-Boot may not function properly\n");
		}
	}

	return 0;
}

static int run_main_loop(void)
{
	/* 进入命令/启动主循环：触发 EVT_MAIN_LOOP 事件，然后无限循环调用 main_loop()。
	 * main_loop() 若返回(重试 autoboot)则再次调用，理论上不退出。 */
	int ret;

#ifdef CONFIG_SANDBOX
	sandbox_main_loop_init();
#endif

	ret = event_notify_null(EVT_MAIN_LOOP);
	if (ret)
		return ret;

	/* main_loop() can return to retry autoboot, if so just run it again */
	for (;;)
		main_loop();
	return 0;
}

/*
 * Over time we hope to remove most of the driver-related init and do it
 * if/when the driver is later used.
 *
 * TODO: perhaps reset the watchdog in the initcall function after each call?
 */

static void initcall_run_r(void)
{
	/*
	 * board_init_r 的核心：按固定顺序调用全部 INITCALL(relocation 之后)。
	 * 顺序要点：
	 *  1) trace / reloc / reloc_global_data：完成 relocation 收尾与数据重映射；
	 *  2) caches / malloc / DM 全量扫描 / OF_LIVE：建立运行期环境；
	 *  3) board_init(架构/板级芯片选择)、各类存储(NOR/NAND/ONENAND/MMC)、网络；
	 *  4) env 重定位、控制台终态(console_init_r)、LED、看门狗重置；
	 *  5) 最后 run_main_loop -> main_loop 进入命令/启动循环(NOTREACHED)。
	 * 其中穿插大量 WATCHDOG_RESET() 以在长初始化期间喂狗。
	 */
	INITCALL(initr_trace);
	INITCALL(initr_reloc);
	INITCALL(event_init);
	/* TODO: could x86/PPC have this also perhaps? */
#if CONFIG_IS_ENABLED(ARM) || CONFIG_IS_ENABLED(RISCV)
	INITCALL(initr_caches);
	/* Note: For Freescale LS2 SoCs, new MMU table is created in DDR.
	 *	 A temporary mapping of IFC high region is since removed,
	 *	 so environmental variables in NOR flash is not available
	 *	 until board_init() is called below to remap IFC to high
	 *	 region.
	 */
#endif
	INITCALL(initr_reloc_global_data);
#if CONFIG_IS_ENABLED(SYS_INIT_RAM_LOCK) && CONFIG_IS_ENABLED(E500)
	INITCALL(initr_unlock_ram_in_cache);
#endif
	INITCALL(initr_barrier);
	INITCALL(initr_malloc);
	INITCALL(log_init);
	INITCALL(initr_bootstage); /* Needs malloc() but has its own timer */
#if CONFIG_IS_ENABLED(CONSOLE_RECORD)
	INITCALL(console_record_init);
#endif
#if CONFIG_IS_ENABLED(SYS_HAS_NONCACHED_MEMORY)
	INITCALL(noncached_init);
#endif
	INITCALL(initr_of_live);
#if CONFIG_IS_ENABLED(DM)
	INITCALL(initr_dm);
#endif
#if CONFIG_IS_ENABLED(ADDR_MAP)
	INITCALL(init_addr_map);
#endif
#if CONFIG_IS_ENABLED(BOARD_INIT)
	INITCALL(board_init);	/* Setup chipselects */
#endif
	/*
	 * TODO: printing of the clock inforamtion of the board is now
	 * implemented as part of bdinfo command. Currently only support for
	 * davinci SOC's is added. Remove this check once all the board
	 * implement this.
	 */
#if CONFIG_IS_ENABLED(CLOCKS)
	INITCALL(set_cpu_clk_info);
#endif
	INITCALL(initr_lmb);
#if CONFIG_IS_ENABLED(EFI_LOADER)
	INITCALL(efi_memory_init);
#endif
#if CONFIG_IS_ENABLED(BINMAN_FDT)
	INITCALL(initr_binman);
#endif
#if CONFIG_IS_ENABLED(FSP_VERSION2)
	INITCALL(arch_fsp_init_r);
#endif
	INITCALL(initr_dm_devices);
	INITCALL(stdio_init_tables);
	INITCALL(serial_initialize);
	INITCALL(initr_announce);
	INITCALL(dm_announce);
#if CONFIG_IS_ENABLED(WDT)
	INITCALL(initr_watchdog);
#endif
	WATCHDOG_RESET();
	INITCALL(arch_initr_trap);
#if CONFIG_IS_ENABLED(BOARD_EARLY_INIT_R)
	INITCALL(board_early_init_r);
#endif
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(POST)
	INITCALL(post_output_backlog);
#endif
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(PCI_INIT_R) && CONFIG_IS_ENABLED(SYS_EARLY_PCI_INIT)
	/*
	 * Do early PCI configuration _before_ the flash gets initialised,
	 * because PCU resources are crucial for flash access on some boards.
	 */
	INITCALL(pci_init);
#endif
#if CONFIG_IS_ENABLED(ARCH_EARLY_INIT_R)
	INITCALL(arch_early_init_r);
#endif
	INITCALL(power_init_board);
#if CONFIG_IS_ENABLED(MTD_NOR_FLASH)
	INITCALL(initr_flash);
#endif
	WATCHDOG_RESET();
#if IS_ENABLED(CONFIG_PPC) || CONFIG_IS_ENABLED(M68K) || CONFIG_IS_ENABLED(X86)
	/* initialize higher level parts of CPU like time base and timers */
	INITCALL(cpu_init_r);
#endif
#if CONFIG_IS_ENABLED(EFI_LOADER)
	INITCALL(efi_init_early);
#endif
#if CONFIG_IS_ENABLED(CMD_NAND)
	INITCALL(initr_nand);
#endif
#if CONFIG_IS_ENABLED(CMD_ONENAND)
	INITCALL(initr_onenand);
#endif
#if CONFIG_IS_ENABLED(MMC)
	INITCALL(initr_mmc);
#endif
#if CONFIG_IS_ENABLED(XEN)
	INITCALL(xen_init);
#endif
#if CONFIG_IS_ENABLED(PVBLOCK)
	INITCALL(initr_pvblock);
#endif
	INITCALL(initr_env);
#if CONFIG_IS_ENABLED(SYS_MALLOC_BOOTPARAMS)
	INITCALL(initr_malloc_bootparams);
#endif
	WATCHDOG_RESET();
	INITCALL(cpu_secondary_init_r);
#if CONFIG_IS_ENABLED(ID_EEPROM)
	INITCALL(mac_read_from_eeprom);
#endif
	INITCALL_EVT(EVT_SETTINGS_R);
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(PCI_INIT_R) && !CONFIG_IS_ENABLED(SYS_EARLY_PCI_INIT)
	/*
	 * Do pci configuration
	 */
	INITCALL(pci_init);
#endif
	INITCALL(stdio_add_devices);
	INITCALL(jumptable_init);
#if CONFIG_IS_ENABLED(API)
	INITCALL(api_init);
#endif
	INITCALL(console_init_r);	/* fully init console as a device */
#if CONFIG_IS_ENABLED(DISPLAY_BOARDINFO_LATE)
	INITCALL(console_announce_r);
	INITCALL(show_board_info);
#endif
	/* miscellaneous arch-dependent init */
#if CONFIG_IS_ENABLED(ARCH_MISC_INIT)
	INITCALL(arch_misc_init);
#endif
	/* miscellaneous platform-dependent init */
#if CONFIG_IS_ENABLED(MISC_INIT_R)
	INITCALL(misc_init_r);
#endif
	WATCHDOG_RESET();
#if CONFIG_IS_ENABLED(CMD_KGDB)
	INITCALL(kgdb_init);
#endif
	INITCALL(interrupt_init);
#if defined(CONFIG_MICROBLAZE) || defined(CONFIG_M68K)
	INITCALL(timer_init);		/* initialize timer */
#endif
	INITCALL(initr_boot_led_blink);
	/* PPC has a udelay(20) here dating from 2002. Why? */
#if CONFIG_IS_ENABLED(BOARD_LATE_INIT)
	INITCALL(board_late_init);
#endif
#if CONFIG_IS_ENABLED(PCI_ENDPOINT)
	INITCALL(pci_ep_init);
#endif
#if CONFIG_IS_ENABLED(NET)
	WATCHDOG_RESET();
	INITCALL(initr_net);
#endif
#if CONFIG_IS_ENABLED(POST)
	INITCALL(initr_post);
#endif
	WATCHDOG_RESET();
	INITCALL_EVT(EVT_LAST_STAGE_INIT);
#if defined(CFG_PRAM)
	INITCALL(initr_mem);
#endif
	INITCALL(initr_boot_led_on);
	INITCALL(run_main_loop);
}

void board_init_r(gd_t *new_gd, ulong dest_addr)
{
	/* relocation 之后"第二阶段"初始化总入口。由 crt0.S(ARM)或 board_init_f_r(x86)
	 * 调用，此时 U-Boot 已在 RAM 顶部运行、gd 已复制到新位置。
	 * 职责：切换到新 gd、标记串口/日志为重初始化、调用 initcall_run_r()，
	 * 之后进入 run_main_loop() 不返回。
	 * 参数 new_gd：relocation 后的 global data 指针；
	 * 参数 dest_addr：relocation 目标地址(= gd->relocaddr)。 */

	printf("[TRACE] board_init_r entered\n");

	/*
	 * The pre-relocation drivers may be using memory that has now gone
	 * away. Mark serial as unavailable - this will fall back to the debug
	 * UART if available.
	 *
	 * Do the same with log drivers since the memory may not be available.
	 */
	gd->flags &= ~(GD_FLG_SERIAL_READY | GD_FLG_LOG_READY);	/* 标记旧串口/日志设备暂不可用，待重建 */

	/*
	 * Set up the new global data pointer. So far only x86 does this
	 * here.
	 * TODO(sjg@chromium.org): Consider doing this for all archs, or
	 * dropping the new_gd parameter.
	 */
	if (CONFIG_IS_ENABLED(X86_64) && !IS_ENABLED(CONFIG_EFI_APP))
		arch_setup_gd(new_gd);	/* x86_64 在此切换新 gd */

#if defined(CONFIG_RISCV)
	set_gd(new_gd);	/* RISC-V 设置新 gd */
#elif !defined(CONFIG_X86) && !defined(CONFIG_ARM) && !defined(CONFIG_ARM64)
	gd = new_gd;	/* 其它架构直接替换 gd 指针 */
#endif
	gd->flags &= ~GD_FLG_LOG_READY;

	initcall_run_r();

	/* NOTREACHED - run_main_loop() does not return */
	hang();
}
