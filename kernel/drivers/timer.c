/**
 * =============================================================================
 * timer.c - PIT (Programmable Interval Timer) Driver for MakhOS
 * =============================================================================
 * Implements the timer interrupt driver that drives the system clock
 * and provides the basis for preemptive multitasking.
 *
 * Phase 9 Changes:
 *   - Added proc_yield() call in timer_handler for preemptive scheduling
 *   - Added in_interrupt_context flag to prevent nested context switches
 * =============================================================================
 */

#include <drivers/timer.h>
#include <arch/pic.h>
#include <kernel.h>
#include <vga.h>

/**
 * PHASE 9 CHANGE: Added extern declaration for scheduler
 * The timer interrupt will call the scheduler to implement preemptive multitasking
 */
extern void proc_yield(void);

// Tick counter - incremented every timer interrupt
static volatile uint64_t timer_ticks = 0;
static volatile uint32_t timer_frequency = 0;

/**
 * timer_init - Initialize the PIT timer
 * @frequency: Desired frequency in Hz (e.g., 100 for 100Hz = 10ms ticks)
 * 
 * Calculates the divisor from base frequency (1193182 Hz) and programs
 * the PIT to generate interrupts at the specified frequency.
 */
void timer_init(uint32_t frequency) {
    char buf[32];
    
    terminal_writestring("[TIMER] Initializing PIT with ");
    uint64_to_string(frequency, buf);
    terminal_writestring(buf);
    terminal_writestring(" Hz...\n");
    
    // Save frequency for calculations
    timer_frequency = frequency;
    timer_ticks = 0;
    
    // Calculate divisor
    // Divisor = base_frequency / desired_frequency
    // Maximum divisor is 65535 (16-bit)
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;
    
    // Cap divisor at 65535 to prevent overflow
    if (divisor > 65535) {
        divisor = 65535;
    }
    
    // Send command byte to PIT
    // 0x36 = 00 11 011 0
    //   - Channel 0
    //   - Access mode: lobyte/hibyte
    //   - Mode 3: Square wave generator
    //   - Binary counter
    outb(PIT_COMMAND, PIT_SET_CHANNEL0);
    
    // Send divisor (low byte, then high byte)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    
    // Unmask IRQ0 (timer interrupt)
    pic_unmask_irq(IRQ_TIMER);
    
    terminal_writestring("[TIMER] Initialized at ");
    uint64_to_string(frequency, buf);
    terminal_writestring(buf);
    terminal_writestring(" Hz (divisor: ");
    uint64_to_string(divisor, buf);
    terminal_writestring(buf);
    terminal_writestring(")\n");
}

/**
 * timer_handler - Handle timer interrupt (IRQ0)
 * @regs: CPU register state
 * 
 * PHASE 9 CHANGE: This function now implements preemptive scheduling!
 * 
 * On each timer tick:
 *   1. Set in_interrupt_context = 1 (prevent nested context switches)
 *   2. Increment timer_ticks (system clock)
 *   3. Call proc_yield() to give CPU to another process
 *   4. Set in_interrupt_context = 0
 * 
 * This enables round-robin preemptive multitasking by forcing
 * the scheduler to run on every timer interrupt.
 */
void timer_handler(registers_t* regs) {
    (void)regs;  // Unused parameter
    
    /**
     * PHASE 9 CHANGE: Mark interrupt context
     * 
     * Set flag before calling scheduler. This tells proc_yield()
     * that we're in interrupt context so it should skip the actual
     * context switch (to prevent nested interrupts from crashing).
     */
    extern volatile int in_interrupt_context;
    in_interrupt_context = 1;
    
    // Increment system tick counter
    timer_ticks++;
    
    /**
     * PHASE 9 CHANGE: Call scheduler for preemptive multitasking
     * 
     * This is the heart of preemptive scheduling - on every timer
     * tick, we give up the CPU so the scheduler can decide which
     * process should run next.
     */
    proc_yield();
    
    // Clear interrupt context flag
    in_interrupt_context = 0;
}

/**
 * timer_get_ticks - Get the current tick count
 * @return: Number of timer ticks since initialization
 */
uint64_t timer_get_ticks(void) {
    return timer_ticks;
}

/**
 * timer_sleep - Busy-wait for a specified number of milliseconds
 * @ms: Number of milliseconds to sleep
 * 
 * Simple busy-wait delay using timer ticks.
 * Not suitable for production use (blocks CPU).
 */
void timer_sleep(uint32_t ms) {
    if (timer_frequency == 0) {
        return;  // Timer not initialized
    }
    
    // Calculate target ticks
    // ticks = (ms * frequency) / 1000
    uint64_t target_ticks = timer_ticks + ((uint64_t)ms * timer_frequency) / 1000;
    
    // Busy wait
    while (timer_ticks < target_ticks) {
        __asm__ volatile("pause");  // Hint to CPU that we're spinning
    }
}

/**
 * timer_print_stats - Print timer statistics
 * 
 * Displays current tick count and uptime.
 */
void timer_print_stats(void) {
    char buf[32];
    
    terminal_writestring("[TIMER] Statistics:\n");
    terminal_writestring("  Total ticks: ");
    uint64_to_string(timer_ticks, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    if (timer_frequency > 0) {
        uint64_t seconds = timer_ticks / timer_frequency;
        uint64_t ms = ((timer_ticks % timer_frequency) * 1000) / timer_frequency;
        
        terminal_writestring("  Uptime: ");
        uint64_to_string(seconds, buf);
        terminal_writestring(buf);
        terminal_writestring(".");
        
        // Print milliseconds with leading zeros
        if (ms < 100) terminal_writestring("0");
        if (ms < 10) terminal_writestring("0");
        uint64_to_string(ms, buf);
        terminal_writestring(buf);
        terminal_writestring(" seconds\n");
        
        terminal_writestring("  Frequency: ");
        uint64_to_string(timer_frequency, buf);
        terminal_writestring(buf);
        terminal_writestring(" Hz\n");
    }
}
