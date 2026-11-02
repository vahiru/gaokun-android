# docs/ 索引 · Documentation index

**Using the ROM?** Start here — these are the only pages written for end users, in English and Chinese:

| | English | 中文 |
|---|---|---|
| Install / update / remove | [INSTALL.md](INSTALL.md) | [INSTALL.zh-CN.md](INSTALL.zh-CN.md) |
| Questions & troubleshooting | [FAQ.md](FAQ.md) | [FAQ.zh-CN.md](FAQ.zh-CN.md) |
| Known limitations, unsupported features | [known-limitations.md](known-limitations.md) | [known-limitations.zh-CN.md](known-limitations.zh-CN.md) |
| Release notes | [relnotes/](relnotes/) (latest released: [v0.7.1-alpha](relnotes/v0.7.1-alpha.md)) | 同左（已发布的各版只有英文；1.0.0 起另附 `.zh-CN.md`） |

项目总览与状态表在仓库根的 [README.md](../README.md) / [README.zh-CN.md](../README.zh-CN.md)。
下面是全部文档，给维护者和 AI 助手用。

**状态**：**现行** = 描述现状，改代码要跟着改；**参考** = 长期有效的事实 / 规格 / 证据，不追现状；
**部分有效** = 一部分已被取代，见备注；**已取代** = 只剩历史价值，指向接替者；**历史** = 当时的记录，结论可能已被推翻。
⚠️ 冲突时的优先级：实机实测 > [`stage4-findings.md`](stage4-findings.md) 的案卷 > `CLAUDE.md` > [`project-log.md`](project-log.md)。
**面向**：用户 / 维护者 / AI（AI 助手开工必读的标 AI）。

链接是否有效：`python3 scripts/check-doc-links.py`（只查相对链接与 `#锚点`，有问题退出码 1）。

## 用户文档

| 文档 | 讲什么 | 状态 | 面向 |
|---|---|---|---|
| [INSTALL.md](INSTALL.md) / [.zh-CN](INSTALL.zh-CN.md) | 图形 / 命令行安装、免 U 盘双系统、更新、adb、救援系统、启动入口与 fastboot | 现行（中英同步） | 用户 |
| [FAQ.md](FAQ.md) / [.zh-CN](FAQ.zh-CN.md) | 按"出了什么事 → 怎么办"组织的问答：启动菜单、回退、抓日志、恢复出厂、卸载 | 现行（中英同步；出处在中文版注释里） | 用户 |
| [known-limitations.md](known-limitations.md) / [.zh-CN](known-limitations.zh-CN.md) | 装之前该知道的长期限制、不支持的功能、安全与许可取舍（每条注释对应 v1.0-plan 的 id） | 现行（中英同步） | 用户 |
| [relnotes/](relnotes/) | 各版发版说明：v0.3.0 / v0.6.0 / v0.6.1 / v0.6.2 / v0.7.0 / v0.7.1 已发布 | 历史（已发布的不改） | 用户 |
| [relnotes/v1.0.0-draft.md](relnotes/v1.0.0-draft.md) / [.zh-CN](relnotes/v1.0.0-draft.zh-CN.md) | 1.0.0 发版说明草稿（定稿时改名） | 现行（草稿） | 维护者 |
| [relnotes/v0.6.3-alpha.md](relnotes/v0.6.3-alpha.md) | 从未发布的 v0.6.3 草稿 | 已取代 → [v0.7.0-alpha](relnotes/v0.7.0-alpha.md)（并入） | 维护者 |
| [relnotes/TEMPLATE.md](relnotes/TEMPLATE.md) | 发版说明模板（头注释的状态行格式） | 现行 | 维护者 |
| [relnotes/v0.7.1-alpha-sources.md](relnotes/v0.7.1-alpha-sources.md) + `v0.7.1-alpha-config.txt` | v0.7.1 内核对应源码清单与配置（GPL §3，事后补录；之后由 release.sh 自动生成） | 参考 | 维护者 |

## 现在在做什么

| 文档 | 讲什么 | 状态 | 面向 |
|---|---|---|---|
| [TODO.md](TODO.md) | 还剩什么、下一步（按 1.0 发版路径分组的总表）；过程记录见 [archive/TODO-history-2026-10.md](archive/TODO-history-2026-10.md) | 现行 | 维护者 · AI |
| [v1.0-plan.md](v1.0-plan.md) | 1.0.0 发版前修复计划：发版标准、阻断项、分批路线、待拍板的决定（D 编号） | 现行 | 维护者 · AI |
| [release-checklist.md](release-checklist.md) | 发版回归清单（A 无人值守 / B 要人在场 / C 长稳与测量），每个候选版从这里抄 | 现行 | 维护者 |
| [build-machine.md](build-machine.md) | 构建机（Azure VM）按负载选机型、停机规矩、盘的代价 | 现行 | 维护者 · AI |
| [boot-entry-design.md](boot-entry-design.md) | 统一启动入口 `gk3boot.efi` + fastboot 执行端 + 双系统（方案 Y，实验 E0–E11、决定 U1–U25） | 现行（开头"还没有实现"已过时：E3–E8 真机过，实现见 [`tools/gk3boot/README.md`](../tools/gk3boot/README.md)） | 维护者 · AI |
| [fastboot-design.md](fastboot-design.md) | 最早的 fastboot 设计稿（C′：ESP 常驻 initramfs） | 部分有效：§4 `gk3-fastbootd` 的协议 / 白名单 / 清除语义 / USB / 界面仍是执行端依据；§3.3 推荐与 §7.1 U1–U7 已被 [boot-entry-design.md](boot-entry-design.md) 取代 | 维护者 |
| [stage7-flutter-debian.md](stage7-flutter-debian.md) | 图形安装器（Flutter + Debian live）：决定、里程碑 M0–M4、真机记录 | 现行 | 维护者 · AI |
| [installer-rust-design.md](installer-rust-design.md) | 安装器后端用 Rust 重写（并行轨道）：不静默失败的规则、迁移顺序、协议兼容与偏差表、对拍、进镜像的方式；对着 shell 版查出的问题 S1–S5 | 现行（阶段 0 + 1a 完成，未替换任何东西；代码 [`tools/gk3-installer/`](../tools/gk3-installer/)） | 维护者 · AI |
| [stage7-live-installer.md](stage7-live-installer.md) | 第一版 Stage 7 设计（C + cairo 安装器 + Alpine 救援）与 M0 实测 | 部分有效：一套镜像 / 共用内核与 dtb / ESP 约束 / M0 实测仍被 `scripts/live/` 引用；界面与底座已被 [stage7-flutter-debian.md](stage7-flutter-debian.md) 取代 | 维护者 |
| [a17-migration-plan.md](a17-migration-plan.md) | crDroid 13 / Android 17 迁移：上游现状、耦合点、OTA 与构建机约束、步骤与进度（1.0 改在 A17 上发） | 现行 | 维护者 · AI |
| [mesa-a17.md](mesa-a17.md) | A17 上 turnip 的生成管线（mesa 26.1 + meson2hermetic，`scripts/mesa-a17/`） | 现行 | 维护者 · AI |
| [fingerprint-driver-design.md](fingerprint-driver-design.md) | 指纹（FTE7001）：TA 已加载进 QSEE（M1），M2 发真命令暂停；架构、复现步骤 | 现行（暂停中） | 维护者 · AI |
| [gaming.md](gaming.md) | 游戏特化总览：39 位地址窗口、着色器缓存、低延迟音频、触摸 game 预设、游戏模式脚本、netlink 边界、版本串覆写；边界（如实修正 / 不伪造）与待定项 | 现行 | 维护者 · AI |
| [duck-detector-plan.md](duck-detector-plan.md) | Duck Detector 体检的逐项诊断、A–D 分类、每次上机后的 delta（10-11）；文首有更正 | 现行（过程记录；结论以 [gaming.md](gaming.md) 为准） | 维护者 · AI |
| [upstream/README.md](upstream/README.md) | 5 份待投上游的相机 / camcc 补丁稿与收件人（未发，等用户点头） | 现行 | 维护者 |

## 案卷与证据（"某个结论是怎么来的"）

| 文档 | 讲什么 | 状态 | 面向 |
|---|---|---|---|
| [stage4-findings.md](stage4-findings.md) | 按 `#NN` 编号的实测案卷（#26 起；`stage4-findings.md#78` 这样的锚点可直接跳） | 参考（最权威；被推翻的条目原地标注） | 维护者 · AI |
| [stage0-findings.md](stage0-findings.md) | Stage 0：从参考树 grep 核实的事实（内核基线、固件路径等） | 参考 / 历史 | 维护者 |
| [stage1-results.md](stage1-results.md) | Stage 1 验收：binderfs、dwc3 peripheral | 参考 / 历史（证据类，不归档） | 维护者 |
| [stage2-findings.md](stage2-findings.md) + [`stage2-acceptance-live.txt`](stage2-acceptance-live.txt) | Stage 2：Android 启动链路的十二个真问题与验收现场 | 参考 / 历史 | 维护者 · AI |
| [stage5-freedreno.md](stage5-freedreno.md) | Stage 5：turnip + ANGLE 硬件 GPU 的路线与浸泡 | 参考 / 历史 | 维护者 |
| [stage6-crdroid.md](stage6-crdroid.md) | Stage 6：转 crDroid 16.0 与产品化（OTA / root / SELinux / 传感器 / 硬解） | 参考 / 历史 | 维护者 · AI |
| [camera-photo-rotation-2026-09-19.md](camera-photo-rotation-2026-09-19.md) | 前后摄照片转 90° 的根因与修复（一次事故的完整记录） | 历史（事故记录） | 维护者 |
| [`evidence-58-drm-crtc-panic.txt`](evidence-58-drm-crtc-panic.txt) | 案卷 #58 的 efi_pstore 原始崩溃日志 | 参考（证据） | 维护者 |
| [source-audit-20261018.md](source-audit-20261018.md) | 构建机源码核对：V5 六条出处（libbase errno、vold、sysprop.mk、configfs UDC、Settings 文案、SF `--latency` 64 帧）与 GAME-1（0085 版本串伪装）的副作用排查 | 参考（2026-10-18；上机核对项见 TODO GAME-1） | 维护者 · AI |
| [project-log.md](project-log.md) | CLAUDE.md 曾经的前言框与 Stage 里程碑（按时间倒序，原样搬来） | 历史 | 维护者 · AI |

## 硬件与协议参考

| 文档 | 讲什么 | 状态 | 面向 |
|---|---|---|---|
| [hw-inventory.md](hw-inventory.md) | 硬件清单：config、固件路径、modetest、evdev、UCM、pstore（2026-08-13 采集） | 参考 | 维护者 · AI |
| [hw/README.md](hw/README.md) | 硬件原始转储：后摄 EEPROM、BIOS 拆包；另有统一启动入口实验 E3–E8 的上机日志 `hw/gk3boot*-20261005.txt`（README 里未登记） | 参考（证据） | 维护者 |
| [hw/bios-2.16/README.md](hw/bios-2.16/README.md) | BIOS 2.16 升级包拆包：工具、逐模块清单、逐字节复现步骤 | 参考 | 维护者 |
| [sensors-ssc-protocol.md](sensors-ssc-protocol.md) | SSC 传感器 QMI / protobuf 协议规格（每条带出处），sensors HAL 的实现依据 | 参考（开头"客户端尚未实现"已过时：HAL 在 [`device/huawei/gaokun3/sensors-hal/`](../device/huawei/gaokun3/sensors-hal/README.md)） | 维护者 |
| [stylus.md](stylus.md) | 手写笔（M-Pencil）：触摸从片与笔 MCU 两条数据通路，驱动 / 守护进程 / 权限的组成，设计要点，已知限制，调试 | 现行 | 维护者 · AI |
| [contrib/slpi-sensors-deploy.md](contrib/slpi-sensors-deploy.md) | 外部贡献：Linux 侧 SLPI 传感器部署（推翻了 #37 的旧结论） | 参考（原样保留） | 维护者 |
| [fingerprint/README.md](fingerprint/README.md) | 指纹 Windows 侧逆向报告的目录说明（报告本身不入库） | 参考 | 维护者 |
| [parallel-mainline-generic.md](parallel-mainline-generic.md) | LineageOS 系 mainline-generic 的 gaokun3 支持（他人分享的补丁与交叉验证） | 历史（2026-08-17 后未跟进） | 维护者 |
| [deep-sleep.md](deep-sleep.md) | 深度待机：`qcom_stats` 三个计数的含义，之前挡住 DDR 和 CX 下电的三处投票，链路断开时的供电，WoW 下的 BA 失步，Wi-Fi 崩溃恢复，恢复失败时怎么办，排查方法 | 现行 | 维护者 · AI |
| [palm-rejection.md](palm-rejection.md) | 触摸屏的手掌识别：驱动里“手的位置图”的规则，以`MT_TOOL_PALM`撤回已上报的触点，参数、计数器、已知限制与调试 | 现行 | 维护者 · AI |
| `img/`、`stage3-desktop.png` | README 与案卷引用的截图 / 照片 | — | — |

## 归档（[archive/](archive/README.md)）

计划类文档在它规划的工作做完之后归档；案卷类永不归档。登记表与理由见 [archive/README.md](archive/README.md)。

| 文档 | 讲什么 | 状态 |
|---|---|---|
| [archive/stage1-kernel-plan.md](archive/stage1-kernel-plan.md) | Stage 0 收尾 + Stage 1 内核构建方案 | 已取代 → [stage1-results.md](stage1-results.md) |
| [archive/stage2-plan.md](archive/stage2-plan.md) | Stage 2 引导链 + AOSP 启动方案 | 已取代 → [stage2-findings.md](stage2-findings.md) |
| [archive/plan-2026-09-14.md](archive/plan-2026-09-14.md) | 09-14 收尾计划（相机内核 + v0.6.1 两批） | 已取代 → [TODO.md](TODO.md) |
| [archive/touch-morning-runbook.md](archive/touch-morning-runbook.md) | 2026-09-16 触摸实机调参手册 | 已取代 → 案卷 #114–#116、[`scripts/touch/README.md`](../scripts/touch/README.md) |
| [archive/TODO-history-2026-10.md](archive/TODO-history-2026-10.md) | TODO 截至 2026-10-06 的过程记录（dev.1–dev.9、已结案调查、v0.6/0.7 收口条目） | 历史 |
| [archive/stage7-installer-roadmap.md](archive/stage7-installer-roadmap.md) | C 版安装器的需求排序与 08-23 搁置时的接手说明 | 已取代 → [stage7-flutter-debian.md](stage7-flutter-debian.md) |

## docs/ 以外的说明文档

脚本、工具与设备树各自的 README（`scripts/*/README.md`、`tools/*/README.md`、`device/huawei/gaokun3/*/README.md`、
`live/installer-flutter/README.md`、`patches/*/README.md`）讲的是**那个目录怎么用**，跟着代码走，不在这张表里重复。
常用的几份：[`scripts/live/README.md`](../scripts/live/README.md)（live 镜像与安装器后端测试）、
[`tools/gk3boot/README.md`](../tools/gk3boot/README.md)（统一启动入口实现）、
[`scripts/windows/README.md`](../scripts/windows/README.md)（Windows 侧脚本与伴随工具）、
[`scripts/selinux/README.md`](../scripts/selinux/README.md)（SELinux 普查工具）、
[`scripts/touch/README.md`](../scripts/touch/README.md)（触摸调参）。
