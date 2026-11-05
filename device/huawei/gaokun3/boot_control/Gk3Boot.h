/*
 * 统一启动入口（gk3boot.efi）的 Android 侧：开机完成线程。
 * 设计见 tools/gk3boot/README.md；实现说明在 Gk3Boot.cpp 顶部。
 *
 * Copyright 2026 The gaokun-android contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace gaokun3 {

// 起一个分离的线程：等 vendor.gaokun3.boot.done=1（vendor rc 在 sys.boot_completed=1 时设），
// 然后做一次 bless / 清 boot_streak / 导出事件 / 按 persist.vendor.gaokun3.gk3boot 部署或撤掉入口。
// main() 在加入 binder 线程池之前调一次。
// ★ S15（2026-10-05）：同时起请求线程（等 vendor.gaokun3.bootentry.ring=1，把 Parts 的 next_windows / default_windows /
//   default_android 写进 GK3 记录，回 vendor.gaokun3.bootentry.ack）。
void StartBootCompletedWorker();

// S15：vendor rc 的 on shutdown 里 exec 本二进制 --gk3-mark-poweroff 时走这里（不起 binder、不起线程）：
// 关机且默认是 Windows ⇒ GK3 记录置 clean_poweroff。总是返回 0（失败的后果只是下次开机进 Android，不拖住关机）。
int MarkPoweroffMain();

}  // namespace gaokun3
