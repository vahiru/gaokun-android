# gk3w：gaokun3 安装器的 Windows 后端（原型）。
#
#   make test        主机单测（clang + ASan/UBSan）：协议编码、空闲区计算
#   make win-x64     x86_64 版（PC 上对着 VHDX 测）
#   make win-arm64   ARM64 版（平板）
#
# Windows 版用 llvm-mingw 在 Linux 上交叉编译（github.com/mstorsjo/llvm-mingw），LLVM_MINGW 指向解包目录。
# libgk3core（tools/gk3boot/core）按源码编进来：GPT、CRC32、boot.img、misc 的盘上格式只在那里有一份。

LLVM_MINGW ?= $(HOME)/opt/llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64
HOSTCC     ?= clang
B          := build

CORE      := ../../tools/gk3boot/core
CORE_SRC  := $(wildcard $(CORE)/src/*.c)
PORTABLE  := src/proto.c src/layout.c
WIN_SRC   := $(wildcard src/win/*.c)
HEADERS   := $(wildcard src/*.h src/win/*.h) $(CORE)/include/gk3core.h

WARN      := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wno-unused-parameter
SAN       := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
WINFLAGS  := -std=gnu11 -O2 $(WARN) -Werror -municode -DUNICODE -D_UNICODE -I$(CORE)/include -static

.PHONY: all test win-x64 win-arm64 clean
all: test win-x64 win-arm64

test: $(B)/test_host
	$(B)/test_host

$(B)/test_host: test/test_main.c $(PORTABLE) $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O1 -g $(WARN) -Wmissing-prototypes -Werror $(SAN) -o $@ test/test_main.c $(PORTABLE)

win-x64: $(B)/x64/gk3w.exe
win-arm64: $(B)/arm64/gk3w.exe

$(B)/x64/gk3w.exe: $(PORTABLE) $(WIN_SRC) $(CORE_SRC) $(HEADERS)
	@mkdir -p $(@D)
	$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang $(WINFLAGS) -o $@ $(PORTABLE) $(WIN_SRC) $(CORE_SRC) -lrpcrt4

$(B)/arm64/gk3w.exe: $(PORTABLE) $(WIN_SRC) $(CORE_SRC) $(HEADERS)
	@mkdir -p $(@D)
	$(LLVM_MINGW)/bin/aarch64-w64-mingw32-clang $(WINFLAGS) -o $@ $(PORTABLE) $(WIN_SRC) $(CORE_SRC) -lrpcrt4

clean:
	rm -rf $(B)
