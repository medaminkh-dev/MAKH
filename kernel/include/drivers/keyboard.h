/**
 * MakhOS - keyboard.h
 * PS/2 Keyboard driver header
 */

#ifndef MAKHOS_KEYBOARD_H
#define MAKHOS_KEYBOARD_H

#include <types.h>
#include <arch/idt.h>

// PS/2 keyboard ports
#define KEYBOARD_DATA_PORT     0x60
#define KEYBOARD_STATUS_PORT   0x64
#define KEYBOARD_COMMAND_PORT  0x64

// Status register bits
#define KEYBOARD_STATUS_OUTPUT 0x01  // Output buffer full
#define KEYBOARD_STATUS_INPUT  0x02  // Input buffer full

// Keyboard commands
#define KEYBOARD_CMD_LED       0xED
#define KEYBOARD_CMD_ECHO      0xEE
#define KEYBOARD_CMD_SCANCODE  0xF0
#define KEYBOARD_CMD_ENABLE    0xF4
#define KEYBOARD_CMD_RESET     0xFF

// Keyboard responses
#define KEYBOARD_ACK           0xFA
#define KEYBOARD_RESEND        0xFE
#define KEYBOARD_ERROR         0xFC

// LED flags
#define KEYBOARD_LED_SCROLL    0x01
#define KEYBOARD_LED_NUM       0x02
#define KEYBOARD_LED_CAPS      0x04

// Special keys scancodes (Set 1)
#define KEY_ESC         0x01
#define KEY_BACKSPACE   0x0E
#define KEY_TAB         0x0F
#define KEY_ENTER       0x1C
#define KEY_LCTRL       0x1D
#define KEY_LSHIFT      0x2A
#define KEY_RSHIFT      0x36
#define KEY_LALT        0x38
#define KEY_SPACE       0x39
#define KEY_CAPS        0x3A
#define KEY_F1          0x3B
#define KEY_F2          0x3C
#define KEY_F3          0x3D
#define KEY_F4          0x3E
#define KEY_F5          0x3F
#define KEY_F6          0x40
#define KEY_F7          0x41
#define KEY_F8          0x42
#define KEY_F9          0x43
#define KEY_F10         0x44
#define KEY_NUM         0x45
#define KEY_SCROLL      0x46
#define KEY_F11         0x57
#define KEY_F12         0x58

// Arrow key scancodes
#define KEY_UP          0x48
#define KEY_DOWN        0x50
#define KEY_LEFT        0x4B
#define KEY_RIGHT       0x4D

// Extended character codes for special keys (outside ASCII range)
#define CHAR_ARROW_UP    0x80
#define CHAR_ARROW_DOWN  0x81
#define CHAR_ARROW_LEFT  0x82
#define CHAR_ARROW_RIGHT 0x83

// Selection character codes
#define CHAR_SELECT_LEFT    0x84
#define CHAR_SELECT_RIGHT   0x85

// Delete key (forward delete)
#define CHAR_DELETE         0x86

// Ctrl shortcuts (for input line editing)
#define CHAR_CTRL_A         0x90   // Home (start of line)
#define CHAR_CTRL_E         0x91   // End (end of line)
#define CHAR_CTRL_K         0x92   // Kill to end of line
#define CHAR_CTRL_U         0x93   // Kill to start of line
#define CHAR_CTRL_W         0x94   // Kill word back
#define CHAR_CTRL_Y         0x95   // Yank (paste)
#define CHAR_CTRL_L         0x96   // Clear screen

// Release codes are scancode | 0x80
#define KEY_RELEASE_MASK 0x80

// Buffer size
#define KEYBOARD_BUFFER_SIZE 256

// Functions
void keyboard_init(void);
void keyboard_handler(registers_t* regs);
char keyboard_get_char(void);
int keyboard_has_input(void);
void keyboard_clear_buffer(void);
void keyboard_set_leds(uint8_t leds);
uint8_t keyboard_get_modifiers(void);

#endif
