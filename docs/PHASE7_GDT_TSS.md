# Phase 7: GDT with TSS (Global Descriptor Table & Task State Segment)

## Overview
This phase implements the GDT with TSS to prepare for user mode and system calls.

## Architecture

### 1. TSS (Task State Segment)
- Purpose: Store stack pointers for privilege level transitions
- Size: 104 bytes minimum (0x68)
- Key fields:
  - RSP0: Ring 0 stack pointer
  - IST1-IST7: Interrupt Stack Table
  - IOMAP: I/O permission map base

### 2. GDT Structure
| Index | Selector | Type | Ring | Purpose |
|-------|----------|------|------|---------|
| 0 | 0x00 | NULL | - | Null descriptor |
| 1 | 0x08 | Code | 0 | Kernel code |
| 2 | 0x10 | Data | 0 | Kernel data |
| 3 | 0x18 | Code | 3 | User code |
| 4 | 0x20 | Data | 3 | User data |
| 5-6 | 0x28 | TSS | 0 | 128-bit TSS descriptor |

### 3. Critical Fix: 128-bit TSS Descriptor
In x86_64 long mode, TSS requires TWO consecutive GDT entries:
- Entry 5: Lower 64 bits (base[23:0], limit, access, granularity)
- Entry 6: Upper 64 bits (base[63:32])

## Implementation Steps

### Step 1: TSS Header (tss.h)
Define the TSS structure with proper packed attribute.

### Step 2: GDT Header (gdt.h)
Define segment selectors and GDT entry structures.

### Step 3: String Library
Add basic string functions needed by TSS/GDT code.

### Step 4: TSS Implementation (tss.c)
- Initialize TSS with memset
- Set RSP0 for kernel stack
- Configure IST entries

### Step 5: GDT Implementation (gdt.c)
- Set up 7 GDT entries
- Handle 128-bit TSS descriptor
- Reload segment registers with far return
- Load TSS into TR register

### Step 6: Integration (kernel.c)
- Call tss_init() before gdt_init()
- Allocate kernel stack and IST stacks
- Test segment registers

### Step 7: Build System
- Add new source files to Makefile

## Testing
```bash
# Build and test
make clean && make
qemu-system-x86_64 -cdrom makhos.iso -serial stdio

# Verify with GDB
gdb -batch \
  -ex "file makhos.kernel" \
  -ex "target remote localhost:1234" \
  -ex "break gdt_load_tss" \
  -ex "continue" \
  -ex "x/16wx &gdt"
```

## Expected Output
```
[TSS] Initializing Task State Segment...
[TSS] Initialized at 0x..., size 104 bytes
[TSS] Kernel stack set to 0x...
[TSS] IST1 set to 0x...
[GDT] Initializing Global Descriptor Table...
[GDT] Loaded successfully
[TEST] Testing GDT and TSS...
  [OK] CS is kernel code segment
  [OK] DS is kernel data segment
  [OK] TSS loaded in TR
```

## Files Changed
- kernel/include/arch/tss.h (new)
- kernel/include/arch/gdt.h (new)
- kernel/include/lib/string.h (new)
- kernel/lib/string.c (new)
- kernel/arch/tss.c (new)
- kernel/arch/gdt.c (new)
- kernel/kernel.c (modified)
- Makefile (modified)
