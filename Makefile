# =============================================================================
# MakhOS Makefile
# =============================================================================
# MakhOS - A simple 64-bit operating system kernel
# 
# Phase 9 Change: Added process management support
#   - Added kernel/arch/context_switch.asm to ASM_SOURCES
#   - Added kernel/proc/proc.c to C_SOURCES
# =============================================================================

# Toolchain
CC = gcc
AS = nasm
LD = ld

# Flags
CFLAGS = -ffreestanding -fno-pie -O2 -Wall -Wextra
CFLAGS += -std=gnu99 -fno-stack-protector -nostdinc
CFLAGS += -I kernel/include
ASFLAGS = -f elf64
LDFLAGS = -T linker.ld -nostdlib

# Source files
# PHASE 10 CHANGE: Added usermode.asm for user mode transition
# PHASE 10.2c CHANGE: Added user_program.asm for user program
ASM_SOURCES = boot/boot.asm kernel/mm/paging_asm.asm kernel/arch/idt_asm.asm kernel/arch/syscall_asm.asm kernel/arch/context_switch.asm kernel/arch/usermode.asm kernel/user/user_program.asm
# PHASE 9 CHANGE: Added proc/proc.c for process management
# SERIAL CHANGE: Added drivers/serial.c for serial port output
C_SOURCES = kernel/kernel.c kernel/vga.c kernel/multiboot.c kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/kheap.c kernel/arch/idt.c kernel/arch/pic.c kernel/arch/gdt.c kernel/arch/tss.c kernel/drivers/timer.c kernel/drivers/keyboard.c kernel/drivers/serial.c kernel/input_line.c kernel/lib/string.c kernel/syscall/syscall.c kernel/proc/proc.c

# Object files
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)
C_OBJECTS = $(C_SOURCES:.c=.o)
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# Output
KERNEL = makhos.kernel
ISO = makhos.iso

# Default target
.PHONY: all
all: $(KERNEL) $(ISO)

# Link kernel
$(KERNEL): $(OBJECTS)
	@echo "LD" $@
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

# Assemble assembly files in root directory
%.o: %.asm
	@echo "AS" $<
	$(AS) $(ASFLAGS) -o $@ $<

# Assemble assembly files in kernel subdirectories
kernel/%.o: kernel/%.asm
	@echo "AS" $<
	$(AS) $(ASFLAGS) -o $@ $<

# Compile C files
%.o: %.c
	@echo "CC" $<
	$(CC) $(CFLAGS) -c -o $@ $<

# Create ISO
$(ISO): $(KERNEL)
	@echo "Creating ISO..."
	@mkdir -p isodir
	@mkdir -p isodir/boot
	@mkdir -p isodir/boot/grub
	@cp grub.cfg isodir/boot/grub/
	@cp $(KERNEL) isodir/boot/
	@grub-mkrescue -o $(ISO) isodir 2>/dev/null || (echo "Note: grub-mkrescue not found, using alternative method" && cp $(KERNEL) $(ISO))
	@rm -rf isodir
	@echo "ISO created: $(ISO)"

# Clean
.PHONY: clean
clean:
	@echo "Cleaning..."
	@rm -f $(OBJECTS)
	@rm -f $(KERNEL)
	@rm -f $(ISO)
	@rm -rf isodir
	@echo "Clean complete."

# Run in QEMU
.PHONY: run
run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -vga std -m 128M -no-reboot

# Debug in QEMU with GDB
.PHONY: debug
debug: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -vga std -m 128M -no-reboot -s -S &
	@echo "QEMU running with GDB stub on localhost:1234"
	@echo "Run: gdb -ex 'file $(KERNEL)' -ex 'target remote localhost:1234'"
