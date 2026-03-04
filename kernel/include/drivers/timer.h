/**
 * MakhOS - timer.h
 * PIT (Programmable Interval Timer) driver header
 */

#ifndef MAKHOS_TIMER_H
#define MAKHOS_TIMER_H

#include <types.h>
#include <arch/idt.h>

// PIT ports
#define PIT_CHANNEL0    0x40
#define PIT_CHANNEL1    0x41
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43

// PIT commands
#define PIT_SET_CHANNEL0  0x36  // Channel 0, lobyte/hibyte, mode 3

// Timer frequency (100Hz = 10ms ticks)
#define TIMER_FREQUENCY 100

// PIT base frequency (1.193182 MHz)
#define PIT_BASE_FREQUENCY 1193182

// Functions
void timer_init(uint32_t frequency);
void timer_handler(registers_t* regs);
uint64_t timer_get_ticks(void);
void timer_sleep(uint32_t ms);
void timer_print_stats(void);

#endif
