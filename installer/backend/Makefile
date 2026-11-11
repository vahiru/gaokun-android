# gk3w：gaokun3 安装器的 Windows 后端（原型）。
#
#   make test        主机单测（clang + ASan/UBSan）：协议编码、空闲区、sparse 展开、分区方案、ESP 文本与空间检查
#   make win-x64     x86_64 版（PC 上对着 VHDX 测）
#   make win-arm64   ARM64 版（平板）
#   make host-tools  主机上的辅助工具（build/host/unsparse：用同一份 zstd + sparse 代码把 super.img.zst 展开成文件，
#                    与 gk3-unsparse.py 的结果对拍）
#
# Windows 版用 llvm-mingw 在 Linux 上交叉编译（github.com/mstorsjo/llvm-mingw），LLVM_MINGW 指向解包目录。
# libgk3core（tools/gk3boot/core）按源码编进来：GPT、CRC32、boot.img、misc 的盘上格式只在那里有一份。
# zstd 只取解码部分（third_party/zstd，见 zstd_dec.c 文件头），第三方代码不加 -Werror。

LLVM_MINGW ?= $(HOME)/opt/llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64
HOSTCC     ?= clang
B          := build

CORE      := ../../tools/gk3boot/core
CORE_SRC  := $(wildcard $(CORE)/src/*.c)
PORTABLE  := src/proto.c src/layout.c src/sparse.c src/plan.c src/esp.c
WIN_SRC   := $(wildcard src/win/*.c)
HEADERS   := $(wildcard src/*.h src/win/*.h) $(CORE)/include/gk3core.h
ZSTD      := third_party/zstd/zstd_dec.c
ZSTD_INC  := -Ithird_party/zstd/lib

WARN      := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wno-unused-parameter
SAN       := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
WINFLAGS  := -std=gnu11 -O2 $(WARN) -Werror -municode -DUNICODE -D_UNICODE -I$(CORE)/include $(ZSTD_INC)
ZSTDFLAGS := -O2 -w
WINLIBS   := -static -lrpcrt4 -lbcrypt

.PHONY: all test win-x64 win-arm64 host-tools clean
all: test win-x64 win-arm64 host-tools

test: $(B)/test_host $(B)/test_stage2
	$(B)/test_host
	$(B)/test_stage2

$(B)/test_host: test/test_main.c $(PORTABLE) $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O1 -g $(WARN) -Wmissing-prototypes -Werror $(SAN) -o $@ test/test_main.c $(PORTABLE)

$(B)/test_stage2: test/test_stage2.c $(PORTABLE) $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O1 -g $(WARN) -Wmissing-prototypes -Werror $(SAN) -o $@ test/test_stage2.c $(PORTABLE)

host-tools: $(B)/host/unsparse

$(B)/host/zstd.o: $(ZSTD)
	@mkdir -p $(@D)
	$(HOSTCC) $(ZSTDFLAGS) $(ZSTD_INC) -c -o $@ $<

$(B)/host/unsparse: test/unsparse_host.c src/sparse.c src/sparse.h $(B)/host/zstd.o
	$(HOSTCC) -std=c11 -O2 $(WARN) -Werror $(ZSTD_INC) -o $@ test/unsparse_host.c src/sparse.c $(B)/host/zstd.o

win-x64: $(B)/x64/gk3w.exe
win-arm64: $(B)/arm64/gk3w.exe

$(B)/x64/zstd.o: $(ZSTD)
	@mkdir -p $(@D)
	$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang $(ZSTDFLAGS) $(ZSTD_INC) -c -o $@ $<

$(B)/arm64/zstd.o: $(ZSTD)
	@mkdir -p $(@D)
	$(LLVM_MINGW)/bin/aarch64-w64-mingw32-clang $(ZSTDFLAGS) $(ZSTD_INC) -c -o $@ $<

$(B)/x64/gk3w.exe: $(PORTABLE) $(WIN_SRC) $(CORE_SRC) $(HEADERS) $(B)/x64/zstd.o
	$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang $(WINFLAGS) -o $@ $(PORTABLE) $(WIN_SRC) $(CORE_SRC) $(B)/x64/zstd.o $(WINLIBS)

$(B)/arm64/gk3w.exe: $(PORTABLE) $(WIN_SRC) $(CORE_SRC) $(HEADERS) $(B)/arm64/zstd.o
	$(LLVM_MINGW)/bin/aarch64-w64-mingw32-clang $(WINFLAGS) -o $@ $(PORTABLE) $(WIN_SRC) $(CORE_SRC) $(B)/arm64/zstd.o $(WINLIBS)

clean:
	rm -rf $(B)
