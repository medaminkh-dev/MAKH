# MakhOS Changelog - Version 0.0.2

## Release Date
March 4, 2026

## Overview
Version 0.0.2 represents the completion of Phase 2 of MakhOS development, introducing comprehensive memory management capabilities and interrupt handling infrastructure. This release transforms MakhOS from a basic bootable kernel into a functional x86_64 operating system with proper memory management.

---

## Summary of Changes

### Phase 6: Keyboard Driver (New in this update)
**Status:** Complete  
**New Files:** 2  
**Modified Files:** 4

#### New Files (Phase 6)
1. **[`kernel/include/drivers/keyboard.h`](kernel/include/drivers/keyboard.h:1)** - PS/2 Keyboard driver header
2. **[`kernel/drivers/keyboard.c`](kernel/drivers/keyboard.c:1)** - PS/2 Keyboard driver implementation

#### Modified Files (Phase 6)
3. **[`kernel/arch/idt.c`](kernel/arch/idt.c:1)** - Added keyboard handler dispatch
4. **[`kernel/kernel.c`](kernel/kernel.c:1)** - Added `test_keyboard()` function
5. **[`kernel/include/kernel.h`](kernel/include/kernel.h:1)** - Updated to "Phase 2 Complete"
6. **[`Makefile`](Makefile:1)** - Added keyboard.c to build

---

## Phase 7: GDT with TSS (New in this update)
**Status:** Complete  
**New Files:** 6  
**Modified Files:** 2

### Added
- Task State Segment (TSS) implementation
  - RSP0 for ring 0 stack transitions
  - IST (Interrupt Stack Table) with 7 entries
  - TSS initialization and management functions
- Global Descriptor Table (GDT) with user mode support
  - 7 entries: NULL, Kernel Code/Data, User Code/Data, TSS (128-bit)
  - Proper x86_64 128-bit TSS descriptor format
  - Segment register reloading with far return
- String library for kernel
  - memset, memcpy, strlen, strcmp functions
- GDT/TSS verification tests
  - Check CS, DS segment registers
  - Verify TSS loaded in Task Register

### Technical Details
- TSS size: 104 bytes (0x68)
- TSS limit: 0x67
- GDT entries: 7 (6 regular + 2 for 128-bit TSS)
- Critical fix: x86_64 requires 128-bit TSS descriptor using 2 GDT entries

#### New Files (Phase 7)
1. **[`kernel/include/arch/tss.h`](kernel/include/arch/tss.h:1)** - TSS structure definition
2. **[`kernel/include/arch/gdt.h`](kernel/include/arch/gdt.h:1)** - GDT header with selectors
3. **[`kernel/include/lib/string.h`](kernel/include/lib/string.h:1)** - String library declarations
4. **[`kernel/lib/string.c`](kernel/lib/string.c:1)** - String library implementation
5. **[`kernel/arch/tss.c`](kernel/arch/tss.c:1)** - TSS initialization and management
6. **[`kernel/arch/gdt.c`](kernel/arch/gdt.c:1)** - GDT implementation with 128-bit TSS

#### Modified Files (Phase 7)
7. **[`kernel/kernel.c`](kernel/kernel.c:1)** - GDT/TSS integration and tests
8. **[`Makefile`](Makefile:1)** - Added gdt.c, tss.c, string.c to build

### Testing
- Verified with GDB: No GPF when loading TSS
- All segment registers correctly set
- TR register contains TSS selector 0x28

---

### Previous Phases (Already Documented)

#### Phase 5: PIC and Timer
- PIC driver (pic.c, pic.h)
- Timer driver (timer.c, timer.h)
- IDT IRQ support

#### Phase 4: Interrupt Descriptor Table
- IDT implementation (idt.c, idt_asm.asm, idt.h)
- Exception handling

#### Phase 2 Parts 1-3: Memory Management
- Physical Memory Manager (pmm.c, pmm.h)
- Virtual Memory Manager (vmm.c, vmm.h, paging_asm.asm)
- Kernel Heap Manager (kheap.c, kheap.h)
- Standard library (stdlib.h)

### Total Files in v0.0.2
- **New:** 19 files
- **Modified:** 8 files
- **Total Lines of Code:** ~9,000+

---

## Phase 6: Keyboard Driver Details

### Overview
The PS/2 Keyboard driver provides interrupt-driven keyboard input with the following features:

- **Circular Buffer:** 256-character ring buffer for keystrokes
- **Dual Keymaps:** Normal and shifted character mappings
- **Modifier Tracking:** Shift, Ctrl, Alt, and Caps Lock states
- **LED Control:** Caps Lock LED feedback
- **Scancode Set 1:** Full support for standard PS/2 keyboards

### API Functions

**`void keyboard_init(void)`**
- Resets keyboard controller and enables scanning
- Unmasks IRQ1 for interrupt-driven input
- Output: Initialization status messages

**`char keyboard_get_char(void)`**
- Blocking read from keyboard buffer
- Returns ASCII characters including control codes (\n, \b, \t)
- Uses HLT instruction to save CPU while waiting

**`int keyboard_has_input(void)`**
- Non-blocking check for available input
- Returns 1 if character available, 0 otherwise

**`void keyboard_clear_buffer(void)`**
- Clears the circular input buffer

**`uint8_t keyboard_get_modifiers(void)`**
- Returns bitmask of active modifiers
- Bits: 0=Shift, 1=Ctrl, 2=Alt, 3=Caps Lock

### Key Features

**Modifier Handling:**
- Left/Right Shift: Toggles uppercase keymap
- Caps Lock: Toggles case for letters with LED indicator
- Ctrl/Alt: Tracked for future use

**Special Keys:**
- Enter: Returns '\n' character
- Backspace: Returns '\b' character
- Tab: Returns '\t' character
- Space: Returns ' ' character

**Interactive Test:**
The `test_keyboard()` function in kernel.c provides:
- Real-time character echo to screen
- Backspace support for corrections
- Pattern recognition (type "TEST")
- Success/failure reporting

### Example Usage
```c
// Initialize keyboard
keyboard_init();

// Blocking read
char c = keyboard_get_char();
terminal_putchar(c);

// Check modifiers
if (keyboard_get_modifiers() & 0x08) {
    terminal_writestring("Caps Lock is ON\n");
}
```

---

## Detailed Changes

### 1. Build System ([`Makefile`](Makefile:1))

**Changes:**
- Added new assembly source files to build:
  - `kernel/mm/paging_asm.asm` - Paging operations
  - `kernel/arch/idt_asm.asm` - Interrupt stubs
- Added new C source files to build:
  - `kernel/mm/pmm.c` - Physical Memory Manager
  - `kernel/mm/vmm.c` - Virtual Memory Manager
  - `kernel/mm/kheap.c` - Kernel Heap Manager
  - `kernel/arch/idt.c` - Interrupt Descriptor Table

**Impact:** Enables compilation of all new kernel subsystems.

---

### 2. Boot Loader ([`boot/boot.asm`](boot/boot.asm:1))

**Changes:**
- Added global variable `multiboot_info_ptr` to store multiboot information pointer
- Store RDI (multiboot info pointer) in global variable for C code access
- New data section with 8-byte aligned storage for multiboot pointer

**Impact:** Allows kernel to access GRUB's multiboot2 information structure, including memory maps.

---

### 3. Kernel Header ([`kernel/include/kernel.h`](kernel/include/kernel.h:1))

**Changes:**
- Updated `KERNEL_PHASE` from "Phase 1" to "Phase 2 Part 4"
- Added utility function declarations:
  - `uint64_to_string()` - Convert uint64 to decimal string
  - `uint64_to_hex()` - Convert uint64 to hexadecimal string

**Impact:** Provides standardized number formatting utilities for kernel output.

---

### 4. Main Kernel ([`kernel/kernel.c`](kernel/kernel.c:1))

**Major Changes:**

#### New Includes:
- `mm/pmm.h` - Physical Memory Manager
- `mm/vmm.h` - Virtual Memory Manager
- `mm/kheap.h` - Kernel Heap Manager
- `stdlib.h` - Standard library
- `arch/idt.h` - Interrupt Descriptor Table

#### New Utility Functions:
- `uint64_to_string()` - Decimal string conversion for uint64
- `uint64_to_hex()` - Hexadecimal string conversion for uint64

#### New Test Functions:
- `test_vmm()` - Comprehensive Virtual Memory Manager testing
- `test_kheap()` - Kernel heap allocator testing
- `test_interrupts()` - IDT and interrupt testing

#### Initialization Sequence:
1. VGA driver initialization
2. Multiboot info parsing
3. **PMM initialization** (NEW)
4. **VMM initialization** (NEW)
5. **Kernel heap initialization** (NEW)
6. **IDT initialization** (NEW)

**Impact:** Transforms kernel from basic boot to fully functional OS with memory management.

---

## New Subsystems

### Physical Memory Manager (PMM)

**Files:** [`kernel/mm/pmm.c`](kernel/mm/pmm.c:1), [`kernel/include/mm/pmm.h`](kernel/include/mm/pmm.h:1)

**Features:**
- Bitmap-based page allocation (4KB pages)
- First-fit allocation algorithm
- Support for up to 4GB of physical memory
- Reserved memory protection (first 4MB for kernel)
- Memory statistics tracking

**API:**
- `pmm_init()` - Initialize using multiboot memory map
- `pmm_alloc_page()` - Allocate single physical page
- `pmm_free_page()` - Free physical page
- `pmm_get_free_memory()` - Get free memory count
- `pmm_get_total_page_count()` - Get total page count
- `pmm_get_used_page_count()` - Get used page count

---

### Virtual Memory Manager (VMM)

**Files:** [`kernel/mm/vmm.c`](kernel/mm/vmm.c:1), [`kernel/mm/paging_asm.asm`](kernel/mm/paging_asm.asm:1), [`kernel/include/mm/vmm.h`](kernel/include/mm/vmm.h:1)

**Features:**
- 4-level x86_64 paging (PML4 -> PDPT -> PD -> PT)
- Higher-half kernel mapping
- Dynamic page table creation
- Page mapping/unmapping
- TLB management
- Support for NX bit (No Execute)

**API:**
- `vmm_init()` - Initialize kernel page tables
- `vmm_alloc_page()` - Allocate virtual page with physical backing
- `vmm_free_page()` - Free virtual page
- `vmm_map_page()` - Map virtual to physical address
- `vmm_unmap_page()` - Unmap virtual address
- `vmm_get_physical()` - Get physical address for virtual

---

### Kernel Heap Manager

**Files:** [`kernel/mm/kheap.c`](kernel/mm/kheap.c:1), [`kernel/include/mm/kheap.h`](kernel/include/mm/kheap.h:1)

**Features:**
- Block-based allocator with header/footer
- Free list management
- Block splitting and coalescing
- 16-byte alignment for all allocations
- Magic numbers for corruption detection
- Statistics tracking

**API:**
- `kheap_init()` - Initialize heap
- `kmalloc()` - Allocate memory (malloc equivalent)
- `kcalloc()` - Allocate zeroed memory
- `krealloc()` - Reallocate memory
- `kfree()` - Free memory
- `kheap_get_total/used/free()` - Statistics

---

### Standard Library

**File:** [`kernel/include/stdlib.h`](kernel/include/stdlib.h:1)

**Features:**
- Standard malloc/calloc/realloc/free macros mapping to kheap
- POSIX exit codes
- Utility function declarations

---

### Interrupt Descriptor Table (IDT)

**Files:** [`kernel/arch/idt.c`](kernel/arch/idt.c:1), [`kernel/arch/idt_asm.asm`](kernel/arch/idt_asm.asm:1), [`kernel/include/arch/idt.h`](kernel/include/arch/idt.h:1)

**Features:**
- 256 IDT entries
- Exception handlers for CPU exceptions (0-31)
- Assembly stubs for all interrupts
- Register state preservation
- Exception message lookup table

**Supported Exceptions:**
- 0: Division By Zero
- 1: Debug
- 2: Non-Maskable Interrupt
- 3: Breakpoint
- 4: Overflow
- 5: Bound Range Exceeded
- 6: Invalid Opcode
- 7: Device Not Available
- 8: Double Fault
- 9-21: Various CPU exceptions
- 14: Page Fault

**API:**
- `idt_init()` - Initialize IDT
- `idt_set_gate()` - Set interrupt gate
- `idt_enable_interrupts()` - Enable interrupts (STI)
- `idt_disable_interrupts()` - Disable interrupts (CLI)

---

## Testing

All subsystems include comprehensive test suites:

### PMM Tests:
- Allocate multiple pages
- Free and reallocate pages
- Memory statistics verification

### VMM Tests:
- Virtual page allocation
- Fixed virtual-to-physical mapping
- Physical address translation
- Read/write through virtual addresses

### Heap Tests:
- Basic allocation
- Multiple allocations
- Variable size allocations
- 16-byte alignment verification
- Zeroed memory (kcalloc)
- Reallocation with data preservation
- Statistics reporting
- Standard library macro testing

### IDT Tests:
- Software interrupt triggering
- Handler execution

---

## Technical Specifications

### Memory Layout:
- **Physical Memory:** Up to 4GB supported
- **Page Size:** 4KB
- **Virtual Address Space:** 48-bit (x86_64 standard)
- **Higher Half Base:** 0xFFFF800000000000
- **Heap Start:** 0xFFFF900000000000
- **Reserved Low Memory:** 0-4MB (kernel, VGA, BIOS)

### Page Table Hierarchy:
```
PML4 (Page Map Level 4)
  └─ PDPT (Page Directory Pointer Table)
       └─ PD (Page Directory)
            └─ PT (Page Table)
                 └─ 4KB Page
```

### Build Requirements:
- NASM assembler
- GCC cross-compiler (x86_64-elf)
- GNU Make
- GRUB2 (for ISO creation)
- QEMU (for testing)

---

## Migration from v0.0.1

No breaking changes. v0.0.2 is fully backward compatible with v0.0.1 boot process while adding new capabilities.

---

## Known Limitations

1. **Physical Memory:** Limited to 4GB due to static bitmap sizing
2. **Heap Size:** Initial heap limited to 16KB, grows dynamically
3. **Interrupts:** Software interrupts tested, hardware interrupts pending
4. **Processes:** No process management yet (Phase 3)

---

## Next Steps (Phase 3 Preview)

- Process management and scheduling
- System calls infrastructure
- Timer interrupt handling
- Keyboard input driver
- Basic shell/interpreter

---

## Contributors
- MakhOS Development Team

## License
MIT License - See LICENSE file for details
