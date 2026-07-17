/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2013
 * David Feng <fenghua@phytium.com.cn>
 */

#include <asm-offsets.h>
#include <config.h>
#include <linux/linkage.h>
#include <asm/macro.h>
#include <asm/armv8/mmu.h>

/*************************************************************************
 *
 * Startup Code (reset vector)
 * ----------------------------------------------------------------------
 * ARMv8(AArch64) 复位启动汇编。本文件是 U-Boot 最早期的入口：
 * 上电/复位后 CPU 跳到 _start，依次完成：
 *   1) PIE(Position Independent Executable) 重定位修复，使 U-Boot 可在
 *      与链接地址不同的位置运行；
 *   2) 设置异常向量表 VBAR(指向 vectors)；
 *   3) 根据当前异常等级 EL3/EL2/EL1 配置 SCR/CPTR/CPACR 等系统寄存器；
 *   4) 应用 ARM core 勘误(apply_core_errata)；
 *   5) 底层 SoC 初始化 lowlevel_init(主要是 GIC 中断控制器)；
 *   6) 主核(master_cpu)调用 _main(位于 crt0.S)，进而进入 C 语言的
 *      board_init_f，开始 relocation 与后续初始化。
 * 从核(secondary)通常走 spin-table / PSCI / parking-protocol 等待主核释放。
 *************************************************************************/

.globl	_start
_start:
#if defined(CONFIG_LINUX_KERNEL_IMAGE_HEADER)
#include <asm/boot0-linux-kernel-header.h>
#elif defined(CONFIG_ENABLE_ARM_SOC_BOOT0_HOOK)
/*
 * Various SoCs need something special and SoC-specific up front in
 * order to boot, allow them to set that in their boot0.h file and then
 * use it here.
 */
#include <asm/arch/boot0.h>
#else
	b	reset		/* 没有特殊 boot0 头时，直接跳转到 reset 复位处理入口 */
#endif

	.align 3

.globl	_TEXT_BASE
_TEXT_BASE:
	.quad	CONFIG_TEXT_BASE	/* U-Boot 链接时设定的代码基址(text base) */

/*
 * These are defined in the linker script.
 * 以下符号均由链接脚本(arm64 的 u-boot.lds)定义，这里把它们相对 _start
 * 的偏移量以 .quad 形式放在数据段，供 relocation 计算使用。
 */
.globl	_end_ofs
_end_ofs:
	.quad	_end - _start		/* 映像末尾相对 _start 的偏移 */

.globl	_bss_start_ofs
_bss_start_ofs:
	.quad	__bss_start - _start	/* BSS 段起点相对 _start 的偏移 */

.globl	_bss_end_ofs
_bss_end_ofs:
	.quad	__bss_end - _start	/* BSS 段结束相对 _start 的偏移 */

reset:
	/* Allow the board to save important registers */
	b	save_boot_params	/* 先跳到 save_boot_params 保存 BootROM/上电固件传入的关键寄存器 */

.globl	save_boot_params_ret
save_boot_params_ret:
	/* save_boot_params 保存参数后返回此处，继续后续早期初始化 */

#if CONFIG_POSITION_INDEPENDENT && !defined(CONFIG_XPL_BUILD)
	/* Verify that we're 4K aligned.  */
	adr	x0, _start		/* 取得 _start 的运行期(实际加载)地址 */
	ands	x0, x0, #0xfff		/* 检查是否 4KB 对齐(低 12 位为 0) */
	b.eq	1f			/* 已对齐，跳到 1f 继续 */
0:
	/*
	 * FATAL, can't continue.
	 * U-Boot needs to be loaded at a 4K aligned address.
	 *
	 * We use ADRP and ADD to load some symbol addresses during startup.
	 * The ADD uses an absolute (non pc-relative) lo12 relocation
	 * thus requiring 4K alignment.
	 */
	wfi				/* 未 4KB 对齐是致命错误：进入 WFI 低功耗等待，不再继续 */
	b	0b
1:

	/*
	 * Fix .rela.dyn relocations. This allows U-Boot to be loaded to and
	 * executed at a different address than it was linked at.
	 */
pie_fixup:				/* PIE 重定位修复入口：让 U-Boot 可在与链接地址不同的物理地址运行 */
	adr	x0, _start		/* x0 <- _start 的运行期地址 */
	ldr	x1, _TEXT_BASE		/* x1 <- _start 的链接期地址(CONFIG_TEXT_BASE) */
	subs	x9, x0, x1		/* x9 <- 运行地址与链接地址之差(relocation offset) */
	beq	pie_fixup_done		/* 差为 0 说明无需重定位，直接跳过 */
	adrp    x2, __rel_dyn_start	/* x2 <- __rel_dyn_start 运行期地址(动态重定位表起点) */
	add     x2, x2, #:lo12:__rel_dyn_start
	adrp    x3, __rel_dyn_end	/* x3 <- __rel_dyn_end 运行期地址(动态重定位表终点) */
	add     x3, x3, #:lo12:__rel_dyn_end
pie_fix_loop:
	ldp	x0, x1, [x2], #16	/* (x0,x1) <- (待修正符号的链接地址, 重定位类型) */
	ldr	x4, [x2], #8		/* x4 <- addend(加数) */
	cmp	w1, #1027		/* 相对重定位类型 R_AARCH64_RELATIVE 的值为 1027 ? */
	bne	pie_skip_reloc		/* 不是相对重定位，跳过此次修正 */
	/* relative fix: store addend plus offset at dest location */
	add	x0, x0, x9		/* 把链接地址加上 offset 得到运行期目标地址 */
	add	x4, x4, x9		/* 把 addend 加上 offset 得到运行期值 */
	str	x4, [x0]		/* 将修正后的值写回目标位置 */
pie_skip_reloc:
	cmp	x2, x3			/* 是否已遍历完整张重定位表 */
	b.lo	pie_fix_loop		/* 未遍历完，继续循环 */
pie_fixup_done:
#endif

#if defined(CONFIG_ARMV8_SPL_EXCEPTION_VECTORS) || !defined(CONFIG_XPL_BUILD)
.macro	set_vbar, regname, reg	/* 宏：把异常向量表地址写入指定 VBAR_ELn 寄存器 */
	msr	\regname, \reg
.endm
	adr	x0, vectors		/* x0 <- 异常向量表 vectors 的运行期地址 */
#else
.macro	set_vbar, regname, reg	/* SPL/XPL 未启用异常向量时，该宏为空操作(什么都不做) */
.endm
#endif
	/*
	 * Could be EL3/EL2/EL1, Initial State:
	 * Little Endian, MMU Disabled, i/dCache Disabled
	 * 此时 CPU 可能处于 EL3/EL2/EL1 任意一级，且处于小端、MMU 关闭、
	 * 指令/数据 Cache 关闭的初始状态。
	 */
	switch_el x1, 3f, 2f, 1f	/* 根据当前异常等级(EL)跳转到 3f(EL3)/2f(EL2)/1f(EL1) */
3:	set_vbar vbar_el3, x0		/* EL3：设置 VBAR_EL3 指向异常向量表 */
	mrs	x0, scr_el3		/* 读 SCR_EL3(安全配置寄存器) */
	orr	x0, x0, #0xf		/* SCR_EL3.NS|IRQ|FIQ|EA：置非安全态、把 IRQ/FIQ/SError 路由到 EL3 */
	msr	scr_el3, x0
	msr	cptr_el3, xzr		/* 清零 CPTR_EL3，开启 EL3 对 FP/SIMD 的访问 */
	b	0f
2:	mrs	x1, hcr_el2		/* EL2：读 HCR_EL2(hypervisor 配置寄存器) */
	tbnz	x1, #HCR_EL2_E2H_BIT, 1f	/* 若 E2H 置位(EL2 host 模式)，直接走 EL1 分支 */
	orr	x1, x1, #HCR_EL2_AMO_EL2	/* 把 SError 路由到 EL2 */
	msr	hcr_el2, x1
	set_vbar vbar_el2, x0		/* 设置 VBAR_EL2 */
	mov	x0, #0x33ff		/* CPTR_EL2 值：开启 FP/SIMD 并屏蔽其它 trap */
	msr	cptr_el2, x0		/* 开启 EL2 对 FP/SIMD 的访问 */
	b	0f
1:	set_vbar vbar_el1, x0		/* 设置 VBAR_EL1 */
	mov	x0, #3 << 20		/* CPACR_EL1：开启 EL1 对 FP/SIMD 的访问(第 20/21 位) */
	msr	cpacr_el1, x0
0:
	msr	daifclr, #0x4		/* 清除 DAIF 的 D 位(SError mask)，即 unmask SError 中断 */

#if CONFIG_COUNTER_FREQUENCY
	branch_if_not_highest_el x0, 4f	/* 若不是最高 EL(例如在 EL2 上电)，跳过 CNTFRQ 设置 */
	ldr	x0, =CONFIG_COUNTER_FREQUENCY
	msr	cntfrq_el0, x0		/* 初始化通用定时器频率寄存器 CNTFRQ_EL0 */
#endif

4:	isb				/* 指令同步屏障，确保前面系统寄存器的修改立即生效 */

	/*
	 * Enable SMPEN bit for coherency.
	 * This register is not architectural but at the moment
	 * this bit should be set for A53/A57/A72.
	 * 开启 SMP 一致性位，多核场景下必须设置(针对 A53/A57/A72 等)。
	 */
#ifdef CONFIG_ARMV8_SET_SMPEN
	switch_el x1, 3f, 1f, 1f	/* 仅 EL3 需要设置 SMPEN，EL2/EL1 直接跳过 */
3:
	mrs     x0, S3_1_c15_c2_1	/* 读 CPUACTLR_EL1(厂商自定义寄存器) */
	orr     x0, x0, #0x40		/* 置 SMPEN 位，开启多核缓存一致性 */
	msr     S3_1_c15_c2_1, x0
	isb
1:
#endif

	/* Apply ARM core specific erratas */
	bl	apply_core_errata	/* 调用 CPU 勘误修复(针对具体 Cortex-A 型号) */

	/*
	 * Cache/BPB/TLB Invalidate
	 * i-cache is invalidated before enabled in icache_enable()
	 * tlb is invalidated before mmu is enabled in dcache_enable()
	 * d-cache is invalidated before enabled in dcache_enable()
	 * 注：各级 Cache/TLB 的实际失效在后续 enable 阶段才做，这里先不处理。
	 */

	/* Processor specific initialization */
	bl	lowlevel_init		/* 调用底层 SoC 初始化(主要是 GIC 中断控制器初始化) */

#if defined(CONFIG_ARMV8_SPIN_TABLE) && !defined(CONFIG_XPL_BUILD)
	branch_if_master x0, master_cpu	/* 主核走 master_cpu；从核走自旋表(spin-table)等待释放 */
	b	spin_table_secondary_jump
	/* never return */
#elif defined(CONFIG_ACPI_PARKING_PROTOCOL) && !defined(CONFIG_SPL_BUILD)
	branch_if_master x0, master_cpu
	/*
	 * Waits for ACPI parking protocol memory to be allocated and the spin-table
	 * code to be written. Once ready the secondary CPUs will jump and spin in
	 * their own 4KiB memory region, which is also used as mailbox, until released
	 * by the OS.
	 */
	b	acpi_pp_secondary_jump
	/* never return */
#elif defined(CONFIG_ARMV8_MULTIENTRY)
	branch_if_master x0, master_cpu	/* 主核跳到 master_cpu */

	/*
	 * Slave CPUs
	 * 从核：进入低功耗等待，轮询 CPU_RELEASE_ADDR 是否被主核写入释放地址。
	 */
slave_cpu:
	wfe				/* 从核进入 WFE 低功耗等待，直到被唤醒 */
	ldr	x1, =CPU_RELEASE_ADDR	/* 读取释放地址邮箱 */
	ldr	x0, [x1]
	cbz	x0, slave_cpu		/* 若 release 地址仍为 0，继续等待 */
	br	x0			/* 否则跳转到该地址(由主核或 Boot 协议释放) */
#endif /* CONFIG_ARMV8_MULTIENTRY */
master_cpu:
	msr	SPSel, #1		/* 选择 SP_ELx(异常专用栈指针)，而非 SP_EL0 */
	bl	_main			/* 进入 crt0.S 的 _main，开始搭建 C 运行环境并最终调用 board_init_f */

/*-----------------------------------------------------------------------*/

WEAK(apply_core_errata)
	/* ARMv8 CPU 勘误(errata)修复总入口(弱符号，可被 SoC 代码覆盖)。
	 * 先识别当前 Cortex 核心型号，再跳到对应 A53/A57 的勘误实现。 */
	mov	x29, lr			/* 用 x29 保存返回地址 LR，便于子流程返回 */

	/* Check if we are running on a Cortex-A53 core */
	branch_if_a53_core x0, apply_a53_core_errata	/* 识别为 A53 则跳到 A53 勘误处理 */

	/* Check if we are running on a Cortex-A57 core */
	branch_if_a57_core x0, apply_a57_core_errata	/* 识别为 A57 则跳到 A57 勘误处理 */
0:
	mov	lr, x29			/* 恢复 LR */
	ret

apply_a53_core_errata:
	/* Cortex-A53 专用勘误实现。通过 MIDR_EL1 判断 variant/revision 是否命中。 */

#ifdef CONFIG_ARM_ERRATA_855873
	mrs	x0, midr_el1		/* 读 MIDR_EL1(主 ID 寄存器，含 variant/revision) */
	tst	x0, #(0xf << 20)	/* 检查 variant 字段(第 20-23 位) */
	b.ne	0b			/* 非目标修订版本，返回 */

	mrs	x0, midr_el1
	and	x0, x0, #0xf		/* 取 revision 低 4 位 */
	cmp	x0, #3
	b.lt	0b			/* revision < 3 不适用该勘误，返回 */

	mrs	x0, S3_1_c15_c2_0	/* 读 CPUACTLR_EL1(厂商定义) */
	/* Enable data cache clean as data cache clean/invalidate */
	orr	x0, x0, #1 << 44	/* 置位：让 DC CIVAC 表现为 clean+invalidate(规避 855873 勘误) */
	msr	S3_1_c15_c2_0, x0	/* 写回 CPUACTLR_EL1 */
	isb
#endif
	b 0b

apply_a57_core_errata:
	/* Cortex-A57 专用勘误实现。每个宏开关对应一个具体勘误。 */

#ifdef CONFIG_ARM_ERRATA_828024
	mrs	x0, S3_1_c15_c2_0	/* 读 CPUACTLR_EL1 */
	/* Disable non-allocate hint of w-b-n-a memory type */
	orr	x0, x0, #1 << 49	/* 关闭 w-b-n-a 内存类型的 non-allocate hint */
	/* Disable write streaming no L1-allocate threshold */
	orr	x0, x0, #3 << 25	/* 关闭写流水的 no-L1-allocate 阈值 */
	/* Disable write streaming no-allocate threshold */
	orr	x0, x0, #3 << 27	/* 关闭写流水的 no-allocate 阈值 */
	msr	S3_1_c15_c2_0, x0
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_826974
	mrs	x0, S3_1_c15_c2_0	/* 读 CPUACTLR_EL1 */
	/* Disable speculative load execution ahead of a DMB */
	orr	x0, x0, #1 << 59	/* 禁止 DMB 之前的推测性 load 执行(规避 826974) */
	msr	S3_1_c15_c2_0, x0
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_833471
	mrs	x0, S3_1_c15_c2_0	/* 读 CPUACTLR_EL1 */
	/* FPSCR write flush. */
	orr	x0, x0, #1 << 38	/* 开启 FPSCR 写刷新，规避浮点相关勘误(833471) */
	msr	S3_1_c15_c2_0, x0
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_829520
	mrs	x0, S3_1_c15_c2_0	/* 读 CPUACTLR_EL1 */
	/* Disable Indirect Predictor bit will prevent this erratum from occurring */
	orr	x0, x0, #1 << 4		/* 关闭 indirect predictor，规避 BTB 推测执行勘误(829520) */
	msr	S3_1_c15_c2_0, x0
	isb
#endif

#ifdef CONFIG_ARM_ERRATA_833069
	mrs	x0, S3_1_c15_c2_0	/* 读 CPUACTLR_EL1 */
	/* Disable Enable Invalidates of BTB bit */
	and	x0, x0, #0xE		/* 仅保留低 3 位其余清零，关闭"使 BTB 失效"使能位(833069) */
	msr	S3_1_c15_c2_0, x0
	isb
#endif
	b 0b
ENDPROC(apply_core_errata)

/*-----------------------------------------------------------------------*/

WEAK(lowlevel_init)
	/* 底层 SoC 相关初始化(弱符号)。主要工作是初始化 GIC 中断控制器，
	 * 并对多核场景下的从核做 PSCI/EL 切换处理。 */
	mov	x29, lr			/* 保存 LR 到 x29 */

#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
	branch_if_slave x0, 1f		/* 从核跳过 GIC Distributor 的安全世界初始化 */
	ldr	x0, =GICD_BASE
	bl	gic_init_secure		/* 主核初始化 GIC Distributor 的安全部分(GICD) */
1:
#if defined(CONFIG_GICV3)
	ldr	x0, =GICR_BASE
	bl	gic_init_secure_percpu	/* GICv3：每核初始化 Redistributor(GICR) */
#elif defined(CONFIG_GICV2)
	ldr	x0, =GICD_BASE
	ldr	x1, =GICC_BASE
	bl	gic_init_secure_percpu	/* GICv2：每核初始化 CPU interface(GICC) */
#endif
#endif

#ifdef CONFIG_ARMV8_MULTIENTRY
	branch_if_master x0, 2f		/* 主核跳到 2f 直接返回 */

	/*
	 * Slave should wait for master clearing spin table.
	 * 从核需要先等待主核清空白旋表，避免读到错误的释放地址而跳错位置。
	 */
#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
#ifdef CONFIG_GICV2
	ldr	x0, =GICC_BASE
#endif
	bl	gic_wait_for_interrupt	/* 从核在此等待中断(由主核通过 SGI 唤醒) */
#endif

	/* All slaves will enter EL2 and optionally EL1. */
#if defined(CONFIG_ARMV8_PSCI) && !defined(CONFIG_XPL_BUILD)
        bl      psci_setup_vectors	/* 若启用 PSCI，为从核设置 PSCI 入口向量 */
#endif
	adr	x4, lowlevel_in_el2
	ldr	x5, =ES_TO_AARCH64
	bl	armv8_switch_to_el2	/* 从核切换到 EL2(准备后续被 OS 接管) */

lowlevel_in_el2:
#ifdef CONFIG_ARMV8_SWITCH_TO_EL1
	adr	x4, lowlevel_in_el1
	ldr	x5, =ES_TO_AARCH64
	bl	armv8_switch_to_el1	/* 若配置了，从核继续切换到 EL1 */

lowlevel_in_el1:
#endif

#endif /* CONFIG_ARMV8_MULTIENTRY */

2:
	mov	lr, x29			/* 恢复 LR */
	ret
ENDPROC(lowlevel_init)

WEAK(smp_kick_all_cpus)
	/* 通过 SGI 0 软中断把其它从核唤醒(弱符号)。 */
#if defined(CONFIG_GICV2) || defined(CONFIG_GICV3)
	ldr	x0, =GICD_BASE
	b	gic_kick_secondary_cpus	/* 跳到 GIC 驱动的实现去发送唤醒中断 */
#endif
	ret
ENDPROC(smp_kick_all_cpus)

/*-----------------------------------------------------------------------*/

ENTRY(c_runtime_cpu_setup)
	/* relocation(重定位)之后、进入 C 运行环境前调用：把 VBAR 重新指向
	 * 已搬到 RAM 新位置的异常向量表 vectors。 */
#if defined(CONFIG_ARMV8_SPL_EXCEPTION_VECTORS) || !defined(CONFIG_XPL_BUILD)
	/* Relocate vBAR */
	adr	x0, vectors
	switch_el x1, 3f, 2f, 1f	/* 按当前 EL 选择对应的 VBAR 寄存器 */
3:	msr	vbar_el3, x0		/* 重新设置 VBAR_EL3 指向新位置的 vectors */
	b	0f
2:	msr	vbar_el2, x0		/* 重新设置 VBAR_EL2 */
	b	0f
1:	msr	vbar_el1, x0		/* 重新设置 VBAR_EL1 */
0:
#endif

	ret
ENDPROC(c_runtime_cpu_setup)

WEAK(save_boot_params)
	/* 保存 Boot ROM / 上电固件传入的寄存器参数(弱符号，可被板级代码覆盖)。
	 * 当启用 bloblist 时，把 x0-x3 存到 .data 段的 saved_args 中供后续使用。 */
#if (IS_ENABLED(CONFIG_BLOBLIST))
	/* Calculate the PC-relative address of saved_args */
	adrp	x9, saved_args		/* 计算 saved_args 的 PC 相对基址 */
	add	x9, x9, :lo12:saved_args	/* 加上低 12 位偏移得到精确地址 */
	stp 	x0, x1, [x9]		/* 保存 x0,x1(通常含 FDT/ATAGS 等指针) */
	stp 	x2, x3, [x9, #16]	/* 保存 x2,x3 */
#endif
	b	save_boot_params_ret	/* 返回 reset 中的 save_boot_params_ret 处继续初始化 */
ENDPROC(save_boot_params)

#if (IS_ENABLED(CONFIG_BLOBLIST))
.section .data
.align 2
.global saved_args
saved_args:
	.rept 4
	.dword 0			/* 预留 4 个 64 位 slot，用于保存 boot 参数(x0-x3) */
	.endr
END(saved_args)
#endif
