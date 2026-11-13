/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - keyboard.c
 * PS/2 Keyboard driver implementation
 * 
 * Features:
 * - Circular buffer for keystrokes
 * - Dual keymaps (normal/shift)
 * - Modifier tracking (Shift, Ctrl, Alt)
 * - Caps Lock support with LED
 * - Scancode Set 1 support
 */

#include <tty.h>
#include <drivers/keyboard.h>
#include <arch/pic.h>
#include <kernel.h>
#include <vga.h>
#include <irq.h>

// Circular buffer for keystrokes
static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint16_t buffer_head = 0;
static volatile uint16_t buffer_tail = 0;

// Modifier states
static volatile uint8_t shift_pressed = 0;
static volatile uint8_t ctrl_pressed = 0;
static volatile uint8_t alt_pressed = 0;
static volatile uint8_t caps_on = 0;

// Current LED state
static uint8_t led_state = 0;

// Deferred LED update (T-L02): the keyboard IRQ must never block on the PS/2
// controller, so CapsLock in interrupt context only records the wanted LED
// state here; keyboard_service_leds() performs the actual (bounded) PS/2 I/O
// later from task context.
static volatile uint8_t led_update_pending = 0;
static volatile uint8_t led_pending_value  = 0;

// Upper bound on PS/2 controller polls before giving up (T-L03).
#define PS2_SPIN_MAX 500000u

// Extended scancode state (for 0xE0/0xE1 prefixes)
static uint8_t extended_scancode = 0;

// US QWERTY Keymap - Normal state
static const char keymap_normal[] = {
    0,   // 0x00 - Error
    0,   // 0x01 - ESC
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '-', '=',
    '\b', // 0x0E - Backspace
    '\t', // 0x0F - Tab
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', // 0x1C - Enter
    0,    // 0x1D - Left Ctrl
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    // 0x2A - Left Shift
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,    // 0x36 - Right Shift
    '*',  // 0x37 - Keypad *
    0,    // 0x38 - Left Alt
    ' ',  // 0x39 - Space
    0,    // 0x3A - Caps Lock
    0,    // 0x3B - F1
    0,    // 0x3C - F2
    0,    // 0x3D - F3
    0,    // 0x3E - F4
    0,    // 0x3F - F5
    0,    // 0x40 - F6
    0,    // 0x41 - F7
    0,    // 0x42 - F8
    0,    // 0x43 - F9
    0,    // 0x44 - F10
    0,    // 0x45 - Num Lock
    0,    // 0x46 - Scroll Lock
    '7',  // 0x47 - Keypad Home/7
    '8',  // 0x48 - Keypad Up/8
    '9',  // 0x49 - Keypad PgUp/9
    '-',  // 0x4A - Keypad -
    '4',  // 0x4B - Keypad Left/4
    '5',  // 0x4C - Keypad 5
    '6',  // 0x4D - Keypad Right/6
    '+',  // 0x4E - Keypad +
    '1',  // 0x4F - Keypad End/1
    '2',  // 0x50 - Keypad Down/2
    '3',  // 0x51 - Keypad PgDn/3
    '0',  // 0x52 - Keypad Ins/0
    '.',  // 0x53 - Keypad Del/.
    0, 0, // 0x54-0x55
    0,    // 0x56 - Non-US backslash
    0,    // 0x57 - F11
    0,    // 0x58 - F12
};

// US QWERTY Keymap - Shift state
static const char keymap_shift[] = {
    0,   // 0x00 - Error
    0,   // 0x01 - ESC
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '_', '+',
    '\b', // 0x0E - Backspace
    '\t', // 0x0F - Tab
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', // 0x1C - Enter
    0,    // 0x1D - Left Ctrl
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    // 0x2A - Left Shift
    '|',  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,    // 0x36 - Right Shift
    '*',  // 0x37 - Keypad *
    0,    // 0x38 - Left Alt
    ' ',  // 0x39 - Space
    0,    // 0x3A - Caps Lock
    0,    // 0x3B - F1
    0,    // 0x3C - F2
    0,    // 0x3D - F3
    0,    // 0x3E - F4
    0,    // 0x3F - F5
    0,    // 0x40 - F6
    0,    // 0x41 - F7
    0,    // 0x42 - F8
    0,    // 0x43 - F9
    0,    // 0x44 - F10
    0,    // 0x45 - Num Lock
    0,    // 0x46 - Scroll Lock
    '7',  // 0x47 - Keypad Home/7
    '8',  // 0x48 - Keypad Up/8
    '9',  // 0x49 - Keypad PgUp/9
    '-',  // 0x4A - Keypad -
    '4',  // 0x4B - Keypad Left/4
    '5',  // 0x4C - Keypad 5
    '6',  // 0x4D - Keypad Right/6
    '+',  // 0x4E - Keypad +
    '1',  // 0x4F - Keypad End/1
    '2',  // 0x50 - Keypad Down/2
    '3',  // 0x51 - Keypad PgDn/3
    '0',  // 0x52 - Keypad Ins/0
    '.',  // 0x53 - Keypad Del/.
    0, 0, // 0x54-0x55
    0,    // 0x56 - Non-US backslash
    0,    // 0x57 - F11
    0,    // 0x58 - F12
};

/**
 * keyboard_buffer_write - Add character to circular buffer
 * @c: Character to add
 * 
 * Thread-safe for interrupt-driven operation.
 * Returns 1 on success, 0 if buffer is full.
 */
static int keyboard_buffer_write(char c) {
    uint16_t next_head = (buffer_head + 1) % KEYBOARD_BUFFER_SIZE;
    
    // Check if buffer is full
    if (next_head == buffer_tail) {
        return 0;  // Buffer full
    }
    
    keyboard_buffer[buffer_head] = c;
    buffer_head = next_head;
    return 1;
}

/**
 * keyboard_buffer_read - Read character from circular buffer
 * 
 * Returns the character, or 0 if buffer is empty.
 */
static char keyboard_buffer_read(void) {
    if (buffer_head == buffer_tail) {
        return 0;  // Buffer empty
    }
    
    char c = keyboard_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

/**
 * keyboard_send_data - Send data byte to keyboard
 * @data: Data byte to send
 * 
 * Waits for input buffer to be clear before sending.
 */
/* Bounded busy-wait on the PS/2 controller (T-L03): poll busy() until it reads
 * 0 (ready), or `limit` polls elapse. Returns 0 if ready, -1 on timeout — so a
 * wedged or missing controller can never hang the system. Exposed (non-static)
 * so a KTEST can drive it with a stub predicate; the driver passes the real
 * status-port reads below. */
int ps2_spin_until(int (*busy)(void), uint32_t limit) {
    for (uint32_t i = 0; i < limit; i++) {
        if (!busy()) return 0;
        io_wait();
    }
    return -1;
}

static int kbd_input_full(void)   { return inb(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_INPUT; }
static int kbd_output_empty(void) { return !(inb(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_OUTPUT); }

static void keyboard_send_data(uint8_t data) {
    // Wait (bounded) for the input buffer to clear; a stuck controller returns
    // instead of spinning forever. Drop the byte on timeout rather than hang.
    if (ps2_spin_until(kbd_input_full, PS2_SPIN_MAX) != 0) return;
    outb(KEYBOARD_DATA_PORT, data);
}

/**
 * keyboard_read_data - Read data from keyboard
 *
 * Waits (bounded) for the output buffer to fill.
 * Returns the data byte, or -1 on timeout.
 */
static int keyboard_read_data(void) {
    if (ps2_spin_until(kbd_output_empty, PS2_SPIN_MAX) != 0) return -1;
    return inb(KEYBOARD_DATA_PORT);
}

/**
 * keyboard_wait_ack - Wait for ACK response from keyboard
 *
 * Returns 1 on ACK, 0 on timeout or error. (read_data returns -1 on timeout,
 * which is != KEYBOARD_ACK, so the timeout path really does return 0 now.)
 */
static int keyboard_wait_ack(void) {
    int response = keyboard_read_data();
    return (response == KEYBOARD_ACK);
}

/**
 * keyboard_set_leds - Set keyboard LED state
 * @leds: LED flags (KEYBOARD_LED_SCROLL, KEYBOARD_LED_NUM, KEYBOARD_LED_CAPS)
 */
void keyboard_set_leds(uint8_t leds) {
    led_state = leds;
    
    // Send LED command
    keyboard_send_data(KEYBOARD_CMD_LED);
    if (!keyboard_wait_ack()) {
        return;  // Error
    }
    
    // Send LED state
    keyboard_send_data(leds);
    keyboard_wait_ack();
}

/**
 * keyboard_service_leds - Flush a pending LED update from TASK (non-IRQ) context.
 *
 * T-L02: the keyboard IRQ handler must never block on the PS/2 controller, so
 * CapsLock in interrupt context only sets led_update_pending. This performs the
 * actual bounded PS/2 I/O later, with interrupts enabled. A no-op when nothing
 * is pending; safe to call from any non-interrupt path (e.g. keyboard_get_char).
 */
void keyboard_service_leds(void) {
    irqflags_t f = local_irq_save();
    if (!led_update_pending) { local_irq_restore(f); return; }
    uint8_t v = led_pending_value;
    led_update_pending = 0;
    local_irq_restore(f);
    keyboard_set_leds(v);                 /* bounded PS/2 I/O, outside the IRQ */
}

/**
 * keyboard_get_modifiers - Get current modifier state
 * 
 * Returns bitmask of active modifiers.
 */
uint8_t keyboard_get_modifiers(void) {
    uint8_t mods = 0;
    if (shift_pressed) mods |= 0x01;
    if (ctrl_pressed) mods |= 0x02;
    if (alt_pressed) mods |= 0x04;
    if (caps_on) mods |= 0x08;
    return mods;
}

/**
 * keyboard_handle_scancode - Process a keyboard scancode
 * @scancode: The scancode from the keyboard
 * 
 * Updates modifier states and adds characters to buffer.
 */
static void keyboard_handle_scancode(uint8_t scancode) {
    // Handle extended scancode prefix (0xE0 for arrow keys)
    if (scancode == 0xE0 || scancode == 0xE1) {
        extended_scancode = scancode;
        return;
    }
    
    int is_release = (scancode & KEY_RELEASE_MASK);
    uint8_t key = scancode & ~KEY_RELEASE_MASK;
    
    // For extended scancodes, check the key code after 0xE0
    if (extended_scancode == 0xE0 && !is_release) {
        /* User-shell mode: deliver navigation keys to the line discipline as the
         * conventional ANSI escape sequences (ESC [ A/B/C/D, ESC [ 3 ~), so the
         * ring-3 shell's line editor can read arrows, Delete and history. */
        if (tty_is_active()) {
            char fin = 0;
            switch (key) {
                case KEY_UP:    fin = 'A'; break;
                case KEY_DOWN:  fin = 'B'; break;
                case KEY_RIGHT: fin = 'C'; break;
                case KEY_LEFT:  fin = 'D'; break;
                case 0x53:      /* Delete */
                    tty_input(27); tty_input('['); tty_input('3'); tty_input('~');
                    extended_scancode = 0; return;
            }
            if (fin) { tty_input(27); tty_input('['); tty_input(fin); }
            extended_scancode = 0;
            return;
        }
        switch (key) {
            case KEY_UP:    keyboard_buffer_write(CHAR_ARROW_UP);    extended_scancode = 0; return;
            case KEY_DOWN:  keyboard_buffer_write(CHAR_ARROW_DOWN);  extended_scancode = 0; return;
            case KEY_LEFT:
                if (shift_pressed) {
                    keyboard_buffer_write(CHAR_SELECT_LEFT);
                } else {
                    keyboard_buffer_write(CHAR_ARROW_LEFT);
                }
                extended_scancode = 0;
                return;
            case KEY_RIGHT:
                if (shift_pressed) {
                    keyboard_buffer_write(CHAR_SELECT_RIGHT);
                } else {
                    keyboard_buffer_write(CHAR_ARROW_RIGHT);
                }
                extended_scancode = 0;
                return;
            case 0x53:  // Delete key (extended scancode)
                keyboard_buffer_write(CHAR_DELETE);
                extended_scancode = 0;
                return;
        }
    }
    extended_scancode = 0;
    
    // Handle modifier keys
    switch (key) {
        case KEY_LSHIFT:
        case KEY_RSHIFT:
            shift_pressed = !is_release;
            return;
            
        case KEY_LCTRL:
            ctrl_pressed = !is_release;
            return;
            
        case KEY_LALT:
            alt_pressed = !is_release;
            return;
            
        case KEY_CAPS:
            if (!is_release) {
                // Toggle caps lock on press (not release)
                caps_on = !caps_on;
                // T-L02: we are in the IRQ handler — never block on the PS/2
                // controller here. Record the wanted LED state and defer the
                // actual write to keyboard_service_leds() in task context.
                led_pending_value = caps_on ? (uint8_t)(led_state | KEYBOARD_LED_CAPS)
                                            : (uint8_t)(led_state & ~KEYBOARD_LED_CAPS);
                led_update_pending = 1;
            }
            return;
            
        default:
            break;
    }
    
    // Ignore key releases for character input
    if (is_release) {
        return;
    }

    // Handle arrow keys specially - write extended codes to buffer
    switch (key) {
        case KEY_UP:
            keyboard_buffer_write(CHAR_ARROW_UP);
            return;
        case KEY_DOWN:
            keyboard_buffer_write(CHAR_ARROW_DOWN);
            return;
        case KEY_LEFT:
            if (shift_pressed) {
                keyboard_buffer_write(CHAR_SELECT_LEFT);
            } else {
                keyboard_buffer_write(CHAR_ARROW_LEFT);
            }
            return;
        case KEY_RIGHT:
            if (shift_pressed) {
                keyboard_buffer_write(CHAR_SELECT_RIGHT);
            } else {
                keyboard_buffer_write(CHAR_ARROW_RIGHT);
            }
            return;
    }

    // Handle Ctrl shortcuts
    if (ctrl_pressed && !is_release) {
        switch (key) {
            case 0x1E: keyboard_buffer_write(CHAR_CTRL_A); return;  // Ctrl+A
            case 0x12: keyboard_buffer_write(CHAR_CTRL_E); return;  // Ctrl+E
            case 0x25: keyboard_buffer_write(CHAR_CTRL_K); return;  // Ctrl+K
            case 0x16: keyboard_buffer_write(CHAR_CTRL_U); return;  // Ctrl+U
            case 0x11: keyboard_buffer_write(CHAR_CTRL_W); return;  // Ctrl+W
            case 0x15: keyboard_buffer_write(CHAR_CTRL_Y); return;  // Ctrl+Y
            case 0x26: keyboard_buffer_write(CHAR_CTRL_L); return;  // Ctrl+L
        }
    }

    // Get the character from the appropriate keymap
    const char* keymap = shift_pressed ? keymap_shift : keymap_normal;
    char c = 0;
    
    if (key < sizeof(keymap_normal)) {
        c = keymap[key];
    }
    
    // Handle caps lock (affects letters only)
    if (c && caps_on && !shift_pressed) {
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }
    } else if (c && caps_on && shift_pressed) {
        // Shift + Caps = lowercase
        if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }
    
    // Add to buffer if we have a valid character
    if (c) {
        if (tty_is_active()) {
            /* User-shell mode: route keystrokes through the terminal line
             * discipline. Ctrl+<letter> becomes the control byte (^C=3, ^D=4,
             * ^Z=26) so ISIG/EOF work. */
            char t = c;
            if (ctrl_pressed && (c | 0x20) >= 'a' && (c | 0x20) <= 'z')
                t = (char)((c | 0x20) - 'a' + 1);
            tty_input(t);
        } else {
            keyboard_buffer_write(c);          /* legacy kernel shell path */
        }
    }
}

/**
 * keyboard_handler - IRQ1 interrupt handler for keyboard
 * @regs: CPU register state
 * 
 * Called when a key is pressed or released.
 */
void keyboard_handler(registers_t* regs) {
    (void)regs;  // Unused parameter
    
    // Read scancode from keyboard data port
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Process the scancode
    keyboard_handle_scancode(scancode);
}

/**
 * keyboard_init - Initialize the PS/2 keyboard
 * 
 * Resets the keyboard, enables it, and unmasks IRQ1.
 */
void keyboard_init(void) {
    terminal_writestring("[KEYBOARD] Initializing PS/2 controller...\n");
    
    // Clear the buffer
    keyboard_clear_buffer();
    
    // Reset keyboard
    keyboard_send_data(KEYBOARD_CMD_RESET);
    
    // Wait for response (ACK followed by 0xAA for success); read_data returns
    // -1 on timeout, so a missing controller no longer hangs boot.
    int response = keyboard_read_data();
    if (response == KEYBOARD_ACK) {
        response = keyboard_read_data();
        if (response == 0xAA) {
            terminal_writestring("[KEYBOARD] Reset successful\n");
        } else if (response == 0xFC || response == 0xFD) {
            terminal_writestring("[KEYBOARD] Reset failed (self-test error)\n");
        }
    }
    
    // Enable keyboard scanning
    keyboard_send_data(KEYBOARD_CMD_ENABLE);
    keyboard_wait_ack();
    
    // Unmask IRQ1 (keyboard)
    pic_unmask_irq(IRQ_KEYBOARD);
    
    terminal_writestring("[KEYBOARD] Initialized (IRQ1 unmasked)\n");
    terminal_writestring("[KEYBOARD] Ready for input\n");
}

/**
 * keyboard_get_char - Get a character from the keyboard buffer
 * 
 * Returns the next character from the buffer, or 0 if empty.
 * This function blocks until a key is available.
 */
char keyboard_get_char(void) {
    for (;;) {
        // Flush any deferred LED update (T-L02) from task context, interrupts on.
        keyboard_service_leds();

        // T-L04: check the buffer with interrupts OFF, so a keyboard IRQ cannot
        // fill it in the window between the check and the sleep.
        __asm__ volatile("cli");
        char c = keyboard_buffer_read();
        if (c) { __asm__ volatile("sti"); return c; }

        // Buffer empty: `sti` defers interrupt delivery until after the NEXT
        // instruction, so `sti; hlt` atomically enables interrupts and halts —
        // no lost wakeup, and we never hlt with interrupts disabled.
        __asm__ volatile("sti; hlt");
    }
}

/**
 * keyboard_has_input - Check if there's input waiting
 * 
 * Returns 1 if a character is available, 0 otherwise.
 */
int keyboard_has_input(void) {
    return (buffer_head != buffer_tail);
}

/**
 * keyboard_clear_buffer - Clear the keyboard buffer
 */
void keyboard_clear_buffer(void) {
    buffer_head = 0;
    buffer_tail = 0;
}

/* ---- test seam ------------------------------------------------------------
 * These let a KTEST drive the scancode path and inspect the deferred-LED state
 * without a physical keyboard. keyboard_inject_scancode feeds the same code the
 * IRQ handler runs; the accessors expose the otherwise-static state. */
void keyboard_inject_scancode(uint8_t sc)   { keyboard_handle_scancode(sc); }
int  keyboard_test_led_pending(void)        { return led_update_pending; }
uint8_t keyboard_test_led_value(void)       { return led_pending_value; }
int  keyboard_test_caps(void)               { return caps_on; }
void keyboard_test_push(char c)             { keyboard_buffer_write(c); }
