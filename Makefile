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
CFLAGS = -ffreestanding -fno-pie -O2 -Wall -Wextra
CFLAGS += -std=gnu99 -fno-stack-protector -nostdinc
CFlAGS += -mno-red-zone
CFLAGS += -I kernel/include
ASFLAGS = -f elf64
LDFLAGS = -T linker.ld -nostdlib

# =============================================================================
# SOURCE FILES
# =============================================================================

# Assembly source files
ASM_SOURCES = \
    boot/boot.asm \
    kernel/mm/paging_asm.asm \
    kernel/arch/idt_asm.asm \
    kernel/arch/syscall_asm.asm \
    kernel/arch/context_switch.asm

# C source files - Original kernel files
C_SOURCES_ORIG = \
    kernel/kernel.c \
    kernel/vga.c \
    kernel/multiboot.c \
    kernel/input_line.c \
    kernel/lib/string.c \
    kernel/syscall/syscall.c

# C source files - Architecture
C_SOURCES_ARCH = \
    kernel/arch/idt.c \
    kernel/arch/pic.c \
    kernel/arch/gdt.c \
    kernel/arch/tss.c

# C source files - Memory Management
C_SOURCES_MM = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kheap.c

# C source files - Drivers
C_SOURCES_DRIVERS = \
    kernel/drivers/timer.c \
    kernel/drivers/serial.c \
    kernel/drivers/keyboard.c

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
    kernel/proc/ready_api/ready_api.c

# Combine all C sources
C_SOURCES = \
    $(C_SOURCES_ORIG) \
    $(C_SOURCES_ARCH) \
    $(C_SOURCES_MM) \
    $(C_SOURCES_DRIVERS) \
    $(C_SOURCES_PROC)

# =============================================================================
# OBJECT FILES
# =============================================================================

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

.PHONY: all clean run run-debug debug list-sources list-objects check-files size map clean-deps

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
	$(CC) $(CFLAGS) -c -o $@ $<

# Create ISO
$(ISO): $(KERNEL)
	@echo "Creating ISO..."
	@mkdir -p isodir/boot/grub
	@cp grub.cfg isodir/boot/grub/
	@cp $(KERNEL) isodir/boot/
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
	@rm -f $(OBJECTS) $(KERNEL) $(ISO)
	@rm -rf isodir
	@echo "Clean complete."

# =============================================================================
# RUN TARGETS
# =============================================================================

run: $(ISO)
	@echo "Running MakhOS in QEMU..."
	@rm -f serial.log
	qemu-system-x86_64 -cdrom $(ISO) -vga std -m 128M -no-reboot -serial file:serial.log

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

%.d: %.c
	@set -e; rm -f $@; \
	$(CC) -MM $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

-include $(C_SOURCES:.c=.d)

clean-deps:
	@rm -f $(C_SOURCES:.c=.d)