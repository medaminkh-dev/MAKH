# MakhOS Changelog - Version 0.0.2

## Release Date
March 4, 2026

## Overview
Version 0.0.2 represents the completion of Phase 2 of MakhOS development, introducing comprehensive memory management capabilities and interrupt handling infrastructure. This release transforms MakhOS from a basic bootable kernel into a functional x86_64 operating system with proper memory management.

---

## Summary of Changes

### Modified Files (4 files)
1. **[`Makefile`](Makefile:1)** - Updated build configuration
2. **[`boot/boot.asm`](boot/boot.asm:1)** - Added multiboot info pointer storage
3. **[`kernel/include/kernel.h`](kernel/include/kernel.h:1)** - Added utility function declarations
4. **[`kernel/kernel.c`](kernel/kernel.c:1)** - Integrated all new subsystems

### New Files (11 files)

#### Memory Management Subsystem (8 files)
1. **[`kernel/mm/pmm.c`](kernel/mm/pmm.c:1)** - Physical Memory Manager implementation
2. **[`kernel/mm/vmm.c`](kernel/mm/vmm.c:1)** - Virtual Memory Manager implementation
3. **[`kernel/mm/kheap.c`](kernel/mm/kheap.c:1)** - Kernel Heap Manager implementation
4. **[`kernel/mm/paging_asm.asm`](kernel/mm/paging_asm.asm:1)** - Paging assembly helpers
5. **[`kernel/include/mm/pmm.h`](kernel/include/mm/pmm.h:1)** - PMM header
6. **[`kernel/include/mm/vmm.h`](kernel/include/mm/vmm.h:1)** - VMM header
7. **[`kernel/include/mm/kheap.h`](kernel/include/mm/kheap.h:1)** - Heap header
8. **[`kernel/include/stdlib.h`](kernel/include/stdlib.h:1)** - Standard library header

#### Interrupt Management Subsystem (3 files)
9. **[`kernel/arch/idt.c`](kernel/arch/idt.c:1)** - Interrupt Descriptor Table implementation
10. **[`kernel/arch/idt_asm.asm`](kernel/arch/idt_asm.asm:1)** - IDT assembly stubs
11. **[`kernel/include/arch/idt.h`](kernel/include/arch/idt.h:1)** - IDT header

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
