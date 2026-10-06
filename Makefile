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
    kernel/mm/vmspace.c

# C source files - Drivers
C_SOURCES_DRIVERS = \
    kernel/drivers/timer.c \
    kernel/drivers/serial.c \
    kernel/drivers/keyboard.c \
    kernel/drivers/pci.c \
    kernel/drivers/e1000.c

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
    kernel/fs/tmpfs.c \
    kernel/fs/devfs.c \
    kernel/fs/tar.c

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
    kernel/proc/tree/tree.c

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
    kernel/tests/test_signal.c

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
    kernel/fs/tmpfs.c \
    kernel/fs/tar.c \
    kernel/tty/tty.c \
    kernel/signal/signal.c
COV_OBJECTS = $(COV_SOURCES:.c=.o)
$(COV_OBJECTS): CFLAGS += -fsanitize-coverage=trace-pc

# Phase 18: the initrd is a USTAR archive of the initrd/ directory, loaded by
# GRUB as a multiboot2 module and unpacked into the root tmpfs at boot.
INITRD = initrd.tar
$(INITRD): $(shell find initrd -type f 2>/dev/null)
	@echo "Building initrd.tar"
	@(cd initrd && tar -cf ../$(INITRD) --format=ustar *)

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

.PHONY: all clean run run-debug debug test smoke stress check-license list-sources list-objects check-files size map clean-deps

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

test: $(TEST_ISO)
	@python3 tools/run_tests.py $(TEST_ISO)

# Boot the normal image, type commands through the emulated PS/2 keyboard and
# check what the shell prints (ping over the real e1000, arp, mem, ...).
smoke: $(ISO)
	@python3 tools/shell_smoke.py $(ISO)

# Every source file must carry the AGPL-3.0-only SPDX header.
check-license:
	@bash tools/check_license.sh

# Run the whole suite many times in parallel to flush out timing bugs.
stress: $(TEST_ISO)
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