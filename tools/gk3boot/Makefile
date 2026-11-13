# libgk3core 的主机构建与测试（macOS clang / Linux clang 都能跑）。
#
#   make test            主机单测（ASan + UBSan），向量在 test/vectors/
#   make upstream        与真正的 libboot_control.cpp 逐字节对拍（要 refs/，先跑 scripts/clone-refs.sh）
#   make freestanding    按 UEFI 的条件编 aarch64 目标文件，并断言没有任何外部符号依赖（T9）
#   make gk3-misc        主机版 CLI（dump misc / gpt / boot.img；init = 安装器初始化 misc，S10）
#   make gk3-misc-static Linux 上静态链接的 gk3-misc（手工检查、初始化 misc 用）
#   make misc-test       gk3-misc init 的字节测试（期望值由 Python 独立算，test/misc/run.sh）
#   make hal-test        boot_control HAL 开机完成线程（device/…/boot_control/Gk3Boot.cpp）的主机场景测试（test/hal/run.sh）
#   make postinstall-test OTA postinstall 的选直连条目 + 入口部署一节，mksh/dash/ksh 各跑一遍（test/postinstall/run.sh；先 hal-test 才有交叉核对）
#   make fastbootd       执行端 gk3-fastbootd 的主机版（ASan + UBSan；macOS 上也能编，只是没有 USB / 挂载 ESP）
#   make fastbootd-static  Linux 上静态链接、-O2 的发布形态（scripts/gk3boot/test-fastbootd.sh 在 arm64 容器里编这个）
#   make fbd-unit        执行端里不碰盘的部分（sparse / LP / SHA-256）的主机单测
#   make check           以上全部（fastbootd-static 除外）
#
# 可覆盖：REFS=<refs 目录>  BOOTIMG=<完整 boot.img，用来复算整份 SHA1(id)>

ROOT     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
REFS     ?= $(ROOT)/refs
BOOTIMG  ?= $(wildcard $(ROOT)/out/issues-1791053208/boot.img)
CC       ?= cc
CXX      ?= c++
B        := build

CORE_SRC := $(wildcard core/src/*.c)
WARN     := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
HOSTFLAGS:= -std=c11 -O1 -g $(WARN) -Werror -Icore/include $(SAN)

TEST_SRC := $(wildcard test/*.c)

.PHONY: all test upstream freestanding check clean hal-test postinstall-test fastbootd fastbootd-static fbd-unit gk3-misc-static misc-test
all: check

$(B):
	mkdir -p $(B)

$(B)/test_core: $(CORE_SRC) $(TEST_SRC) core/include/gk3core.h test/t.h | $(B)
	$(CC) $(HOSTFLAGS) -Wno-missing-prototypes -Itest -o $@ $(CORE_SRC) $(TEST_SRC) -lz

test: $(B)/test_core
	GK3_BOOTIMG="$(BOOTIMG)" ./$(B)/test_core test/vectors

# —— 上游对拍 ——
# -include sys/stat.h：libboot_control.cpp:138 用了 struct stat 却没 include（bionic 里是被间接带进来的）
HI       := $(REFS)/aosp-hardware-interfaces/boot/1.1/default/boot_control
REC      := $(REFS)/lineage-bootable-recovery/bootloader_message/include
UPSTREAM_OK := $(and $(wildcard $(HI)/libboot_control.cpp),$(wildcard $(REC)/bootloader_message/bootloader_message.h))

$(B)/core_san.a: $(CORE_SRC) core/include/gk3core.h | $(B)
	rm -rf $(B)/san && mkdir -p $(B)/san
	cd $(B)/san && for f in $(CORE_SRC); do $(CC) $(HOSTFLAGS) -I$(ROOT)/tools/gk3boot/core/include -c $(ROOT)/tools/gk3boot/$$f || exit 1; done
	ar rcs $@ $(B)/san/*.o

$(B)/test_upstream: test/upstream/test_upstream.cpp $(B)/core_san.a | $(B)
	$(CXX) -std=c++17 -O1 -g $(SAN) -Wall -Wno-unused-parameter \
	    -DBOARD_RECOVERY_BLDRMSG_OFFSET=0 -include sys/stat.h \
	    -Itest/upstream/shim -I$(HI)/include -I$(REC) -Icore/include \
	    -o $@ test/upstream/test_upstream.cpp $(HI)/libboot_control.cpp $(B)/core_san.a

upstream:
ifeq ($(UPSTREAM_OK),)
	@echo "SKIP upstream：$(HI) 或 $(REC) 不在（先跑 bash scripts/clone-refs.sh，或 make REFS=…）"
else
	$(MAKE) $(B)/test_upstream
	./$(B)/test_upstream
endif

# —— freestanding：UEFI（aarch64、无 libc、不用 FP/SIMD 寄存器）的编译条件 ——
FS_TARGET ?= aarch64-unknown-none-elf
FSFLAGS  := --target=$(FS_TARGET) -std=c11 -Os -ffreestanding -fno-builtin -nostdlibinc \
            -fno-stack-protector -fshort-wchar -mgeneral-regs-only -fPIC $(WARN) -Werror -Icore/include

freestanding: | $(B)
	rm -rf $(B)/fs && mkdir -p $(B)/fs
	for f in $(CORE_SRC); do $(CC) $(FSFLAGS) -c $$f -o $(B)/fs/$$(basename $$f .c).o || exit 1; done
	@for o in $(B)/fs/*.o; do nm -u $$o; done | awk '{print $$NF}' | sort -u > $(B)/fs/undef.txt; \
	for o in $(B)/fs/*.o; do nm -g --defined-only $$o; done | awk '{print $$NF}' | sort -u > $(B)/fs/def.txt; \
	undef=$$(comm -23 $(B)/fs/undef.txt $(B)/fs/def.txt); \
	if [ -n "$$undef" ]; then echo "freestanding：目标文件依赖外部符号：$$undef"; exit 1; fi; \
	echo "freestanding：$(FS_TARGET) 目标文件 $$(ls $(B)/fs/*.o | wc -l | tr -d ' ') 个，未定义符号全部在库内解决（不依赖 libc / memcpy / memset）"; \
	size=$$(cat $(B)/fs/*.o | wc -c | tr -d ' '); echo "  目标文件合计 $$size 字节"

# —— CLI ——
$(B)/gk3-misc: misc/gk3-misc.c $(CORE_SRC) core/include/gk3core.h | $(B)
	$(CC) -std=c11 -O2 $(WARN) -Werror -Wno-missing-prototypes -Icore/include -o $@ misc/gk3-misc.c $(CORE_SRC)

gk3-misc: $(B)/gk3-misc

# 发布形态（live 镜像，/usr/share/gaokun3/gk3-misc）：静态、去符号，只在 Linux 上编得出来。
# ⚠️ build-rootfs.sh 不经 make（构建容器里没装 make），照抄了这一行 —— 改一边要改另一边
$(B)/gk3-misc.static: misc/gk3-misc.c $(CORE_SRC) core/include/gk3core.h | $(B)
	$(CC) -std=c11 -O2 $(WARN) -Werror -Wno-missing-prototypes -Icore/include -static -o $@ misc/gk3-misc.c $(CORE_SRC)
	strip $@

gk3-misc-static: $(B)/gk3-misc.static

misc-test: $(B)/gk3-misc
	bash test/misc/run.sh

# —— 执行端 gk3-fastbootd（S7a） ——
FBD_SRC  := $(wildcard fastbootd/*.c)
FBD_HDR  := fastbootd/fbd.h core/include/gk3core.h
# -dirty 只看 tools/gk3boot 与 scripts/gk3boot（同 scripts/gk3boot/build-fastboot-img.sh）
FBD_VER  ?= $(shell v=$$(git describe --always --abbrev=12 2>/dev/null || echo unknown); \
              git status --porcelain -- . ../../scripts/gk3boot 2>/dev/null | grep -q . && v=$$v-dirty; echo $$v)
FBD_FLAGS:= -std=gnu11 $(WARN) -Werror -Wno-missing-prototypes -Icore/include -Ifastbootd -DGK3FB_VERSION='"$(FBD_VER)"'

$(B)/gk3-fastbootd: $(FBD_SRC) $(CORE_SRC) $(FBD_HDR) | $(B)
	$(CC) $(FBD_FLAGS) -O1 -g $(SAN) -o $@ $(FBD_SRC) $(CORE_SRC) -lpthread

fastbootd: $(B)/gk3-fastbootd

# 发布形态：静态、去符号。只在 Linux 上（glibc / musl）编得出来。
$(B)/gk3-fastbootd.static: $(FBD_SRC) $(CORE_SRC) $(FBD_HDR) | $(B)
	$(CC) $(FBD_FLAGS) -O2 -static -o $@ $(FBD_SRC) $(CORE_SRC) -lpthread
	strip $@

fastbootd-static: $(B)/gk3-fastbootd.static

$(B)/test_fbd: test/fbd/test_fbd.c fastbootd/sparse.c fastbootd/lp.c $(CORE_SRC) $(FBD_HDR) | $(B)
	$(CC) $(FBD_FLAGS) -O1 -g $(SAN) -Itest -o $@ test/fbd/test_fbd.c fastbootd/sparse.c fastbootd/lp.c $(CORE_SRC)

fbd-unit: $(B)/test_fbd
	$(B)/test_fbd

check: test upstream freestanding gk3-misc misc-test fastbootd fbd-unit

hal-test:
	bash test/hal/run.sh

postinstall-test:
	bash test/postinstall/run.sh

clean:
	rm -rf $(B)
