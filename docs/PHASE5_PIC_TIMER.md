# MakhOS Phase 5 - PIC and Timer Implementation

## Overview

Phase 5 completes the interrupt infrastructure by implementing the 8259A Programmable Interrupt Controller (PIC) and the PIT (Programmable Interval Timer) driver. This enables hardware interrupts, allowing the kernel to respond to external events such as timer ticks.

## Release Date
March 4, 2026

## Version
**v0.0.2 - Phase 2 Part 5**

---

## Summary of Changes

### New Files (5 files)

#### PIC Driver (2 files)
1. **[`kernel/include/arch/pic.h`](kernel/include/arch/pic.h:1)** - PIC header with constants and function declarations
2. **[`kernel/arch/pic.c`](kernel/arch/pic.c:1)** - 8259A PIC implementation

#### Timer Driver (2 files)
3. **[`kernel/include/drivers/timer.h`](kernel/include/drivers/timer.h:1)** - PIT timer header
4. **[`kernel/drivers/timer.c`](kernel/drivers/timer.c:1)** - PIT timer implementation

#### Modified Files (5 files)
5. **[`kernel/arch/idt_asm.asm`](kernel/arch/idt_asm.asm:1)** - Added IRQ stubs (vectors 32-47)
6. **[`kernel/arch/idt.c`](kernel/arch/idt.c:1)** - Added `irq_handler()` for hardware interrupts
7. **[`kernel/include/arch/idt.h`](kernel/include/arch/idt.h:1)** - Added `irq_handler()` declaration
8. **[`kernel/kernel.c`](kernel/kernel.c:1)** - Added `test_timer()` function
9. **[`kernel/include/kernel.h`](kernel/include/kernel.h:1)** - Updated version to "Phase 2 Part 5"
10. **[`Makefile`](Makefile:1)** - Added `kernel/arch/pic.c` and `kernel/drivers/timer.c` to build

---

## Detailed Changes

### 1. PIC Driver ([`kernel/include/arch/pic.h`](kernel/include/arch/pic.h:1) & [`kernel/arch/pic.c`](kernel/arch/pic.c:1))

#### Hardware Background
The 8259A PIC manages hardware interrupts on x86 systems. It consists of two cascaded controllers:
- **Master PIC**: IRQs 0-7 (vectors 0x20-0x27, remapped from 0x08-0x0F)
- **Slave PIC**: IRQs 8-15 (vectors 0x28-0x2F, remapped from 0x70-0x77)

#### Remapping Rationale
The PIC must be remapped because the default vectors (0x08-0x0F and 0x70-0x77) conflict with CPU exception vectors (0-31). We remap to:
- Master: 0x20-0x27 (32-39)
- Slave: 0x28-0x2F (40-47)

#### API Functions

**`void pic_init(void)`**
- Initializes both master and slave PICs
- Sends ICW1-ICW4 initialization commands
- Masks all IRQs initially (must be unmasked individually)
- Output: `[PIC] Initializing 8259A...` → `[PIC] Remapped to vectors 0x20-0x2F`

**`void pic_send_eoi(uint8_t irq)`**
- Sends End of Interrupt (EOI) signal to PIC
- For IRQs 8-15, sends EOI to both slave and master
- For IRQs 0-7, sends EOI to master only
- Must be called after handling any hardware interrupt

**`void pic_mask_irq(uint8_t irq)`**
- Disables (masks) a specific IRQ (0-15)
- Sets the corresponding bit in the PIC mask register

**`void pic_unmask_irq(uint8_t irq)`**
- Enables (unmasks) a specific IRQ (0-15)
- Clears the corresponding bit in the PIC mask register

**`void pic_disable(void)`**
- Disables the PIC entirely (masks all interrupts)
- Useful when switching to APIC/IO-APIC

#### IRQ Assignments
```c
#define IRQ_TIMER     0   // System timer (PIT)
#define IRQ_KEYBOARD  1   // Keyboard controller
#define IRQ_CASCADE   2   // Cascade for slave PIC
#define IRQ_COM2      3   // Serial port COM2
#define IRQ_COM1      4   // Serial port COM1
#define IRQ_LPT2      5   // Parallel port 2
#define IRQ_FLOPPY    6   // Floppy disk controller
#define IRQ_LPT1      7   // Parallel port 1
#define IRQ_CMOS      8   // Real-time clock
#define IRQ_PS2MOUSE 12   // PS/2 mouse
#define IRQ_PRIMARY  14   // Primary IDE
#define IRQ_SECONDARY 15  // Secondary IDE
```

---

### 2. Timer Driver ([`kernel/include/drivers/timer.h`](kernel/include/drivers/timer.h:1) & [`kernel/drivers/timer.c`](kernel/drivers/timer.c:1))

#### Hardware Background
The PIT (Programmable Interval Timer) uses a crystal oscillator running at 1.193182 MHz. By programming a divisor, we can generate periodic interrupts at a desired frequency.

#### Configuration
```c
#define TIMER_FREQUENCY    100  // 100 Hz = 10ms ticks
#define PIT_BASE_FREQUENCY 1193182  // Hz
```

#### API Functions

**`void timer_init(uint32_t frequency)`**
- Initializes the PIT with the specified frequency
- Calculates divisor: `divisor = 1193182 / frequency`
- Programs Channel 0 in Mode 3 (square wave generator)
- Unmasks IRQ0 (timer interrupt)
- Output: `[TIMER] Initialized at 100 Hz (divisor: 11931)`

**`void timer_handler(registers_t* regs)`**
- Interrupt handler called on each timer tick (IRQ0)
- Increments global tick counter
- Prints statistics every second

**`uint64_t timer_get_ticks(void)`**
- Returns the total number of timer ticks since initialization
- Used for timing and delays

**`void timer_sleep(uint32_t ms)`**
- Busy-wait delay for specified milliseconds
- Uses `timer_ticks` for accurate timing
- Note: Blocks CPU - suitable for kernel initialization only

**`void timer_print_stats(void)`**
- Displays current tick count and uptime
- Format: `Uptime: 10.234 seconds`

---

### 3. IDT Modifications

#### [`kernel/arch/idt_asm.asm`](kernel/arch/idt_asm.asm:1)

**Added IRQ Stubs (lines 111-116):**
```nasm
; IRQ stubs (vectors 32-47)
%assign i 32
%rep 16
    ISR_NOERR i
%assign i i+1
%endrep
```

This creates assembly stubs `isr32` through `isr47` that:
1. Push dummy error code (0)
2. Push interrupt number (32-47)
3. Jump to `isr_common` to save registers and call C handler

**Updated stub table (lines 133-135):**
```nasm
; IRQs 32-47 (remapped from PIC)
dq isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
dq isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
```

#### [`kernel/arch/idt.c`](kernel/arch/idt.c:1)

**Added `irq_handler()` (lines 178-223):**
```c
void irq_handler(registers_t* regs) {
    uint8_t vector = regs->int_no;
    uint8_t irq = vector - 32;  // Convert to IRQ number
    
    // Handle spurious IRQs
    if (irq == 7 || irq == 15) {
        // Check ISR register, return if spurious
    }
    
    // Dispatch based on IRQ
    switch (irq) {
        case IRQ_TIMER:
            timer_handler(regs);
            break;
        case IRQ_KEYBOARD:
            // Future: keyboard handling
            break;
    }
    
    pic_send_eoi(irq);
}
```

**Modified `exception_handler()` (lines 110-118):**
```c
void exception_handler(registers_t* regs) {
    uint8_t vector = regs->int_no;
    
    // Handle IRQs (vectors 32-47) separately
    if (vector >= 32 && vector < 48) {
        irq_handler(regs);
        return;
    }
    // ... rest of exception handling
}
```

#### [`kernel/include/arch/idt.h`](kernel/include/arch/idt.h:1)

**Added declaration (line 67):**
```c
void irq_handler(registers_t* regs);
```

---

### 4. Kernel Integration

#### [`kernel/include/kernel.h`](kernel/include/kernel.h:1)

**Updated phase identifier (line 14):**
```c
#define KERNEL_PHASE    "Phase 2 Part 5"
```

#### [`kernel/kernel.c`](kernel/kernel.c:1)

**Added includes:**
```c
#include "include/arch/pic.h"
#include "include/drivers/timer.h"
```

**Added `test_timer()` function (lines 336-380):**
```c
void test_timer(void) {
    // 1. Initialize PIC
    pic_init();
    
    // 2. Initialize Timer at 100 Hz
    timer_init(TIMER_FREQUENCY);
    
    // 3. Enable interrupts
    idt_enable_interrupts();
    
    // 4. Test 2-second sleep
    timer_sleep(2000);
    
    // 5. Verify ~200 ticks occurred
    // 6. Print statistics
}
```

**Updated `kernel_main()` (line 529):**
```c
// Initialize IDT
idt_init();

// Test timer and interrupts
test_timer();
```

#### [`Makefile`](Makefile:1)

**Updated source lists:**
```makefile
C_SOURCES = kernel/kernel.c kernel/vga.c ... kernel/arch/idt.c \
            kernel/arch/pic.c kernel/drivers/timer.c
```

---

## Testing

### Timer Test Output
```
[CHECK] Initializing Programmable Interrupt Controller...
[PIC] Initializing 8259A...
[PIC] Remapped to vectors 0x20-0x2F
[CHECK] Initializing Timer...
[TIMER] Initializing PIT with 100 Hz...
[TIMER] Initialized at 100 Hz (divisor: 11931)
[CHECK] Enabling interrupts...
[IDT] Interrupts enabled

  Waiting for 2 seconds...
[TIMER] Tick: 1 seconds
[TIMER] Tick: 2 seconds
[OK] Timer counted 200 ticks in 2 seconds (expected ~200)

[TIMER] Statistics:
  Total ticks: 200
  Uptime: 2.000 seconds
```

### Expected Behavior
1. Timer interrupts fire at 100Hz (every 10ms)
2. Tick counter increments accurately
3. 2-second sleep results in ~200 ticks
4. System continues running, waiting for interrupts

---

## Architecture Flow

```
Hardware Timer (PIT)
       |
       v
   IRQ0 Triggered
       |
       v
  PIC (Master)
       |
       v
  Vector 0x20 (32)
       |
       v
  CPU -> isr32 (ASM stub)
       |
       v
  Save registers
       |
       v
  Call exception_handler()
       |
       v
  Detect vector >= 32
       |
       v
  Call irq_handler()
       |
       v
  Dispatch to timer_handler()
       |
       v
  Increment tick counter
       |
       v
  Send EOI to PIC
       |
       v
  Restore registers
       |
       v
  IRET (return from interrupt)
```

---

## Files Changed

### New Files
- [`kernel/include/arch/pic.h`](kernel/include/arch/pic.h:1) - 47 lines
- [`kernel/arch/pic.c`](kernel/arch/pic.c:1) - 117 lines
- [`kernel/include/drivers/timer.h`](kernel/include/drivers/timer.h:1) - 34 lines
- [`kernel/drivers/timer.c`](kernel/drivers/timer.c:1) - 163 lines

### Modified Files
- [`kernel/arch/idt_asm.asm`](kernel/arch/idt_asm.asm:111) - Added 16 IRQ stubs
- [`kernel/arch/idt.c`](kernel/arch/idt.c:178) - Added irq_handler() (46 lines)
- [`kernel/include/arch/idt.h`](kernel/include/arch/idt.h:67) - Added irq_handler declaration
- [`kernel/kernel.c`](kernel/kernel.c:336) - Added test_timer() function
- [`kernel/include/kernel.h`](kernel/include/kernel.h:14) - Updated phase string
- [`Makefile`](Makefile:30) - Added pic.c and timer.c to build

---

## Technical Notes

### Spurious IRQs
The handler checks for spurious IRQ7 and IRQ15 by reading the In-Service Register (ISR) from the PIC. If the corresponding bit is not set, the interrupt is spurious and should not be acknowledged with EOI (except for IRQ15, which still needs master EOI for cascade).

### Interrupt Latency
With 100Hz timer (10ms period), interrupt latency is minimal as the handler is lightweight (increments counter, prints periodic messages).

### Future Enhancements
- Implement scheduler using timer ticks
- Add keyboard driver (IRQ1)
- Replace PIC with APIC for SMP support
- Add interrupt nesting/priorities
