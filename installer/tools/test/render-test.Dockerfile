# 图形安装器的运行时：cage + mesa + GTK3 + 中文字体。给 installer/tools/test/test-render.sh 用。
# ★ 它同时是 Debian live 镜像图形部分的预演：这里缺什么，镜像里就会缺什么。
FROM debian@sha256:9cc080028c43b27d2074d63a5f9caf7166d731494965616c1a6d2827a004585c
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      cage wlr-randr grim \
      libgtk-3-0t64 libgl1-mesa-dri libegl1 libegl-mesa0 libgles2 libepoxy0 \
      fonts-wqy-microhei fontconfig procps \
 && rm -rf /var/lib/apt/lists/*
