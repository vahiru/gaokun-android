#!/bin/bash
# Ego Ubuntu 侧：mesa 分析实验室搭建。
# 1) 装构建依赖  2) 拉 mesa-26.0.3 官方源码  3) 打 patches/mesa/0001
# 4) 构建 freedreno decode 工具(crashdec) + turnip  5) 报告路径
set -e
cd /home/user
echo "=== 1. apt 依赖 ==="
sudo apt-get install -y meson ninja-build pkg-config bison flex zlib1g-dev \
  libexpat1-dev libdrm-dev python3-mako python3-yaml libxml2-dev libarchive-dev \
  glslang-tools python3-packaging libwayland-dev wayland-protocols \
  libwayland-egl-backend-dev libx11-dev libxcb1-dev libx11-xcb-dev \
  libxcb-dri2-0-dev libxcb-dri3-dev libxcb-present-dev libxshmfence-dev \
  libxxf86vm-dev libxrandr-dev llvm-dev libclc-dev libclang-dev spirv-tools \
  libvulkan-dev 2>&1 | tail -2

echo "=== 2. mesa 源码 ==="
if [ ! -d mesa-26.0.3 ]; then
  wget -q https://archive.mesa3d.org/mesa-26.0.3.tar.xz
  tar xf mesa-26.0.3.tar.xz
fi
cd mesa-26.0.3
echo "=== 3. 打 0004 补丁 ==="
if [ -f /home/user/0004.patch ] && ! grep -q "vk_image_is_android" src/freedreno/vulkan/tu_device.cc 2>/dev/null; then
  patch -p1 --forward < /home/user/0004.patch || echo "补丁已在或不适用（Ubuntu 无 ANB 路径，可忽略）"
fi

echo "=== 4a. decode 工具构建 ==="
meson setup btools -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
  -Dtools=freedreno -Dglx=disabled -Dbuildtype=release 2>&1 | tail -3
ninja -C btools -j8 2>&1 | tail -3
find btools -name crashdec -type f | head -2

echo "=== 4b. turnip 构建（我们的源码 + gcc/glibc）==="
meson setup bturnip -Dgallium-drivers= -Dvulkan-drivers=freedreno \
  -Dplatforms=x11,wayland -Dglx=disabled -Dbuildtype=release 2>&1 | tail -3
ninja -C bturnip -j8 2>&1 | tail -3
find bturnip -name "libvulkan_freedreno.so" | head -2
find bturnip -name "freedreno_icd*.json" | head -2
echo "=== DONE ==="
