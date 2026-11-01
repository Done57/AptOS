PREFIX ?= $(shell if command -v i686-linux-gnu-gcc >/dev/null 2>&1; then \
                    echo i686-linux-gnu-; else echo /opt/tc/usr/bin/i686-linux-gnu-; fi)

CC      := $(PREFIX)gcc
LD      := $(PREFIX)ld
NASM    ?= nasm
OBJCOPY ?= $(PREFIX)objcopy
PYTHON  ?= python3

BUILD   := build
IMG     := $(BUILD)/os.img
MEM     ?= 128
MINI_MB ?= 8
FLOPPY_KB ?= 1440

CFLAGS  := -m32 -march=i386 -mtune=i386 -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
           -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -mno-sse -mno-mmx -mno-80387 -mno-red-zone \
           -Ikernel
LDFLAGS := -m elf_i386 -T linker.ld --build-id=none

# ---------------------------------------------------------------------------
# 64 位（长模式）构建。
#
# 与 32 位版本并行存在，原因有两个：
#   1. 迁移期间要能随时回到 32 位版本对照行为差异；
#   2. 32 位版本是可用的，64 位版本在完成之前不能拖累它。
#
# 几个不能省的开关：
#   -mcmodel=kernel  把代码+数据限制在地址空间高半区之外的低 2GB 里，
#                    生成的也是 32 位符号扩展寻址，不会插入 PLT。
#   -mno-red-zone    内核不能用红色区域：中断随时会覆盖栈顶以下的内存。
#   -fno-pic         内核是固定地址装载，不要位置无关代码。
# ---------------------------------------------------------------------------
CFLAGS64  := -m64 -mcmodel=kernel -fno-pic -std=gnu11 -O2 -Wall -Wextra \
             -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
             -ffreestanding -fno-stack-protector -fno-builtin -fno-pie \
             -fno-asynchronous-unwind-tables -fno-unwind-tables \
             -mno-sse -mno-mmx -mno-80387 -mno-red-zone \
             -Ikernel
LDFLAGS64 := -m elf_x86_64 -T linker64.ld --build-id=none

KERNEL64_C   := $(wildcard kernel/lm/*.c)
KERNEL64_ASM := $(wildcard kernel/lm/*.asm)
KERNEL64_OBJS := $(BUILD)/entry64.o $(BUILD)/isr64.o \
                 $(patsubst kernel/lm/%.c,$(BUILD)/lm_%.o,$(KERNEL64_C))

# 直接用真实的内核模块（不是副本）：它们本来就写得跨平台，
# 编译成 64 位即可，将来 32 位版本删掉时不用再维护两份。
KERNEL64_SHARED_SRC := kernel/serial.c kernel/util.c kernel/klog.c \
                       kernel/timer.c kernel/keyboard.c
KERNEL64_SHARED := $(patsubst kernel/%.c,$(BUILD)/lm64_%.o,$(KERNEL64_SHARED_SRC))
KERNEL64_OBJS += $(KERNEL64_SHARED)
KERNEL64 := $(BUILD)/kernel64.bin

CSRC    := $(wildcard kernel/*.c)
ASMSRC  := $(wildcard kernel/*.asm)
OBJS    := $(patsubst kernel/%.c,$(BUILD)/%.o,$(CSRC)) \
           $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ASMSRC))

APP_CFLAGS := -m32 -march=i386 -mtune=i386 -std=gnu11 -O2 -Wall -ffreestanding -nostdlib -fno-pie \
              -ffunction-sections -fdata-sections \
              -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
              -fno-unwind-tables -mno-sse -mno-mmx -mno-80387 -Iapps/lib
APPS       := installer hello guess freecell spider aptdemo cube3d
# Regression helper for the zombie-window fix; built on demand, not shipped.
TESTAPPS   := crashtest
# Extra applications that ship as installable .lst packages instead of being
# baked into the disk image.  They appear on the desktop after install.
EXTRA_APPS := imgview sndplay desk
SETUP_APPS := setup

QEMU    := qemu-system-i386
# Devices QEMU puts on the PCI bus for us: an Intel e1000 NIC and an
# Intel HD Audio controller with QEMU's built-in "hda" codec (a duplex
# 48 kHz 16 bit stereo device).  Everything else is default QEMU hardware.
QFLAGS  := -m $(MEM) -vga std \
           -drive file=$(IMG),format=raw,index=0,media=disk \
           -netdev user,id=net0 -device e1000,netdev=net0 \
           -audiodev driver=none,id=snd0 \
           -device intel-hda -device hda-duplex,audiodev=snd0

.PHONY: all run test mini floppy apps pkgs iso clean tools font kernel64 img64

.DEFAULT_GOAL := all

all: $(IMG)

# 单独构建 64 位内核，方便在迁移期间反复验证而不动 32 位镜像
kernel64: $(KERNEL64)
	@echo "64 位内核构建完成: $(KERNEL64)"

# 64 位可启动镜像。--kernel64 只影响 stage2 的一个补丁字节：
# 它决定 stage2 把内核搬到 0x10000（32 位路径）还是 1MB 并进长模式。
# 注意这里不带文件系统 —— 64 位内核还没有磁盘驱动，带也是浪费。
IMG64 := $(BUILD)/os64.img

img64: $(IMG64)
	@echo "64 位镜像: $(IMG64)"

$(IMG64): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL64) tools/mkimg.py
	$(PYTHON) tools/mkimg.py --out $@ --kernel64 \
	    --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	    --kernel $(KERNEL64) --size-mb 8

$(BUILD)/stage1.bin: boot/stage1.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@$(PYTHON) tools/check_kernel.py $@ $(PREFIX)nm
	@echo "--- kernel sections ---"
	@$(PREFIX)objdump -h $@ | awk 'NR>4 && $$2!="" {printf "    %-12s %8d bytes\n", $$2, strtonum("0x"$$3)}' || true

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@echo "kernel.bin: $$(stat -c%s $@) bytes"

# ---------------------------------------------------------------------------
# 64 位内核
# ---------------------------------------------------------------------------
$(BUILD)/lm_%.o: kernel/lm/%.c | $(BUILD)
	$(CC) $(CFLAGS64) -c $< -o $@

$(BUILD)/lm_%.o: kernel/lm/%.asm | $(BUILD)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/entry64.o: kernel/lm/entry64.asm | $(BUILD)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/isr64.o: kernel/lm/isr64.asm | $(BUILD)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/lm64_%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS64) -c $< -o $@

$(KERNEL64).elf: $(KERNEL64_OBJS) linker64.ld
	$(LD) $(LDFLAGS64) -o $@ $(KERNEL64_OBJS)
	@echo "--- 64-bit kernel sections ---"
	@$(PREFIX)readelf -S $@ | awk '/PROGBITS|NOBITS/ {printf "    %-14s %8d bytes @ 0x%s\n", $$2, strtonum("0x"$$6), $$4}' || true

# 必须是平面二进制：stage2 在实模式下用 int 13h 直接把扇区读到 1MB，
# 不认识 ELF 的段头。objcopy -O binary 正好按 VMA 展开，第一个字节
# 就是 _start。
$(KERNEL64): $(KERNEL64).elf
	$(OBJCOPY) -O binary $< $@
	@echo "kernel64.bin: $$(stat -c%s $@) bytes"

$(BUILD)/app_start.o: apps/lib/start.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/app_lib.o: apps/lib/aptos.c | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@

# libapt: the graphics API.  Linked into every program, because a program that
# never calls it costs nothing (the linker drops the unused objects) and one
# that does gets it without extra makefile faff.
$(BUILD)/app_apt.o: apps/lib/apt.c apps/lib/apt.h apps/lib/apt_alloc.h | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/app_alloc.o: apps/lib/apt_alloc.c apps/lib/apt_alloc.h | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@

# The small 3D layer used by the cube test.  Like libapt it is linked into
# every program; if nothing calls it, --gc-sections drops it.
$(BUILD)/app_cube3d.o: apps/lib/cube3d.c apps/lib/cube3d.h | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/%.app: apps/%.c $(BUILD)/app_start.o $(BUILD)/app_lib.o $(BUILD)/app_apt.o $(BUILD)/app_alloc.o $(BUILD)/app_cube3d.o apps/linker.ld
	$(CC) $(APP_CFLAGS) -c $< -o $(BUILD)/$*.app.o
	$(LD) -m elf_i386 -T apps/linker.ld --build-id=none --gc-sections -o $@ \
	    $(BUILD)/app_start.o $(BUILD)/app_lib.o $(BUILD)/app_apt.o $(BUILD)/app_alloc.o $(BUILD)/app_cube3d.o $(BUILD)/$*.app.o
	@echo "  app $@: $$(stat -c%s $@) bytes"

APP_TARGETS := $(addprefix $(BUILD)/,$(addsuffix .app,$(APPS)))
TEST_TARGETS := $(addprefix $(BUILD)/,$(addsuffix .app,$(TESTAPPS)))
EXTRA_TARGETS := $(addprefix $(BUILD)/,$(addsuffix .app,$(EXTRA_APPS)))

$(BUILD)/setup.app: apps/setup.c $(BUILD)/app_start.o $(BUILD)/app_lib.o apps/linker.ld
	$(CC) $(APP_CFLAGS) -c $< -o $(BUILD)/setup.app.o
	$(LD) -m elf_i386 -T apps/linker.ld --build-id=none -o $@ \
	    $(BUILD)/app_start.o $(BUILD)/app_lib.o $(BUILD)/setup.app.o
	@echo "  app $@: $$(stat -c%s $@) bytes"

$(BUILD)/setup.lst: tools/mklst.py $(BUILD)/setup.app
	$(PYTHON) tools/mklst.py --out $@ --name "aptos 2.0 setup" --vendor aptos \
	    --description "system installer" --installer setup \
	    --app setup=$(BUILD)/setup.app

$(BUILD)/SYSTEM.ERT: tools/mkert.py $(BUILD)/kernel.bin $(IMG)
	$(PYTHON) tools/mkert.py --out $@ --disk $(IMG) --kernel $(BUILD)/kernel.bin

$(BUILD)/aptos-install.iso: tools/mkcd.py $(BUILD)/boot.img $(BUILD)/SYSTEM.ERT $(BUILD)/setup.lst
	$(PYTHON) tools/mkcd.py --out $@ --boot $(BUILD)/boot.img --media floppy144 \
	    --volume APTOS_SETUP \
	    --file /KERNEL.ELF=$(BUILD)/kernel.bin \
	    --file /SETUP.LST=$(BUILD)/setup.lst \
	    --file /SYSTEM.ERT=$(BUILD)/SYSTEM.ERT

$(BUILD)/boot.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/mkimg.py
	$(PYTHON) tools/mkimg.py --out $@ --boot-only --size-kb 1440 \
	    --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	    --kernel $(BUILD)/kernel.bin

iso: $(BUILD)/aptos-install.iso

$(BUILD)/apps.lst: tools/mklst.py $(APP_TARGETS)
	$(PYTHON) tools/mklst.py --out $@ --name "aptos starter pack" --vendor aptos \
	    --description "installer + demo apps" --installer installer \
	    $(foreach a,$(APPS),--app $(a)=$(BUILD)/$(a).app)

# --- installable packages -------------------------------------------------
# Each of these is a self-contained .lst the user installs with:
#     install /IMGVIEW.LST      (or /SNDPLAY.LST)
# The package's installer program drops NAME.APP into /APPS, which is what
# makes the icon appear on the desktop.
$(BUILD)/imgview.lst: tools/mklst.py $(BUILD)/imgview.app $(BUILD)/installer.app
	$(PYTHON) tools/mklst.py --out $@ --name "aptos image viewer" --vendor aptos \
	    --description "BMP image viewer" --installer installer \
	    --app installer=$(BUILD)/installer.app \
	    --app imgview=$(BUILD)/imgview.app

$(BUILD)/sndplay.lst: tools/mklst.py $(BUILD)/sndplay.app $(BUILD)/installer.app
	$(PYTHON) tools/mklst.py --out $@ --name "aptos audio player" --vendor aptos \
	    --description "WAV player + tone generator" --installer installer \
	    --app installer=$(BUILD)/installer.app \
	    --app sndplay=$(BUILD)/sndplay.app

$(BUILD)/desktop.lst: tools/mklst.py $(BUILD)/desk.app $(BUILD)/installer.app
	$(PYTHON) tools/mklst.py --out $@ --name "aptos desktop" --vendor aptos \
	    --description "decoupled desktop environment (GUI/TUI)" --installer installer \
	    --app installer=$(BUILD)/installer.app \
	    --app desk=$(BUILD)/desk.app

pkgs: $(BUILD)/imgview.lst $(BUILD)/sndplay.lst $(BUILD)/desktop.lst

apps: $(BUILD)/apps.lst

$(IMG): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/mkimg.py $(BUILD)/apps.lst $(BUILD)/setup.lst $(BUILD)/imgview.lst $(BUILD)/sndplay.lst $(BUILD)/desktop.lst
	$(PYTHON) tools/mkimg.py --out $@ \
	    --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	    --kernel $(BUILD)/kernel.bin --apps $(BUILD)/apps.lst \
	    --add /SETUP.LST=$(BUILD)/setup.lst \
	    --add /IMGVIEW.LST=$(BUILD)/imgview.lst \
	    --add /SNDPLAY.LST=$(BUILD)/sndplay.lst \
	    --add /DESKTOP.LST=$(BUILD)/desktop.lst \
	    $(foreach a,$(APPS),--install $(a)=$(BUILD)/$(a).app)

$(BUILD)/os-mini.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/mkimg.py $(BUILD)/apps.lst
	$(PYTHON) tools/mkimg.py --out $@ \
	    --stage1 $(BUILD)/stage1.bin --stage2 $(BUILD)/stage2.bin \
	    --kernel $(BUILD)/kernel.bin --size-mb $(MINI_MB) \
	    --apps $(BUILD)/apps.lst \
	    $(foreach a,$(APPS),--install $(a)=$(BUILD)/$(a).app)

$(BUILD)/floppy.img: tools/mkimg.py | $(BUILD)
	$(PYTHON) tools/mkimg.py --out $@ --fs-only --size-kb $(FLOPPY_KB) --label FDISK

mini: $(BUILD)/os-mini.img
floppy: $(BUILD)/floppy.img

$(BUILD):
	@mkdir -p $(BUILD)

run: $(IMG)
	$(QEMU) $(QFLAGS) -serial stdio

# Boot with a wav-capture audio backend for headless sound testing.
# Note: -audiodev must come BEFORE -device hda-duplex,audiodev=... on
# QEMU 5+ (there is no default audio backend any more).
run-audiocap: $(IMG)
	$(QEMU) $(filter-out -audiodev%,$(QFLAGS)) -serial stdio \
	    -audiodev driver=wav,id=snd0,path=$(BUILD)/hda-out.wav \
	    -device intel-hda -device hda-duplex,audiodev=snd0

# Where QEMU can find its BIOS.  Add more candidates as needed (the third one
# covers the local toolchain this repository was developed with).
QEMU_BIOS := $(firstword $(foreach d,/opt/tc/usr/share/qemu /usr/share/qemu /tmp/tc/root/usr/share/qemu,$(if $(wildcard $(d)/bios-256k.bin),$(d),)))
QEMU_ARGS ?= $(if $(QEMU_BIOS),-L $(QEMU_BIOS),)

# Rebuild the embedded 16x16 CJK font from GNU Unifont.  The generated header
# kernel/fontcjk.h is committed, so this is only needed after adding new
# Chinese strings to kernel/i18n_str.h or to the user space applications:
#   make font && make
font:
	tools/getunifont.sh

test: $(IMG)
	$(PYTHON) tools/qemu_test.py $(IMG) $(foreach a,$(QEMU_ARGS),--qemu-arg=$(a)) \
	    --step "delay:9" --step "type:ls" --step "key:ret" --step "delay:2" \
	    --step "type:cat HELLO.TXT" --step "key:ret" --step "delay:2" \
	    --step "shot:$(BUILD)/screen.png" --serial $(BUILD)/serial.log

clean:
	rm -rf $(BUILD)

-include $(BUILD)/*.d

# Regression helpers used by the test scripts (not part of the shipped image).
test-apps: $(TEST_TARGETS)
