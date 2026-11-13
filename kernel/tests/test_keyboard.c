/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_keyboard.c
 * Phase-0 liveness regressions for the PS/2 keyboard driver (AUDIT.md
 * T-L02/T-L03/T-L04). Each test fails if its fix is reverted:
 *  - T-L03: the bounded PS/2 wait returns a timeout instead of spinning forever.
 *  - T-L02: CapsLock arriving via the IRQ scancode path only *defers* the LED
 *    write; it must not perform (blocking) PS/2 I/O in interrupt context.
 *  - T-L04: keyboard_get_char returns a buffered character via its interrupts-
 *    off check path, without halting.
 */
#include <ktest.h>
#include <drivers/keyboard.h>
#include <drivers/timer.h>

/* -------- T-L03: PS/2 waits are bounded -------- */

static int kt_always_busy(void) { return 1; }          /* a wedged controller */
static int kt_countdown;
static int kt_busy_then_ready(void) { return kt_countdown-- > 0; }

KTEST(keyboard, ps2_wait_is_bounded) {
    /* A controller whose status bit never clears must NOT hang the system:
     * the bounded wait returns -1 (timeout). Revert the fix (unbounded while)
     * and this test spins forever instead of returning. */
    KEXPECT_EQ(ps2_spin_until(kt_always_busy, 100000u), -1);

    /* A controller that becomes ready after a few polls succeeds (returns 0). */
    kt_countdown = 8;
    KEXPECT_EQ(ps2_spin_until(kt_busy_then_ready, 100000u), 0);
}

/* -------- T-L02: CapsLock defers the LED write out of IRQ context -------- */

KTEST(keyboard, capslock_in_irq_defers_led_write) {
    keyboard_service_leds();                      /* drain anything left pending */

    int caps_before = keyboard_test_caps();
    keyboard_inject_scancode(0x3A);               /* CapsLock press (scancode set 1) */

    /* The IRQ path must only record the wanted LED state, never block on PS/2. */
    KEXPECT(keyboard_test_led_pending());
    KEXPECT_EQ(keyboard_test_caps(), !caps_before);
    if (!caps_before)                              /* CAPS bit requested when turning on */
        KEXPECT(keyboard_test_led_value() & KEYBOARD_LED_CAPS);

    /* The deferred write is flushed only from task context; it then clears. */
    keyboard_service_leds();
    KEXPECT_EQ(keyboard_test_led_pending(), 0);

    keyboard_inject_scancode(0x3A);               /* restore caps to its prior state */
    keyboard_service_leds();
}

/* -------- T-L04: get_char returns a buffered char without halting -------- */

KTEST(keyboard, get_char_returns_buffered_without_halt) {
    keyboard_clear_buffer();
    keyboard_test_push('a');                      /* buffer non-empty before the call */
    KASSERT_TEST(keyboard_has_input());           /* guard: never enter the hlt path */
    char c = keyboard_get_char();                 /* returns via the cli-check/sti path */
    KEXPECT_EQ(c, 'a');
    KEXPECT_EQ(keyboard_has_input(), 0);           /* consumed exactly one char */
}

/* Injected from the timer IRQ while get_char is parked on its hlt. */
static void kt_push_from_irq(void) { keyboard_test_push('Z'); }

KTEST(keyboard, get_char_wakes_from_empty_without_lost_wakeup) {
    /* This is the real T-L04 regression. We empty the buffer and enter
     * keyboard_get_char with interrupts DISABLED, so the only way it can ever
     * return is to re-enable interrupts and halt atomically (`sti; hlt`) and
     * then be woken by an IRQ. A timer one-shot pushes a char from true IRQ
     * context during that halt. The fix wakes, re-checks, and returns 'Z'.
     *
     * Revert to the original `while(!c) { hlt; }` (a *bare* hlt, no sti): with
     * interrupts off on entry, the CPU halts with IF=0, the timer IRQ never
     * fires, the char is never injected, and get_char hangs forever -> the
     * suite times out. That is the lost-wakeup / unsafe-halt defect. */
    keyboard_clear_buffer();
    KASSERT_TEST(keyboard_has_input() == 0);      /* start empty: must reach the halt path */

    timer_test_fire_once(kt_push_from_irq);       /* next tick delivers the char */
    __asm__ volatile("cli");                      /* enter with interrupts OFF */
    char c = keyboard_get_char();                 /* must sti;hlt, wake, return */
    /* get_char's sti path left interrupts enabled; nothing to restore. */

    KEXPECT_EQ(c, 'Z');
    KEXPECT_EQ(keyboard_has_input(), 0);          /* consumed exactly one char */
}
