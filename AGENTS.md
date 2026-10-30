# AGENTS.md — gaokun-android 工作区说明

## 项目范围

MateBook E Go（gaokun3，SC8280XP / Nt36672c）的 Android 移植工作区：
设备树/内核补丁/构建脚本，外加指纹（FocalTech FTE7001）逆向与 bring-up 文档。

## 目录结构

- `device/`、`patches/`、`manifests/`、`scripts/`、`wheels/`、`out/` — 移植主体（内核补丁、构建产物、工具链）
- `docs/` — 设计与调研文档（`fingerprint-driver-design.md`、`fingerprint/` 下的逆向报告）
- `tools/fingerprint-bringup/` — 指纹 bring-up 工具与实验脚本
- `tools/fingerprint-bringup/rev/` — 逆向工作区：提取出的 blob、IDA 配置、实验笔记
- `refs/`、`live/`、`issues.md` — 参考资料与问题记录

## 可编辑 / 需回避

- 可自由编辑：`docs/`、`tools/fingerprint-bringup/`、`scripts/`、`patches/`、本文件
- 不要改动：`out/`（构建产物）、`wheels/`（第三方 wheel）、`refs/`（引用材料）
- **不要入库**（.gitignore 已覆盖，勿 git add）：专有二进制
  （`fingerpr.mbn`、`FtWbioDriverUmdf.dll` 等）与指纹 RE 报告 `docs/fingerprint/*.md`
- 全程默认**离线不碰真机**：除非用户明确要求，禁止 adb / 刷写 / 触发设备

## 注意

- 仓库根目录下若干历史遗留杂项（图片、截图文件）与主线无关，忽略即可。

## 磁盘空间

- 用户明确要求注意磁盘空间。大镜像构建/解包/下载前检查实际输出文件系统的可用空间，按峰值同时计入镜像与验证副本，并保留至少2GiB余量；空间不足不要先启动再等待失败。
- 清理仅针对已核对哈希、可从保留镜像重建的重复cache产物；保留原始输入、回退镜像、设备备份、未保存工作和验证记录。不要全局Docker prune或清理其他项目volume。
- 记录清理后实际可用空间，不能把删除文件的逻辑大小当作实际释放空间。
