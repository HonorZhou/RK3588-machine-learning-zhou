// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2011
 * Corscience GmbH & Co. KG - Simon Schwarz <schwarz@corscience.de>
 *  - Added prep subcommand support
 *  - Reorganized source - modeled after powerpc version
 *
 * (C) Copyright 2002
 * Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Marius Groeger <mgroeger@sysgo.de>
 *
 * Copyright (C) 2001  Erik Mouw (J.A.K.Mouw@its.tudelft.nl)
 */

#include <bootm.h>
#include <bootstage.h>
#include <command.h>
#include <cpu_func.h>
#include <dm.h>
#include <log.h>
#include <asm/global_data.h>
#include <dm/root.h>
#include <env.h>
#include <image.h>
#include <u-boot/zlib.h>
#include <asm/byteorder.h>
#include <linux/libfdt.h>
#include <mapmem.h>
#include <fdt_support.h>
#include <asm/bootm.h>
#include <asm/secure.h>
#include <linux/compiler.h>
#include <bootm.h>
#include <vxworks.h>
#include <asm/cache.h>

#ifdef CONFIG_ARMV7_NONSEC
#include <asm/armv7.h>
#endif
#include <asm/setup.h>

/*
 * =========================================================================
 * bootm.c —— ARM/ARM64 平台 bootm 命令的实现(把 OS 镜像交给内核)
 * -------------------------------------------------------------------------
 * 这是 U-Boot 启动 Linux 内核的"最后一公里"。主入口 do_bootm_linux() 按 flag
 * 分两步完成：
 *   - PREP 阶段：boot_prep_linux() —— 构造传给内核的参数，要么修正 FDT
 *     (device tree)，要么构造传统 ATAGS 链表(旧式 ARM 内核用)；
 *   - GO 阶段：boot_jump_linux() —— 把 CPU 切到正确的异常等级(EL2/EL1)，
 *     清理现场后跳转到内核入口地址(images->ep)。
 * 关键结构：
 *   images(bootm_headers)：本次启动的镜像头集合，含内核入口 ep、设备树地址
 *     ft_addr/ft_len、initrd 范围、OS 架构 os.arch 等。
 *   bd(bd_info)：板级信息(机器号 bi_arch_number、boot params 地址等)。
 *   params(struct tag *)：ATAGS 链表当前写入位置(传统启动方式)。
 * =========================================================================
 */

DECLARE_GLOBAL_DATA_PTR;

static struct tag *params;	/* ATAGS 链表当前写入指针(传统 ARM 启动参数) */

static void setup_start_tag (struct bd_info *bd)
{
	/* ATAGS 起点：写 ATAG_CORE 头，并把 params 指向 bi_boot_params 处 */
	params = (struct tag *)bd->bi_boot_params;

	params->hdr.tag = ATAG_CORE;
	params->hdr.size = tag_size (tag_core);

	params->u.core.flags = 0;
	params->u.core.pagesize = 0;
	params->u.core.rootdev = 0;

	params = tag_next (params);
}

static void setup_memory_tags(struct bd_info *bd)
{
	/* 为每个 DRAM bank 写一条 ATAG_MEM，告诉内核物理内存布局 */
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		params->hdr.tag = ATAG_MEM;
		params->hdr.size = tag_size (tag_mem32);

		params->u.mem.start = gd->dram[i].start;
		params->u.mem.size = gd->dram[i].size;

		params = tag_next (params);
	}
}

static void setup_commandline_tag(struct bd_info *bd, char *commandline)
{
	/* 把内核命令行(bootargs)写入 ATAG_CMDLINE 节点 */
	char *p;

	if (!commandline)
		return;

	/* eat leading white space */
	for (p = commandline; *p == ' '; p++);

	/* skip non-existent command lines so the kernel will still
	 * use its default command line.
	 */
	if (*p == '\0')
		return;

	params->hdr.tag = ATAG_CMDLINE;
	params->hdr.size =
		(sizeof (struct tag_header) + strlen (p) + 1 + 4) >> 2;

	strcpy (params->u.cmdline.cmdline, p);

	params = tag_next (params);
}

static void setup_initrd_tag(struct bd_info *bd, ulong initrd_start,
			     ulong initrd_end)
{
	/* 写 ATAG_INITRD2 节点，告诉内核压缩 ramdisk 的位置与大小 */
	/* an ATAG_INITRD node tells the kernel where the compressed
	 * ramdisk can be found. ATAG_RDIMG is a better name, actually.
	 */
	params->hdr.tag = ATAG_INITRD2;
	params->hdr.size = tag_size (tag_initrd);

	params->u.initrd.start = initrd_start;
	params->u.initrd.size = initrd_end - initrd_start;

	params = tag_next (params);
}

static void setup_serial_tag(struct tag **tmp)
{
	/* 写 ATAG_SERIAL 节点，传入板级序列号(由 get_board_serial 获取) */
	struct tag *params = *tmp;
	struct tag_serialnr serialnr;

	get_board_serial(&serialnr);
	params->hdr.tag = ATAG_SERIAL;
	params->hdr.size = tag_size (tag_serialnr);
	params->u.serialnr.low = serialnr.low;
	params->u.serialnr.high= serialnr.high;
	params = tag_next (params);
	*tmp = params;
}

static void setup_revision_tag(struct tag **in_params)
{
	/* 写 ATAG_REVISION 节点，传入板级硬件版本号 */
	u32 rev = 0;

	rev = get_board_rev();
	params->hdr.tag = ATAG_REVISION;
	params->hdr.size = tag_size (tag_revision);
	params->u.revision.rev = rev;
	params = tag_next (params);
}

static void setup_end_tag(struct bd_info *bd)
{
	/* 写 ATAG_NONE 结束节点，标志 ATAGS 链表结束 */
	params->hdr.tag = ATAG_NONE;
	params->hdr.size = 0;
}

__weak void setup_board_tags(struct tag **in_params) {}
	/* 弱符号：板级可追加自定义 ATAGS 节点(默认空) */

#ifdef CONFIG_ARM64
static void do_nonsec_virt_switch(void)
{
	/* ARM64：跳内核前唤醒所有从核并关闭 D-Cache(刷脏数据)，为切换 EL 做准备 */
	smp_kick_all_cpus();
	dcache_disable();	/* flush cache before switching to EL2 */
}
#endif

__weak void board_prep_linux(struct bootm_headers *images) { }
	/* 弱符号：板级在跳转前的最后准备钩子(默认空) */

/* Subcommand: PREP */
static void boot_prep_linux(struct bootm_headers *images)
{
	/* PREP 子命令：构造传给内核的参数。
	 * 优先使用 FDT(设备树)：调用 image_setup_linux() 修正/重定位设备树；
	 * 否则若启用 ATAGS 支持：依次写 start/mem/cmdline/initrd/serial/revision
	 * 等 tag，并追加板级 tag、写结束 tag；
	 * 两者都不支持则 panic。最后调用 board_prep_linux() 板级钩子。
	 * 参数 images：本次启动的镜像头集合。 */
	char *commandline = env_get("bootargs");

	if (CONFIG_IS_ENABLED(OF_LIBFDT) && IS_ENABLED(CONFIG_LMB) && images->ft_len) {
		debug("using: FDT\n");
		if (image_setup_linux(images)) {
			panic("FDT creation failed!");
		}
	} else if (BOOTM_ENABLE_TAGS) {
		debug("using: ATAGS\n");
		setup_start_tag(gd->bd);
		if (BOOTM_ENABLE_SERIAL_TAG)
			setup_serial_tag(&params);
		if (BOOTM_ENABLE_CMDLINE_TAG)
			setup_commandline_tag(gd->bd, commandline);
		if (BOOTM_ENABLE_REVISION_TAG)
			setup_revision_tag(&params);
		if (BOOTM_ENABLE_MEMORY_TAGS)
			setup_memory_tags(gd->bd);
		if (BOOTM_ENABLE_INITRD_TAG) {
			/*
			 * In boot_ramdisk_high(), it may relocate ramdisk to
			 * a specified location. And set images->initrd_start &
			 * images->initrd_end to relocated ramdisk's start/end
			 * addresses. So use them instead of images->rd_start &
			 * images->rd_end when possible.
			 */
			if (images->initrd_start && images->initrd_end) {
				setup_initrd_tag(gd->bd, images->initrd_start,
						 images->initrd_end);
			} else if (images->rd_start && images->rd_end) {
				setup_initrd_tag(gd->bd, images->rd_start,
						 images->rd_end);
			}
		}
		setup_board_tags(&params);
		setup_end_tag(gd->bd);
	} else {
		panic("FDT and ATAGS support not compiled in\n");
	}

	board_prep_linux(images);
}

__weak bool armv7_boot_nonsec_default(void)
{
	/* 弱符号：ARMv7 默认是否以非安全(nonsec)模式启动内核(默认：非安全，
	 * 除非配置了 ARMv7_BOOT_SEC_DEFAULT) */
#ifdef CONFIG_ARMV7_BOOT_SEC_DEFAULT
	return false;
#else
	return true;
#endif
}

#ifdef CONFIG_ARMV7_NONSEC
bool armv7_boot_nonsec(void)
{
	/* 根据环境变量 "bootm_boot_mode"(sec/nonsec)决定 ARMv7 是否以非安全模式启动 */
	char *s = env_get("bootm_boot_mode");
	bool nonsec = armv7_boot_nonsec_default();

	if (s && !strcmp(s, "sec"))
		nonsec = false;

	if (s && !strcmp(s, "nonsec"))
		nonsec = true;

	return nonsec;
}
#else
bool armv7_boot_nonsec(void)
{
	/* 未启用 ARMv7 非安全支持时，固定返回 false(以安全模式启动) */
	return false;
}
#endif

#ifdef CONFIG_ARM64
__weak void update_os_arch_secondary_cores(uint8_t os_arch)
{
	/* 弱符号：切换从核运行的操作系统架构标识(默认空，PSCI 平台可覆盖) */
}

#ifdef CONFIG_ARMV8_SWITCH_TO_EL1
static void switch_to_el1(void)
{
	/* ARM64：把从 64 位 U-Boot 启动的 32 位 ARM 内核切换到 EL1 运行 */
	if ((IH_ARCH_DEFAULT == IH_ARCH_ARM64) &&
	    (images.os.arch == IH_ARCH_ARM))
		armv8_switch_to_el1(0, (u64)gd->bd->bi_arch_number,
				    (u64)images.ft_addr, 0,
				    (u64)images.ep,
				    ES_TO_AARCH32);
	else
		armv8_switch_to_el1((u64)images.ft_addr, 0, 0, 0,
				    images.ep,
				    ES_TO_AARCH64);
}
#endif
#endif

/* Subcommand: GO */
#ifdef CONFIG_ARM64
static void boot_jump_linux(struct bootm_headers *images, int flag)
{
	/* GO 子命令(ARM64)：清理现场后把 CPU 切到正确 EL 并跳转到内核入口。
	 * 参数 images：镜像头集合；flag：启动子命令标志(含 FAKE_GO 时只模拟不真跳)。
	 * 流程：标记 RUN_OS -> bootm_final -> cleanup_before_linux ->
	 * 唤醒从核/关 D-Cache -> 更新从核 OS 架构 -> 经 armv8_switch_to_el2
	 * (可再转 EL1)跳到内核入口(内核约定：x0=FDT 地址，其余为 0)。 */
	void (*kernel_entry)(void *fdt_addr, void *res0, void *res1,
			void *res2);
	kernel_entry = (void (*)(void *fdt_addr, void *res0, void *res1,
				void *res2))images->ep;

	debug("## Transferring control to Linux (at address %lx)...\n",
		(ulong) kernel_entry);
	bootstage_mark(BOOTSTAGE_ID_RUN_OS);

	bootm_final(flag);
	cleanup_before_linux();

	if (!(flag & BOOTM_STATE_OS_FAKE_GO)) {
#ifdef CONFIG_ARMV8_PSCI
		armv8_setup_psci();	/* 设置 PSCI，供从核启动 */
#endif
		do_nonsec_virt_switch();	/* 唤醒从核并关闭 D-Cache */

		update_os_arch_secondary_cores(images->os.arch);	/* 通知从核 OS 架构 */

#ifdef CONFIG_ARMV8_SWITCH_TO_EL1
		armv8_switch_to_el2((u64)images->ft_addr, 0, 0, 0,
				    (u64)switch_to_el1, ES_TO_AARCH64);	/* 先到 EL2，再由 switch_to_el1 转 EL1 */
#else
		if ((IH_ARCH_DEFAULT == IH_ARCH_ARM64) &&
		    (images->os.arch == IH_ARCH_ARM))
			armv8_switch_to_el2(0, (u64)gd->bd->bi_arch_number,
					    (u64)images->ft_addr, 0,
					    (u64)images->ep,
					    ES_TO_AARCH32);	/* 32 位 ARM 内核：经 EL2 以 AArch32 启动 */
		else
			armv8_switch_to_el2((u64)images->ft_addr, 0, 0, 0,
					    images->ep,
					    ES_TO_AARCH64);	/* 64 位内核：经 EL2 以 AArch64 启动，x0=FDT */
#endif
	}
}
#else
static __maybe_unused bool boot_jump_via_optee;
static __maybe_unused unsigned long boot_jump_via_optee_addr;

static void boot_jump_linux(struct bootm_headers *images, int flag)
{
	/* GO 子命令(ARMv7/其它 32 位 ARM)：跳转到内核入口。
	 * 内核约定：r0=0, r1=机器号 machid, r2=ATAGS/FDT 地址。
	 * 流程：读取 machid(可被环境变量覆盖) -> 标记 RUN_OS -> bootm_final ->
	 * cleanup_before_linux -> 确定 r2(FTB 或 bi_boot_params) -> 必要时进入
	 * 非安全模式(armv7_init_nonsec / secure_ram_addr)或经 OPTEE 跳转 -> 调用
	 * kernel_entry(0, machid, r2)。FAKE_GO 时只准备不真跳。 */
	unsigned long machid = gd->bd->bi_arch_number;
	char *s;
	void (*kernel_entry)(int zero, int arch, uint params);
	unsigned long r2;
	kernel_entry = (void (*)(int, int, uint))images->ep;
#ifdef CONFIG_CPU_V7M_V8M
	ulong addr = (ulong)kernel_entry | 1;	/* Cortex-M  thumb 模式：地址置最低位 */
	kernel_entry = (void *)addr;
#endif

	if (IS_ENABLED(CONFIG_ARMV7_NONSEC) && armv7_boot_nonsec() &&
	    boot_jump_via_optee) {
		printf("Cannot start OPTEE-OS from NS\n");
		return;
	}

	s = env_get("machid");
	if (s) {
		if (strict_strtoul(s, 16, &machid) < 0) {
			debug("strict_strtoul failed!\n");
			return;
		}
		printf("Using machid 0x%lx from environment\n", machid);
	}

	debug("## Transferring control to Linux (at address %08lx)" \
		"...\n", (ulong) kernel_entry);
	bootstage_mark(BOOTSTAGE_ID_RUN_OS);
	bootm_final(flag);
	cleanup_before_linux();

	if (CONFIG_IS_ENABLED(OF_LIBFDT) && images->ft_len)
		r2 = (unsigned long)images->ft_addr;	/* 有设备树：r2 = FDT 地址 */
	else
		r2 = gd->bd->bi_boot_params;	/* 否则：r2 = ATAGS 地址 */

	if (flag & BOOTM_STATE_OS_FAKE_GO)
		return;	/* 仅模拟，不真正跳转 */

#ifdef CONFIG_ARMV7_NONSEC
	if (armv7_boot_nonsec())
		armv7_init_nonsec();	/* 初始化非安全世界(HYP/PSCI) */
#endif

#ifdef CONFIG_BOOTM_OPTEE
	if (boot_jump_via_optee)
		boot_jump_linux_via_optee(kernel_entry, machid, r2, boot_jump_via_optee_addr);	/* 经 OPTEE 跳转 */
#endif

#ifdef CONFIG_ARMV7_NONSEC
	if (armv7_boot_nonsec()) {
		secure_ram_addr(_do_nonsec_entry)(kernel_entry, 0, machid, r2);	/* 在非安全上下文跳内核 */
	} else
#endif
	{
		kernel_entry(0, machid, r2);	/* 安全模式直接跳内核：r0=0,r1=machid,r2=参数地址 */
	}
}
#endif

#ifndef CONFIG_TI_SECURE_DEVICE
static void arch_tee_image_process(ulong image, size_t size)
{
	/* TEE(可信执行环境)镜像处理回调：登记 OPTEE 跳转信息，供后续经 OPTEE 启动内核 */
	boot_jump_via_optee = true;
	boot_jump_via_optee_addr = image;
}
U_BOOT_FIT_LOADABLE_HANDLER(IH_TYPE_TEE, arch_tee_image_process);	/* 注册为 FIT 可加载 TEE 类型的处理器 */
#endif

/* Main Entry point for arm bootm implementation
 *
 * Modeled after the powerpc implementation
 * DIFFERENCE: Instead of calling prep and go at the end
 * they are called if subcommand is equal 0.
 */
int do_bootm_linux(int flag, struct bootm_info *bmi)
{
	/* ARM bootm 命令实现的主入口(由 bootm 命令或 bootstd 调用)。
	 * 参数 flag：启动子命令标志；bmi：启动信息(含 images 镜像头集合)。
	 * 行为：
	 *  - 带 OS_BD_T / OS_CMDLINE 标志在 ARM 上不支持，直接返回 -1；
	 *  - 带 OS_PREP：只执行 boot_prep_linux()(构造参数)；
	 *  - 带 OS_GO / OS_FAKE_GO：只执行 boot_jump_linux()(跳转内核)；
	 *  - 其它(正常启动)：依次执行 PREP + GO，即先准备参数再跳转。 */
	struct bootm_headers *images = bmi->images;

	/* No need for those on ARM */
	if (flag & BOOTM_STATE_OS_BD_T || flag & BOOTM_STATE_OS_CMDLINE)
		return -1;

	if (flag & BOOTM_STATE_OS_PREP) {
		boot_prep_linux(images);
		return 0;
	}

	if (flag & (BOOTM_STATE_OS_GO | BOOTM_STATE_OS_FAKE_GO)) {
		boot_jump_linux(images, flag);
		return 0;
	}

	boot_prep_linux(images);
	boot_jump_linux(images, flag);
	return 0;
}

#if defined(CONFIG_BOOTM_VXWORKS)
void boot_prep_vxworks(struct bootm_headers *images)
{
	/* VxWorks 启动准备：修正设备树 memory 节点(若有)，并清理现场 */
#if defined(CONFIG_OF_LIBFDT)
	int off;

	if (images->ft_addr) {
		off = fdt_path_offset(images->ft_addr, "/memory");
		if (off > 0) {
			if (arch_fixup_fdt(images->ft_addr))
				puts("## WARNING: fixup memory failed!\n");
		}
	}
#endif
	cleanup_before_linux();
}

void boot_jump_vxworks(struct bootm_headers *images)
{
	/* VxWorks 跳转：ARM64+PSCI 下先设置 PSCI 并唤醒从核，再以 FDT 物理地址
	 * 作为唯一参数跳到 VxWorks 入口。 */
#if defined(CONFIG_ARM64) && defined(CONFIG_ARMV8_PSCI)
	armv8_setup_psci();
	smp_kick_all_cpus();
#endif

	/* ARM VxWorks requires device tree physical address to be passed */
	((void (*)(void *))images->ep)(images->ft_addr);
}
#endif
