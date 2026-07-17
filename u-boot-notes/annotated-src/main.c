// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

/* #define	DEBUG	*/

#include <autoboot.h>
#include <button.h>
#include <bootstage.h>
#include <bootstd.h>
#include <cli.h>
#include <command.h>
#include <console.h>
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <net.h>
#include <version_string.h>
#include <efi_loader.h>
#include <event.h>

/*
 * main.c —— U-Boot 启动链路的最末端：命令处理主循环 main_loop()。
 * 它在 board_init_r() 的 run_main_loop() 中被调用，是 U-Boot 初始化完成、
 * 准备接收命令或自动启动 OS 的入口。main_loop() 负责标记启动阶段、设置
 * 版本变量、运行 preboot、处理 autoboot 延迟倒计时，最终要么执行 autoboot
 * (bootcmd)，要么进入交互式命令行 cli_loop() / 标准启动 bootstd_prog_boot()。
 */

/* 在 autoboot 等待之前，运行环境变量 "preboot" 中定义的命令序列 */
static void run_preboot_environment_command(void)
{
	char *p;

	p = env_get("preboot");	/* 读取 preboot 环境变量 */
	if (p != NULL) {
		int prev = 0;

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			prev = disable_ctrlc(1); /* 键控 autoboot 模式下临时禁用 Ctrl-C 中断，避免在 preboot 中被打断 */

		run_command_list(p, -1, 0);	/* 执行 preboot 命令列表(-1 表示以 '\0' 结尾的字符串) */

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			disable_ctrlc(prev);	/* 恢复 Ctrl-C 检查状态 */
	}
}

/* We come here after U-Boot is initialised and ready to process commands */
void main_loop(void)
{
	/* U-Boot 初始化完成、进入命令处理主循环。
	 * 调用时机：由 board_init_r() -> initcall_run_r() -> run_main_loop() -> main_loop() 进入。
	 * 职责：标记启动阶段、设置版本变量、处理 preboot、autoboot 延迟倒计时，
	 * 最终调用 autoboot_command 执行 bootcmd，或被打断后进入 cli_loop / bootstd_prog_boot。 */
	const char *s;

	bootstage_mark_name(BOOTSTAGE_ID_MAIN_LOOP, "main_loop");	/* 标记启动阶段：到达 main_loop */

	if (IS_ENABLED(CONFIG_VERSION_VARIABLE))
		env_set("ver", version_string);  /* 把版本字符串写入环境变量 "ver" */

	cli_init();	/* 初始化命令行接口(命令表、解析缓冲等) */

	if (IS_ENABLED(CONFIG_USE_PREBOOT))
		run_preboot_environment_command();	/* 若启用 preboot，先执行其中的命令 */

	if (event_notify_null(EVT_POST_PREBOOT))	/* 触发 POST_PREBOOT 事件，返回非 0 则提前退出 main_loop */
		return;

	if (IS_ENABLED(CONFIG_UPDATE_TFTP))
		update_tftp(0UL, NULL, NULL);	/* 若启用，从 TFTP 服务器更新固件镜像 */

	if (IS_ENABLED(CONFIG_EFI_CAPSULE_ON_DISK_EARLY)) {
		/* efi_init_early() already called */
		if (efi_init_obj_list() == EFI_SUCCESS)
			efi_launch_capsules();	/* 启动盘上的 EFI capsule 固件升级(early 阶段) */
	}

	process_button_cmds();	/* 处理物理按键触发的命令(若板级支持) */

	s = bootdelay_process();	/* 读取 bootdelay 并启动 autoboot 倒计时，返回 bootcmd 字符串 */
	if (cli_process_fdt(&s))
		cli_secure_boot_cmd(s);	/* 安全启动(secure boot)模式下，直接以受控方式执行命令 */

	autoboot_command(s);	/* 处理 autoboot：倒计时内无按键则执行 s(bootcmd)；有按键则进入命令行 */

	/* if standard boot if enabled, assume that it will be able to boot */
	if (IS_ENABLED(CONFIG_BOOTSTD_PROG)) {
		int ret;

		ret = bootstd_prog_boot();	/* 标准启动：按 bootflow 自动查找并启动 OS(替代传统 bootcmd) */
		printf("Standard boot failed (err=%dE)\n", ret);
		panic("Failed to boot");	/* 标准启动也失败，停机并提示 */
	}

	cli_loop();	/* 进入交互式命令行循环(autoboot 被按键打断时) */

	panic("No CLI available");	/* 理论上 cli_loop() 不会返回，若返回则是致命错误 */
}
