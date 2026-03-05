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

#include <drivers/keyboard.h>
#include <arch/pic.h>
#include <kernel.h>
#include <vga.h>

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
static void keyboard_send_data(uint8_t data) {
    // Wait for input buffer to be clear
    while (inb(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_INPUT) {
        io_wait();
    }
    outb(KEYBOARD_DATA_PORT, data);
}

/**
 * keyboard_read_data - Read data from keyboard
 * 
 * Waits for output buffer to have data.
 * Returns the data byte.
 */
static uint8_t keyboard_read_data(void) {
    // Wait for output buffer to be full (bit 0 = 1)
    while (!(inb(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_OUTPUT)) {
        io_wait();
    }
    return inb(KEYBOARD_DATA_PORT);
}

/**
 * keyboard_wait_ack - Wait for ACK response from keyboard
 * 
 * Returns 1 on ACK, 0 on timeout or error.
 */
static int keyboard_wait_ack(void) {
    uint8_t response = keyboard_read_data();
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
                // Update LED
                if (caps_on) {
                    keyboard_set_leds(led_state | KEYBOARD_LED_CAPS);
                } else {
                    keyboard_set_leds(led_state & ~KEYBOARD_LED_CAPS);
                }
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
        keyboard_buffer_write(c);
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
    
    // Wait for response (ACK followed by 0xAA for success)
    uint8_t response = keyboard_read_data();
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
    char c;
    
    // Wait until there's a character in the buffer
    while ((c = keyboard_buffer_read()) == 0) {
        // Enable interrupts and halt CPU until next interrupt
        __asm__ volatile("hlt");
    }
    
    return c;
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
