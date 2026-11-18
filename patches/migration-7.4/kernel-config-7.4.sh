#!/usr/bin/env bash
# 7.4迁移需要的内核配置改动。在scripts/kernel-config-android.sh之后运行，
# 合并时并入该脚本。与它一样在内核源码树根目录下运行：
#   bash <本目录>/kernel-config-7.4.sh <kernel-out-dir>
set -euo pipefail
OUT="${1:?用法: $0 <kernel-out-dir>}"

# ---- binder ----
# v7.4删除了C版binder（fd2bc059ed "binder: rm -f binder.c"），只保留Rust版。
# 原配置中的ANDROID_BINDER_IPC与ANDROID_BINDERFS会被olddefconfig丢弃，
# binderfs已包含在ANDROID_BINDER_IPC_RUST中。缺少binder时，servicemanager与vold
# 启动即中止，init以vold-failed为由重启。
# CONFIG_RUST依赖RUST_IS_AVAILABLE：PATH中缺少rustc、bindgen或rust-src组件时，
# olddefconfig会静默关闭它，因此下面加以断言；原因可用`make rustavailable`查看。
./scripts/config --file "$OUT/.config" --enable RUST --enable ANDROID_BINDER_IPC_RUST \
    --set-str ANDROID_BINDER_DEVICES "binder,hwbinder,vndbinder"

# ---- SCHEDSTATS：Rust绑定中task_struct的布局 ----
# task_struct中的`struct sched_statistics stats`带64字节对齐。关闭SCHEDSTATS时它是空结构体，
# bindgen（0.73.2实测）生成的Rust类型丢失了这一对齐，stats之后的字段（mm、cred、pid、
# tgid、comm、fs……）在Rust中整体前移：开启UCLAMP_TASK时偏移48字节。内核以
# --no-layout-tests调用bindgen，编译期不会报错；Rust binder因此读错mm，mmap返回EINVAL，
# servicemanager无法启动。开启SCHEDSTATS后结构体非空，对齐正确。统计的采集由运行时开关
# 控制（默认关闭），开销可忽略。
./scripts/config --file "$OUT/.config" --enable SCHEDSTATS

CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" O="$OUT" olddefconfig >/dev/null

bad=0
for s in RUST ANDROID_BINDER_IPC_RUST SCHEDSTATS; do
    grep -qx "CONFIG_$s=y" "$OUT/.config" || { echo "  ✗ CONFIG_$s 不是 =y" >&2; bad=1; }
done
grep -qx 'CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"' "$OUT/.config" ||
    { echo "  ✗ CONFIG_ANDROID_BINDER_DEVICES 与预期不符" >&2; bad=1; }
if [ $bad -ne 0 ]; then
    echo "断言失败：Rust工具链不可用时，请先运行 make rustavailable 查看原因。" >&2
    exit 1
fi
echo "== 7.4配置断言通过：RUST、ANDROID_BINDER_IPC_RUST、SCHEDSTATS =y =="
