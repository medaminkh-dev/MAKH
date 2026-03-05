# MakhOS - Makefile
# Version: 0.0.2
# Build system for x86_64 kernel

# Target architecture
ARCH = x86_64

# Compiler settings
# Use x86_64-elf-gcc if available, otherwise try system gcc with flags
CC = $(shell which x86_64-elf-gcc 2>/dev/null || echo gcc)
LD = $(shell which x86_64-elf-ld 2>/dev/null || echo ld)

# Compiler flags
CFLAGS = -m64 -march=x86-64 -ffreestanding -O2 -Wall -Wextra -Werror -nostdlib -nostartfiles
CFLAGS += -fno-stack-protector -mno-red-zone -mcmodel=large -fno-pic
CFLAGS += -fomit-frame-pointer -fno-asynchronous-unwind-tables
CFLAGS += -Ikernel/include
# Debug flags (uncomment for debugging)
CFLAGS += -g -DDEBUG

# Assembler
AS = nasm
ASFLAGS = -f elf64

# Linker flags
LDFLAGS = -T linker.ld -nostdlib

# Source files
ASM_SOURCES = boot/boot.asm kernel/mm/paging_asm.asm kernel/arch/idt_asm.asm
C_SOURCES = kernel/kernel.c kernel/vga.c kernel/multiboot.c kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/kheap.c kernel/arch/idt.c kernel/arch/pic.c kernel/arch/gdt.c kernel/arch/tss.c kernel/drivers/timer.c kernel/drivers/keyboard.c kernel/input_line.c kernel/lib/string.c

# Object files
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)
C_OBJECTS = $(C_SOURCES:.c=.o)
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# Output
KERNEL = makhos.kernel
ISO = makhos.iso

# QEMU settings
QEMU = qemu-system-x86_64
QEMU_FLAGS = -cdrom $(ISO) -serial stdio -m 128M

# Default target
.PHONY: all clean iso run debug

all: $(KERNEL)

# Create kernel ELF
$(KERNEL): $(OBJECTS) linker.ld
	@echo "LD $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

# Compile assembly files
%.o: %.asm
	@echo "AS $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# Compile C files
%.o: %.c
	@echo "CC $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Create bootable ISO
iso: $(ISO)

$(ISO): $(KERNEL) grub.cfg
	@echo "Creating ISO..."
	@mkdir -p iso/boot/grub
	@cp $(KERNEL) iso/boot/
	@cp grub.cfg iso/boot/grub/
	@grub-mkrescue -o $(ISO) iso/ 2>/dev/null || \
	 (echo "grub-mkrescue not found, trying xorriso..." && \
	  grub-mkrescue --xorriso=/usr/bin/xorriso -o $(ISO) iso/)
	@rm -rf iso/
	@echo "ISO created: $(ISO)"

# Run in QEMU
run: $(ISO)
	@echo "Running MakhOS in QEMU..."
	@$(QEMU) $(QEMU_FLAGS)

# Debug mode (waits for GDB)
debug: $(ISO)
	@echo "Starting QEMU in debug mode..."
	@echo "In another terminal, run: gdb -ex 'target remote localhost:1234'"
	@$(QEMU) $(QEMU_FLAGS) -s -S

# Clean build artifacts
clean:
	@echo "Cleaning..."
	@rm -f $(OBJECTS) $(KERNEL) $(ISO)
	@rm -rf iso/
	@echo "Clean complete."

# Print info
info:
	@echo "MakhOS Build System"
	@echo "==================="
	@echo "Architecture: $(ARCH)"
	@echo "Compiler: $(CC)"
	@echo "Linker: $(LD)"
	@echo "Sources: $(C_SOURCES) $(ASM_SOURCES)"

# Check dependencies
check:
	@echo "Checking dependencies..."
	@which $(CC) > /dev/null && echo "[OK] Compiler ($(CC))" || echo "[MISSING] Compiler"
	@which $(AS) > /dev/null && echo "[OK] Assembler ($(AS))" || echo "[MISSING] Assembler"
	@which $(LD) > /dev/null && echo "[OK] Linker ($(LD))" || echo "[MISSING] Linker"
	@which grub-mkrescue > /dev/null && echo "[OK] grub-mkrescue" || echo "[MISSING] grub-mkrescue"
	@which $(QEMU) > /dev/null && echo "[OK] QEMU ($(QEMU))" || echo "[MISSING] QEMU"
