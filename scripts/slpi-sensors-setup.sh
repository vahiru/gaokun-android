#!/usr/bin/env bash
# SLPI 传感器通路搭建（Linux 侧）—— 在 Ego 的救援 Ubuntu 上跑，不是在构建机上。
#
# 干什么：把加速度计读通。整条链路是
#   fastrpc → hexagonrpcd(VFS) → SLPI 上的 DSP 注册表 → SSC → QMI → libssc
# AP 侧没有任何到传感器芯片的总线，所以只能让 AP 给 DSP 当文件服务器。
# 背景与踩坑全记录见 docs/dev/sensors-ssc-protocol.md。
#
# 用法：
#   bash slpi-sensors-setup.sh /path/to/extracted-cabs
#
# 那个目录要含（从 uup-drivers-sc8280xp 的 release 解包，见
# device/huawei/gaokun3/firmware/README.md；★华为专有，不入版本库）：
#   *.json            传感器配置          ← qcSensorsConfigQrd8280.cab
#   sns_reg_config    DSP 注册表索引      ← 同上，★必须是 407 B 文本格式
#   RSCS.bin          SLPI 伴生固件       ← qcsubsys_ext_scss8280.cab
#
# 实测状态（2026-08-20）：加速度计 ✅ Z≈9.87 m/s²；光感 ❌ 使能即污染会话。
set -euo pipefail

SRC=${1:?用法: $0 <解包后的 cab 目录>}
P=/usr/share/qcom/sc8280xp/HUAWEI/GAOKUN3
say(){ printf '\n=== %s\n' "$*"; }

[ "$(id -u)" -eq 0 ] || { echo "要 root（sudo bash $0 ...）"; exit 1; }

say "0/8 前提检查"
grep -q . /sys/class/remoteproc/remoteproc0/name || { echo "没有 remoteproc"; exit 1; }
for d in /sys/class/remoteproc/*/; do
    [ "$(cat "$d/name")" = slpi ] && [ "$(cat "$d/state")" = running ] && SLPI=ok
done
[ "${SLPI:-}" = ok ] || { echo "SLPI 不在 running"; exit 1; }

say "1/8 fastrpc"
# ⚠️ 内核里是 =m 且 Android 那棵树不发模块 —— 见 #37。这里是救援 Linux，有模块。
modprobe fastrpc 2>/dev/null || true
[ -e /dev/fastrpc-sdsp ] || { echo "/dev/fastrpc-sdsp 没出现，fastrpc 没加载"; exit 1; }
echo fastrpc > /etc/modules-load.d/fastrpc.conf     # 重启后自动加载

say "2/8 挡掉 droid-juicer"
# ⚠️ 0.4.2 有个无限 openat("/usr/share/droid-juicer/configs") 死循环，会把 apt 卡死。
systemctl disable --now droid-juicer 2>/dev/null || true
systemctl mask droid-juicer 2>/dev/null || true

say "3/8 依赖"
apt-get install -y --no-install-recommends \
    meson ninja-build git libglib2.0-dev libqmi-glib-dev libprotobuf-c-dev \
    libqrtr-glib-dev libjson-c-dev protobuf-c-compiler protobuf-compiler \
    qrtr-tools dos2unix hexagonrpcd

say "4/8 VFS 根目录 + socinfo"
mkdir -p "$P"/{sensors/config,sensors/registry,acdb,dsp/sdsp,socinfo}
# 这些值三方一致：内核 /sys/devices/soc0/soc_id=449、JSON 里 soc_id=["449"]、
# cab 里 socinfo 原件逐字相同。别改。
printf 'QRD\n'     > "$P/socinfo/hw_platform"
printf 'Unknown\n' > "$P/socinfo/platform_subtype"
printf '0\n'       > "$P/socinfo/platform_subtype_id"
printf '65536\n'   > "$P/socinfo/platform_version"
printf '449\n'     > "$P/socinfo/soc_id"
printf '3.1\n'     > "$P/socinfo/revision"
# ★ DSP 固件在 Windows 上编译，请求的路径**每一段都带尾随 \r**。
#   ⚠️ 贡献者指南在这里建了 6 个带 \r 的 symlink（理由是 socinfo 走真实文件系统，
#   symlink 就够）。**本仓实测证明那是多余的**：下面那个补丁打在
#   copy_segment_and_advance() 里，那是【通用】分段解析函数，清理后的 segment 才
#   分派给各后端（hexagonfs.c:179），所以物理目录后端同样受益。
#   实测把 6 个 symlink 全部移走，加速度计照样 Z≈9.86 正常读数。
#   → 这条同时让 Android 侧能用普通 PRODUCT_COPY_FILES（造不出带控制字符的文件名）。

say "5/8 传感器文件"
cp "$SRC"/*.json          "$P/sensors/config/"
cp "$SRC/sns_reg_config"  "$P/sensors/sns_reg.conf"
cp "$SRC/RSCS.bin"        "$P/dsp/sdsp/"
# ★ registry 必须是【空文件】：DSP 找不到覆盖值就用默认值（= 全部传感器启用）。
#   ⚠️ 别用 sscregistrygen 预生成 —— 实测 142 个文件会把加速度计一起弄坏，
#   挪走后当场恢复（干净 A/B，见 #37）。
: > "$P/sensors/registry/registry"
# JSON 是从 Windows 来的 CRLF
find "$P" -type f -exec dos2unix -q {} \; 2>/dev/null || true
[ "$(stat -c%s "$P/sensors/sns_reg.conf")" -lt 500 ] \
    || echo "⚠️ sns_reg.conf 超过 500 B —— 可能拿成了 DriverData 的 JSON 格式（会让 DSP 注册表初始化崩溃）"
chown -R fastrpc:fastrpc "$P"
mkdir -p /var/lib/hexagonrpc/persist/sensors/registry
chown -R fastrpc:fastrpc /var/lib/hexagonrpc

say "6/8 自编 hexagonrpcd（apt 版不带 \r 截断补丁）"
SRCDIR=${HEXAGONRPC_SRC:-/root/src/hexagonrpc}
[ -d "$SRCDIR" ] || git clone --depth=1 https://github.com/linux-msm/hexagonrpc.git "$SRCDIR"
cd "$SRCDIR"
# hexagonfs 的内部 VFS 自己解析路径，不经过内核 symlink → 在每段末尾截掉 \r
# 幂等守卫用 -F 定位补丁独有的这行（不含反斜杠，避开 bash/grep 双层转义）
if ! grep -qF 'segment[--segment_len] = 0;' hexagonrpcd/hexagonfs.c; then
    python3 - <<'PY'
import io
p='hexagonrpcd/hexagonfs.c'
s=io.open(p,encoding='utf-8').read()
anchor='segment[segment_len] = 0;'
bs=chr(92)
patch=('\n\tif (segment_len > 0 && segment[segment_len - 1] == '
       "'"+bs+"r'"+')\n\t\tsegment[--segment_len] = 0;')
assert anchor in s, '锚点没找到，上游改过了'
s=s.replace(anchor, anchor+patch, 1)
io.open(p,'w',encoding='utf-8',newline='\n').write(s)
print('  已打补丁')
PY
else
    echo "  补丁已在"
fi
meson setup build --wipe -Dhexagonrpcd_verbose=false
ninja -C build
install -m755 build/hexagonrpcd/hexagonrpcd /usr/libexec/hexagonrpc/hexagonrpcd
# ⚠️ 必须连 .so 一起装：apt 装的是 0.4，自编的是 0.5，只换二进制会
#    "error while loading shared libraries: libhexagonrpc.so.0.5"
find build -name 'libhexagonrpc.so*' -exec cp -a {} /usr/lib/ \; 2>/dev/null || true
ldconfig

say "7/8 systemd override"
# 现成的 shell wrapper 不认识 sc8280xp，会 fallback 到错误的 DSP → 直接调二进制
mkdir -p /etc/systemd/system/hexagonrpcd.service.d
cat > /etc/systemd/system/hexagonrpcd.service.d/override.conf <<EOF
[Service]
ExecStart=
ExecStart=/usr/libexec/hexagonrpc/hexagonrpcd -f /dev/fastrpc-sdsp -d sdsp -s -R $P
ProtectSystem=full
ReadWritePaths=/var/lib/hexagonrpc
EOF
systemctl daemon-reload
systemctl enable --now hexagonrpcd
systemctl restart hexagonrpcd

say "8/8 libssc + ssccli"
LSRC=${LIBSSC_SRC:-/root/src/libssc}
[ -d "$LSRC" ] || git clone --depth=1 https://codeberg.org/dylanvanassche/libssc.git "$LSRC"
cd "$LSRC"
# ⚠️ 指南里的 -Dmocking=disabled 已被上游删掉，加了会报 "Unknown option"
meson setup build --wipe
ninja -C build
ninja -C build install
ldconfig

say "验证"
qrtr-lookup 2>/dev/null | grep -E '\s400\s' || echo "⚠️ QRTR 服务 400 不在"
# ⚠️ 重启后需要沉降时间：6 秒就读实测拿到 0 行，等久一点才有读数。
echo "等待 SSC 沉降 …"; sleep 20
if timeout 25 ssccli --sensor accelerometer 2>&1 | grep -m3 measurement; then
    echo
    echo "✅ 加速度计通了（静止时 Z 应 ≈ 9.8 m/s²）"
else
    echo "⚠️ 还没读到。再等一会儿重试 'ssccli --sensor accelerometer'；"
    echo "   仍不行就 'systemctl restart hexagonrpcd' 再等 20 秒。"
    echo "   ⚠️ 别先去试 --sensor light：它会污染整个 SSC 会话（#37）。"
fi
