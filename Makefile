# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
# =============================================================================
# MakhOS Makefile
# =============================================================================
# MakhOS - A simple 64-bit operating system kernel
#
# Phase 9 Change: Added process management support
#   - Added kernel/arch/context_switch.asm to ASM_SOURCES
# Phase 11/12/13: Split proc.c into multiple modules
#   - Added kernel/proc/core/core.c
#   - Added kernel/proc/list/list.c
#   - Added kernel/proc/pid/pid.c
#   - Added kernel/proc/create/create.c
#   - Added kernel/proc/sched/sched.c
#   - Added kernel/proc/exit/exit.c
#   - Added kernel/proc/table/table.c
#   - Added kernel/proc/tree/tree.c
# =============================================================================

# Toolchain
CC = gcc
AS = nasm
LD = ld

# Flags
# NOTE: a kernel must not use the SysV red zone or SSE/MMX (no FPU state saved
# across interrupts), and needs frame pointers for reliable backtraces.
CFLAGS  = -ffreestanding -fno-pie -O2 -Wall -Wextra
CFLAGS += -std=gnu99 -fno-stack-protector -nostdinc
CFLAGS += -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only
CFLAGS += -fno-omit-frame-pointer
# Higher-half kernel (F21 Path A): the kernel is linked in the top -2GB
# (0xFFFFFFFF80000000+, linker.ld KERNEL_VMA), so code uses the kernel model.
CFLAGS += -mcmodel=kernel
CFLAGS += -I kernel/include
ASFLAGS = -f elf64
LDFLAGS = -T linker.ld -nostdlib -z noexecstack --no-warn-execstack

# =============================================================================
# SOURCE FILES
# =============================================================================

# Assembly source files
ASM_SOURCES = \
    boot/boot.asm \
    kernel/mm/paging_asm.asm \
    kernel/arch/idt_asm.asm \
    kernel/arch/usermode_asm.asm \
    kernel/arch/context_switch.asm \
    kernel/kfuzz/jmp.asm

# C source files - Original kernel files
C_SOURCES_ORIG = \
    kernel/kernel.c \
    kernel/vga.c \
    kernel/multiboot.c \
    kernel/input_line.c \
    kernel/lib/string.c \
    kernel/klog.c \
    kernel/cmdline.c \
    kernel/ktest.c \
    kernel/panic.c \
    kernel/ktime.c \
    kernel/krandom.c \
    kernel/futex.c \
    kernel/pthread/pthread.c \
    kernel/pthread/sem.c \
    kernel/syscall/syscall.c \
    kernel/shell/shell.c \
    kernel/kfuzz/kfuzz.c \
    kernel/kfuzz/kfuzz_targets.c

# C source files - Architecture
C_SOURCES_ARCH = \
    kernel/arch/idt.c \
    kernel/arch/pic.c \
    kernel/arch/gdt.c \
    kernel/arch/tss.c \
    kernel/arch/debugreg.c \
    kernel/arch/usermode.c \
    kernel/arch/uaccess.c

# C source files - Memory Management
C_SOURCES_MM = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kheap.c \
    kernel/mm/page.c \
    kernel/mm/vmspace.c \
    kernel/mm/uvm.c

# C source files - Drivers
C_SOURCES_DRIVERS = \
    kernel/drivers/timer.c \
    kernel/drivers/serial.c \
    kernel/drivers/keyboard.c \
    kernel/drivers/pci.c \
    kernel/drivers/e1000.c \
    kernel/drivers/rtc.c \
    kernel/drivers/virtio_blk.c

# C source files - Network stack (Phase 14)
C_SOURCES_NET = \
    kernel/net/netdev.c \
    kernel/net/eth.c \
    kernel/net/arp.c \
    kernel/net/ipv4.c \
    kernel/net/icmp.c \
    kernel/net/udp.c \
    kernel/net/tcp.c \
    kernel/net/socket.c

# C source files - Filesystem (Phase 18)
C_SOURCES_FS = \
    kernel/fs/vfs.c \
    kernel/fs/path.c \
    kernel/fs/tmpfs.c \
    kernel/fs/devfs.c \
    kernel/fs/tar.c \
    kernel/fs/ext2.c

# C source files - Signals & TTY (Phase 19)
C_SOURCES_SIG = \
    kernel/signal/signal.c \
    kernel/tty/tty.c

# C source files - Process Management (Split into modules)
C_SOURCES_PROC = \
    kernel/proc/core/core.c \
    kernel/proc/list/list.c \
    kernel/proc/pid/pid.c \
    kernel/proc/create/create.c \
    kernel/proc/sched/sched.c \
    kernel/proc/exit/exit.c \
    kernel/proc/table/table.c \
    kernel/proc/tree/tree.c \
    kernel/proc/elf.c \
    kernel/proc/user.c

# C source files - In-kernel tests (registered via the .ktests section)
C_SOURCES_TESTS = \
    kernel/tests/test_lib.c \
    kernel/tests/test_mm.c \
    kernel/tests/test_klog.c \
    kernel/tests/test_sched.c \
    kernel/tests/test_pthread.c \
    kernel/tests/test_net.c \
    kernel/tests/test_shell.c \
    kernel/tests/test_kfuzz.c \
    kernel/tests/test_user.c \
    kernel/tests/test_vm.c \
    kernel/tests/test_vfs.c \
    kernel/tests/test_signal.c \
    kernel/tests/test_proc.c \
    kernel/tests/test_uvm.c \
    kernel/tests/test_path.c \
    kernel/tests/test_fork.c \
    kernel/tests/test_term.c \
    kernel/tests/test_args.c \
    kernel/tests/test_job.c \
    kernel/tests/test_usignal.c \
    kernel/tests/test_tls.c \
    kernel/tests/test_time.c \
    kernel/tests/test_stat.c \
    kernel/tests/test_pipe.c \
    kernel/tests/test_thread.c \
    kernel/tests/test_rtc.c \
    kernel/tests/test_iov.c \
    kernel/tests/test_musl.c \
    kernel/tests/test_busybox.c \
    kernel/tests/test_virtio_blk.c \
    kernel/tests/test_ext2.c \
    kernel/tests/test_g3.c \
    kernel/tests/test_f21.c

# Combine all C sources
C_SOURCES = \
    $(C_SOURCES_ORIG) \
    $(C_SOURCES_ARCH) \
    $(C_SOURCES_MM) \
    $(C_SOURCES_DRIVERS) \
    $(C_SOURCES_PROC) \
    $(C_SOURCES_NET) \
    $(C_SOURCES_FS) \
    $(C_SOURCES_SIG) \
    $(C_SOURCES_TESTS)

# =============================================================================
# OBJECT FILES
# =============================================================================

# KFUZZ coverage: instrument only the subsystems the fuzzer attacks, so the
# coverage map measures the code under test (not the harness, arch or sched).
COV_SOURCES = \
    kernel/mm/kheap.c \
    kernel/mm/pmm.c \
    kernel/mm/page.c \
    kernel/mm/vmspace.c \
    kernel/mm/uvm.c \
    kernel/lib/string.c \
    kernel/shell/shell.c \
    kernel/net/eth.c \
    kernel/net/arp.c \
    kernel/net/ipv4.c \
    kernel/net/icmp.c \
    kernel/net/udp.c \
    kernel/net/tcp.c \
    kernel/net/socket.c \
    kernel/fs/vfs.c \
    kernel/fs/path.c \
    kernel/fs/tmpfs.c \
    kernel/fs/tar.c \
    kernel/tty/tty.c \
    kernel/signal/signal.c \
    kernel/proc/elf.c
COV_OBJECTS = $(COV_SOURCES:.c=.o)
$(COV_OBJECTS): CFLAGS += -fsanitize-coverage=trace-pc

# Phase 18/20-A: the initrd is a USTAR archive (GRUB module) unpacked into the
# root tmpfs at boot. It bundles the static initrd/ content plus the user-space
# ELF programs built from user/.
INITRD = initrd.tar

# User programs: freestanding static ELF64 linked into the per-process region.
UCC      = $(CC)
UCFLAGS  = -ffreestanding -nostdlib -fno-pie -mno-red-zone -mcmodel=large -O2 -Wall -Iuser
ULDFLAGS = -T user/user.ld -nostdlib -no-pie -z noexecstack
USER_BINS = build/user/hello build/user/getpid build/user/spin build/user/faulter \
            build/user/vmtest build/user/mprotfault build/user/cwdtest \
            build/user/forktest build/user/forkexec build/user/forkcow \
            build/user/readline build/user/sh build/user/echo \
            build/user/execargs build/user/spinner build/user/jobtest \
            build/user/sigtest build/user/sigmask build/user/tlstest \
            build/user/timetest build/user/statls build/user/pipetest \
            build/user/countin build/user/threadtest build/user/clktest \
            build/user/iovtest build/user/lowexec

build/user/start.o: user/start.S
	@mkdir -p build/user
	@$(UCC) $(UCFLAGS) -c -o $@ $<

build/user/%: user/%.c user/usys.h build/user/start.o user/user.ld
	@mkdir -p build/user
	@$(UCC) $(UCFLAGS) -c -o build/user/$*.o $<
	@$(LD) $(ULDFLAGS) -o $@ build/user/start.o build/user/$*.o

# A standard non-PIE ET_EXEC linked at the conventional low 0x400000 (slot 0),
# to prove the lower half belongs to the process after the higher-half migration.
build/user/lowexec: user/lowexec.c user/usys.h build/user/start.o user/user_low.ld
	@mkdir -p build/user
	@$(UCC) $(UCFLAGS) -c -o build/user/lowexec.o $<
	@$(LD) -T user/user_low.ld -nostdlib -no-pie -z noexecstack -o $@ build/user/start.o build/user/lowexec.o

# Phase 20-O (F20): a real-libc program. hello.c is an ordinary C program built
# against musl as a static-PIE using the pip `ziglang` toolchain, which vendors
# the musl source. The linked binary is checked in beside its source so the
# normal build (and CI) need no extra toolchain — it ships into the initrd as
# /bin/muslhello. Regenerate it with `make musl-progs` after editing hello.c.
MUSL_PREBUILT = user/musl/muslhello
# Phase 20-S (G3): musl programs for file I/O on ext2 and execve-envp.
MUSL_FIO = user/musl/fio
MUSL_ENVTEST = user/musl/envtest
# Phase 20-O2 (F20-c): a real busybox, built from source as a static-PIE against
# musl with the ziglang toolchain and checked in (see docs/PHASE20O2_BUSYBOX.md
# for the exact recipe). It ships into the initrd as /bin/busybox.
BUSYBOX_PREBUILT = user/musl/busybox
# Phase 21 (F21, self-hosting): a tinycc built as a static-PIE against musl with
# the ziglang toolchain and checked in (see user/tcc/README.md and
# docs/PHASE21_HIGHERHALF.md). It runs on MAKH and compiles C to a standard
# non-PIE ET_EXEC at 0x400000 — which the higher-half kernel now loads.
TCC_PREBUILT = user/tcc/tcc
ZIGCC     ?= python3 -m ziglang cc
ZIGCFLAGS  = -target x86_64-linux-musl -fPIE -pie -static -Os -Wl,-s -Wall

$(INITRD): $(shell find initrd user/sysroot -type f 2>/dev/null) $(USER_BINS) $(MUSL_PREBUILT) $(MUSL_FIO) $(MUSL_ENVTEST) $(BUSYBOX_PREBUILT) $(TCC_PREBUILT) user/tcc/hello.c user/tcc/full.c
	@echo "Building initrd.tar (+ user programs + tcc sysroot)"
	@rm -rf build/initrd && mkdir -p build/initrd/bin build/initrd/share
	@cp -r initrd/. build/initrd/
	@cp $(USER_BINS) build/initrd/bin/
	@cp $(MUSL_PREBUILT) build/initrd/bin/muslhello
	@cp $(MUSL_FIO) build/initrd/bin/fio
	@cp $(MUSL_ENVTEST) build/initrd/bin/envtest
	@cp $(BUSYBOX_PREBUILT) build/initrd/bin/busybox
	@cp $(TCC_PREBUILT) build/initrd/bin/tcc
	@cp user/tcc/hello.c build/initrd/share/tcc-hello.c
	@cp user/tcc/full.c build/initrd/share/tcc-full.c
	@cp -r user/sysroot/usr build/initrd/usr          # musl headers + libc.a + crt + libtcc1.a
	@(cd build/initrd && tar -cf $(abspath $(INITRD)) --format=ustar *)

# Rebuild the checked-in musl program(s) from source. Needs `pip install ziglang`.
musl-progs:
	@$(ZIGCC) --version >/dev/null 2>&1 || { echo "need the ziglang toolchain: pip install ziglang"; exit 1; }
	@echo "Building musl programs with zig $$($(ZIGCC) --version)"
	$(ZIGCC) $(ZIGCFLAGS) user/musl/hello.c -o $(MUSL_PREBUILT)
	$(ZIGCC) $(ZIGCFLAGS) user/musl/fio.c -o $(MUSL_FIO)
	$(ZIGCC) $(ZIGCFLAGS) user/musl/envtest.c -o $(MUSL_ENVTEST)
	@file $(MUSL_PREBUILT) $(MUSL_FIO) $(MUSL_ENVTEST)

ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)
C_OBJECTS   = $(C_SOURCES:.c=.o)
OBJECTS     = $(ASM_OBJECTS) $(C_OBJECTS)

# =============================================================================
# OUTPUT
# =============================================================================

KERNEL = makhos.kernel
ISO    = makhos.iso

# =============================================================================
# BUILD TARGETS
# =============================================================================

.PHONY: all clean run run-debug debug test smoke stress check-license list-sources list-objects check-files size map clean-deps musl-progs

all: $(KERNEL) $(ISO)

# Link kernel
$(KERNEL): $(OBJECTS)
	@echo "LD  $@"
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

# Assemble .asm files
%.o: %.asm
	@echo "AS  $<"
	$(AS) $(ASFLAGS) -o $@ $<

# Compile .c files
%.o: %.c
	@echo "CC  $<"
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Create ISO
$(ISO): $(KERNEL) $(INITRD)
	@echo "Creating ISO..."
	@mkdir -p isodir/boot/grub
	@cp grub.cfg isodir/boot/grub/
	@cp $(KERNEL) isodir/boot/
	@cp $(INITRD) isodir/boot/
	@grub-mkrescue -o $(ISO) isodir 2>/dev/null || \
		(echo "Note: grub-mkrescue not found, copying kernel as ISO" && \
		 cp $(KERNEL) $(ISO))
	@rm -rf isodir
	@echo "ISO created: $(ISO)"

# =============================================================================
# UTILITY TARGETS
# =============================================================================

list-sources:
	@echo "=== Assembly Sources ==="
	@for f in $(ASM_SOURCES); do echo "  $$f"; done
	@echo "=== C Sources ==="
	@for f in $(C_SOURCES); do echo "  $$f"; done
	@echo "Total C files:   $(words $(C_SOURCES))"
	@echo "Total ASM files: $(words $(ASM_SOURCES))"

list-objects:
	@echo "=== Object Files ==="
	@for f in $(OBJECTS); do echo "  $$f"; done
	@echo "Total objects: $(words $(OBJECTS))"

clean:
	@echo "Cleaning..."
	@rm -f $(OBJECTS) $(C_SOURCES:.c=.d) $(KERNEL) $(ISO) $(TEST_ISO)
	@rm -rf isodir isodir-test
	@echo "Clean complete."

# =============================================================================
# RUN TARGETS
# =============================================================================

run: $(ISO)
	@echo "Running MakhOS in QEMU..."
	@rm -f serial.log
	qemu-system-x86_64 -cdrom $(ISO) -vga std -m 128M -no-reboot -serial file:serial.log

# -----------------------------------------------------------------------------
# TEST TARGETS
# -----------------------------------------------------------------------------
# Build a bootable ISO whose GRUB command line contains "makh.test", so the
# kernel runs its self-test suite headless and powers off with a pass/fail
# exit code. tools/run_tests.py builds this ISO, runs QEMU, and interprets
# the result. Used locally (`make test`) and in CI.

TEST_ISO = makhos-test.iso

$(TEST_ISO): $(KERNEL) grub-test.cfg $(INITRD)
	@echo "Creating test ISO..."
	@mkdir -p isodir-test/boot/grub
	@cp grub-test.cfg isodir-test/boot/grub/grub.cfg
	@cp $(KERNEL) isodir-test/boot/
	@cp $(INITRD) isodir-test/boot/
	@grub-mkrescue -o $(TEST_ISO) isodir-test 2>/dev/null
	@rm -rf isodir-test
	@echo "Test ISO created: $(TEST_ISO)"

# Phase 20-P/Q: the disk images the storage KTESTs use. testdisk.img is the raw
# image the virtio-blk test reads/writes; ext2.img is a real ext2 filesystem the
# ext2 test mounts and reads. Both are attached to QEMU by tools/run_tests.py.
TESTDISK = build/testdisk.img
EXT2IMG  = build/ext2.img
$(TESTDISK): tools/mkdisk.py
	@mkdir -p build
	@python3 tools/mkdisk.py $(TESTDISK)
$(EXT2IMG): tools/mkext2.sh
	@mkdir -p build
	@bash tools/mkext2.sh $(EXT2IMG)

# The storage KTESTs need these disks attached, and tools/run_tests.py (run
# directly by CI as well as by `make test`) attaches whatever exists. Tie them
# to the test ISO so *any* build of it also produces the disks — CI builds only
# `makhos-test.iso`, so without this the disk-backed tests boot with no disk.
$(TEST_ISO): $(TESTDISK) $(EXT2IMG)

test: $(TEST_ISO) $(TESTDISK) $(EXT2IMG)
	@python3 tools/run_tests.py $(TEST_ISO) --timeout 240

# Boot the normal image, type commands through the emulated PS/2 keyboard and
# check what the shell prints (ping over the real e1000, arp, mem, ...).
smoke: $(ISO)
	@python3 tools/shell_smoke.py $(ISO)

# Every source file must carry the AGPL-3.0-only SPDX header.
check-license:
	@bash tools/check_license.sh

# Run the whole suite many times in parallel to flush out timing bugs.
stress: $(TEST_ISO) $(TESTDISK) $(EXT2IMG)
	@bash tools/stress_tests.sh 24 4 $(TEST_ISO)

run-debug: $(ISO)
	@echo "Running MakhOS in QEMU with debug output..."
	@rm -f serial.log
	qemu-system-x86_64 -cdrom $(ISO) -vga std -m 128M -no-reboot \
		-serial file:serial.log -d int,cpu_reset -D qemu.log

debug: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -vga std -m 128M -no-reboot \
		-s -S -serial file:serial.log &
	@echo "QEMU running with GDB stub on localhost:1234"
	@echo "Run: gdb -ex 'file $(KERNEL)' -ex 'target remote localhost:1234'"

# =============================================================================
# CHECK TARGETS
# =============================================================================

check-files:
	@echo "Checking for required files..."
	@for f in $(ASM_SOURCES) $(C_SOURCES); do \
		if [ ! -f $$f ]; then \
			echo "ERROR: Missing file $$f"; exit 1; \
		fi \
	done
	@echo "All required files present."

size: $(KERNEL)
	@echo "Kernel size:"
	@size $(KERNEL)

map: $(KERNEL)
	@echo "Symbol map:"
	@nm $(KERNEL) | sort

# =============================================================================
# DEPENDENCY GENERATION
# =============================================================================

# Header dependencies are generated by the compiler itself (-MMD -MP in the
# compile rule), which writes kernel/dir/file.d next to kernel/dir/file.o with
# the FULL object path as the target.
#
# The previous hand-rolled %.d rule ran `gcc -MM` without -MT, so it emitted
# targets like "table.o:" instead of "kernel/proc/table/table.o:" - header edits
# never rebuilt objects in subdirectories. That silently mixed old and new
# struct layouts (a stale table.o allocated 464-byte PCBs while new code wrote
# 488 bytes into them) and was caught by the scheduler churn test.

-include $(C_SOURCES:.c=.d)

clean-deps:
	@rm -f $(C_SOURCES:.c=.d)