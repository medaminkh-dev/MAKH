# MakhOS Phase 6 - Keyboard Driver and Input Line Editing

## Overview

Phase 6 introduces a complete PS/2 keyboard driver with interrupt-driven input handling and a GNU Readline-style line editing system. This enables interactive user input with advanced editing capabilities including cursor movement, text selection, command history, and Emacs-style keyboard shortcuts.

## Release Date
March 5, 2026

## Version
**v0.0.2 - Phase 2 Complete**

---

## Summary of Changes

### New Files (4 files)

#### Keyboard Driver (2 files)
1. **[`kernel/include/drivers/keyboard.h`](kernel/include/drivers/keyboard.h:1)** - Keyboard driver interface with scancodes and extended char codes
2. **[`kernel/drivers/keyboard.c`](kernel/drivers/keyboard.c:1)** - PS/2 keyboard driver implementation

#### Input Line Editing (2 files)
3. **[`kernel/include/input_line.h`](kernel/include/input_line.h:1)** - Line editing buffer and history interface
4. **[`kernel/input_line.c`](kernel/input_line.c:1)** - GNU Readline-style line editing implementation

### Modified Files (7 files)

5. **[`Makefile`](Makefile:30)** - Added `keyboard.c` and `input_line.c` to build, enabled debug flags
6. **[`docs/CHANGELOG_v0.0.2.md`](docs/CHANGELOG_v0.0.2.md:1)** - Updated with Phase 6 details
7. **[`kernel/arch/idt.c`](kernel/arch/idt.c:212)** - Added IRQ1 keyboard interrupt handler registration
8. **[`kernel/include/kernel.h`](kernel/include/kernel.h:14)** - Updated phase marker to "Phase 2 Complete"
9. **[`kernel/include/vga.h`](kernel/include/vga.h:44)** - Added selection tracking, cursor style functions, delete operations
10. **[`kernel/kernel.c`](kernel/kernel.c:690)** - Added interactive shell with full line editing, command history, Emacs shortcuts
11. **[`kernel/vga.c`](kernel/vga.c:16)** - Added hardware cursor sync, insert mode, text selection highlighting

---

## Detailed Changes

### 1. PS/2 Keyboard Driver

#### Hardware Background
The PS/2 keyboard controller uses I/O ports 0x60 (data) and 0x64 (command/status). It generates IRQ1 when a key is pressed or released, sending scancodes that must be translated to ASCII characters.

#### Scancode Set 1 Translation
The driver uses Scancode Set 1, the default for PS/2 keyboards:
- **Make codes**: 0x01-0x58 for key presses
- **Break codes**: Make code | 0x80 (bit 7 set) for releases
- **Extended codes**: 0xE0 prefix for special keys (arrows, function keys)

#### Circular Buffer Architecture
```c
#define KEYBOARD_BUFFER_SIZE 256

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint16_t buffer_head = 0;
static volatile uint16_t buffer_tail = 0;
```

The circular buffer provides:
- **Thread-safe operation**: Interrupt-driven writes, polled reads
- **256-character capacity**: Sufficient for rapid typing
- **Non-blocking checks**: `keyboard_has_input()` for polling
- **Blocking reads**: `keyboard_get_char()` waits with HLT

#### Dual Keymap System

**Normal Keymap (lowercase):**
```c
static const char keymap_normal[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ', 0, /* F1-F10 */ 0,0,0,0,0,0,0,0,0,0,
    0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};
```

**Shift Keymap (uppercase/symbols):**
```c
static const char keymap_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' ', 0, /* F1-F10 */ 0,0,0,0,0,0,0,0,0,0,
    0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
};
```

#### Extended Character Codes
Special keys are mapped to extended codes (0x80+) for application handling:

| Code | Name | Description |
|------|------|-------------|
| 0x80 | CHAR_ARROW_UP | Up arrow key |
| 0x81 | CHAR_ARROW_DOWN | Down arrow key |
| 0x82 | CHAR_ARROW_LEFT | Left arrow key |
| 0x83 | CHAR_ARROW_RIGHT | Right arrow key |
| 0x84 | CHAR_SELECT_LEFT | Shift+Left (extend selection) |
| 0x85 | CHAR_SELECT_RIGHT | Shift+Right (extend selection) |
| 0x86 | CHAR_DELETE | Delete key (forward delete) |
| 0x90 | CHAR_CTRL_A | Ctrl+A (start of line) |
| 0x91 | CHAR_CTRL_E | Ctrl+E (end of line) |
| 0x92 | CHAR_CTRL_K | Ctrl+K (kill to end) |
| 0x93 | CHAR_CTRL_U | Ctrl+U (kill to start) |
| 0x94 | CHAR_CTRL_W | Ctrl+W (kill word back) |
| 0x95 | CHAR_CTRL_Y | Ctrl+Y (yank/paste) |
| 0x96 | CHAR_CTRL_L | Ctrl+L (clear screen) |

#### API Functions

**`void keyboard_init(void)`**
- Resets the PS/2 keyboard controller
- Performs self-test (expects 0xAA response)
- Enables keyboard scanning
- Unmasks IRQ1 for keyboard interrupts
- Output: `[KEYBOARD] Initialized (IRQ1 unmasked)`

**`void keyboard_handler(registers_t* regs)`**
- IRQ1 interrupt handler
- Reads scancode from port 0x60
- Processes modifier keys (Shift, Ctrl, Alt, Caps Lock)
- Translates scancodes to ASCII or extended codes
- Writes to circular buffer

**`char keyboard_get_char(void)`**
- Returns next character from buffer (blocking)
- Uses `hlt` instruction to wait for interrupts
- Returns 0 if interrupted or error

**`int keyboard_has_input(void)`**
- Non-blocking check for available input
- Returns 1 if character available, 0 otherwise

**`void keyboard_clear_buffer(void)`**
- Clears the circular buffer
- Resets head and tail pointers to 0

**`void keyboard_set_leds(uint8_t leds)`**
- Controls keyboard LEDs (Scroll, Num, Caps)
- Parameters: Bitmask of `KEYBOARD_LED_*` flags

**`uint8_t keyboard_get_modifiers(void)`**
- Returns current modifier state bitmask
- Bits: 0x01=Shift, 0x02=Ctrl, 0x04=Alt, 0x08=Caps

---

### 2. Input Line Editing System

#### Architecture Overview
The input line system provides a GNU Readline-compatible editing interface with:
- **Line buffer**: 256-character maximum line length
- **Command history**: 50-entry circular buffer
- **Kill buffer**: Clipboard for cut/paste operations
- **Visual selection**: Shift+arrow text highlighting

#### Data Structures

**Input Line State:**
```c
typedef struct {
    char    buffer[INPUT_LINE_MAX];     // Line content
    int     length;                     // Current length
    int     cursor;                     // Cursor position
    int     sel_start;                  // Selection start (-1 = none)
    int     sel_end;                    // Selection end
} input_line_t;
```

**Command History:**
```c
typedef struct {
    char    entries[INPUT_HISTORY_MAX][INPUT_LINE_MAX];  // History buffer
    int     count;                      // Number of entries
    int     current;                    // Navigation position
} input_history_t;
```

#### Line Editing Functions

**Cursor Movement:**
- `input_line_move_left()` - Move cursor left, clear selection
- `input_line_move_right()` - Move cursor right, clear selection
- `input_line_move_home()` - Jump to start of line (Ctrl+A)
- `input_line_move_end()` - Jump to end of line (Ctrl+E)
- `input_line_move_word_left()` - Previous word (Ctrl+Left)
- `input_line_move_word_right()` - Next word (Ctrl+Right)

**Text Operations:**
- `input_line_insert(line, c)` - Insert character at cursor
- `input_line_backspace(line)` - Delete character before cursor
- `input_line_delete(line)` - Delete character at cursor (forward delete)
- `input_line_clear(line)` - Clear entire line

**Kill and Yank (Cut and Paste):**
- `input_line_kill_to_end(line)` - Cut from cursor to end (Ctrl+K)
- `input_line_kill_to_start(line)` - Cut from start to cursor (Ctrl+U)
- `input_line_kill_word_back(line)` - Cut previous word (Ctrl+W)
- `input_line_yank(line)` - Paste at cursor (Ctrl+Y)

**Selection:**
- `input_line_sel_clear(line)` - Clear selection
- `input_line_sel_delete(line)` - Delete selected text
- `input_line_sel_extend_left(line)` - Extend selection left (Shift+Left)
- `input_line_sel_extend_right(line)` - Extend selection right (Shift+Right)
- `input_line_has_selection(line)` - Check if selection active

**History:**
- `input_history_init(hist)` - Initialize history buffer
- `input_history_push(hist, line)` - Add command to history
- `input_history_prev(hist)` - Get previous entry (Up arrow)
- `input_history_next(hist)` - Get next entry (Down arrow)
- `input_history_load(line, entry)` - Load entry into line buffer

**Rendering:**
- `input_line_render(line, prompt, row)` - Display line with selection highlighting

---

### 3. VGA Enhancements

#### Hardware Cursor Control
```c
void terminal_enable_line_cursor(void);    // Single line cursor
void terminal_enable_block_cursor(void);   // Full block cursor
```

The VGA CRTC (CRT Controller) registers 0x0A-0x0B control cursor appearance:
- **Scanline 14**: Single line cursor (insert mode indicator)
- **Scanlines 0-15**: Full block cursor (normal mode)

#### Selection Highlighting
```c
void terminal_highlight_selection(void);       // Apply highlight color
void terminal_clear_selection_highlight(void); // Restore normal color
```

Highlight uses inverted colors: Black text on Light Cyan background

#### Delete Operations
```c
void terminal_delete_selection(void);   // Delete selected range
void terminal_delete_forward(void);     // Delete at cursor (Delete key)
```

#### Insert Mode
The `terminal_putchar()` function now supports insert mode:
- Shifts existing characters right before writing
- Maintains proper character alignment
- Updates hardware cursor position

---

### 4. Kernel Integration

#### IDT Modifications ([`kernel/arch/idt.c`](kernel/arch/idt.c:212))

**Added keyboard dispatch in `irq_handler()`:**
```c
case IRQ_KEYBOARD:
    // Keyboard interrupt (IRQ1)
    keyboard_handler(regs);
    break;
```

#### Main Input Loop ([`kernel/kernel.c`](kernel/kernel.c:690))

The main loop implements a full interactive shell:

```c
input_line_t current_line;
input_history_t history;
input_line_init(&current_line);
input_history_init(&history);

while (1) {
    input_line_render(&current_line, "MakhOS> ", prompt_row);
    unsigned char c = (unsigned char)keyboard_get_char();
    
    switch (c) {
        case '\n':
            // Execute command
            input_history_push(&history, current_line.buffer);
            break;
        case CHAR_ARROW_UP:
            // Previous history
            input_history_load(&current_line, input_history_prev(&history));
            break;
        case CHAR_ARROW_DOWN:
            // Next history
            input_history_load(&current_line, input_history_next(&history));
            break;
        // ... other key handling
    }
}
```

---

## Testing

### Keyboard Test Output
```
[TEST] Testing Keyboard Input...
Press some keys (they will appear on screen)
Type 'TEST' to complete the test...

T
E
S
T

[OK] Successfully typed 'TEST'
[TEST] Keyboard test complete
```

### Interactive Shell Features

| Key | Action |
|-----|--------|
| Type characters | Insert at cursor position |
| Backspace | Delete character before cursor |
| Delete | Delete character at cursor |
| Left/Right arrows | Move cursor |
| Up/Down arrows | Navigate command history |
| Shift+Left/Right | Select text (highlighted) |
| Ctrl+A | Jump to start of line |
| Ctrl+E | Jump to end of line |
| Ctrl+K | Cut from cursor to end |
| Ctrl+U | Cut from start to cursor |
| Ctrl+W | Cut previous word |
| Ctrl+Y | Paste (yank) |
| Ctrl+L | Clear screen |
| Enter | Execute command, add to history |

---

## Architecture Flow

```
User presses key
       |
       v
PS/2 Keyboard Controller
       |
       v
   IRQ1 Triggered
       |
       v
   PIC (Master)
       |
       v
   Vector 0x21 (33)
       |
       v
   CPU -> isr33 (ASM stub)
       |
       v
   Save registers
       |
       v
   Call exception_handler()
       |
       v
   Detect vector 33 (IRQ1)
       |
       v
   Call irq_handler()
       |
       v
   Dispatch to keyboard_handler()
       |
       v
   Read scancode from port 0x60
       |
       v
   Translate to ASCII/extended code
       |
       v
   Write to circular buffer
       |
       v
   Send EOI to PIC
       |
       v
   IRET (return from interrupt)
       |
       v
   kernel_get_char() returns
       |
       v
   Input line editor processes key
       |
       v
   Render updated line to screen
```

---

## Files Changed

### New Files
- [`kernel/include/drivers/keyboard.h`](kernel/include/drivers/keyboard.h:1) - 108 lines
- [`kernel/drivers/keyboard.c`](kernel/drivers/keyboard.c:1) - 470 lines
- [`kernel/include/input_line.h`](kernel/include/input_line.h:1) - 176 lines
- [`kernel/input_line.c`](kernel/input_line.c:1) - 437 lines

### Modified Files
- [`Makefile`](Makefile:30) - Added keyboard.c and input_line.c to C_SOURCES
- [`docs/CHANGELOG_v0.0.2.md`](docs/CHANGELOG_v0.0.2.md:1) - Added Phase 6 details
- [`kernel/arch/idt.c`](kernel/arch/idt.c:212) - Added IRQ1 keyboard dispatch
- [`kernel/include/kernel.h`](kernel/include/kernel.h:14) - Updated phase string
- [`kernel/include/vga.h`](kernel/include/vga.h:44) - Added selection and cursor functions
- [`kernel/kernel.c`](kernel/kernel.c:690) - Added interactive shell main loop
- [`kernel/vga.c`](kernel/vga.c:16) - Added cursor styles and selection highlighting

---

## Technical Specifications

### PS/2 Controller I/O Ports

| Port | Direction | Purpose |
|------|-----------|---------|
| 0x60 | Read/Write | Data register |
| 0x64 | Read | Status register |
| 0x64 | Write | Command register |

### Status Register Bits

| Bit | Name | Description |
|-----|------|-------------|
| 0 | OUTPUT | Output buffer full (data available) |
| 1 | INPUT | Input buffer full (command pending) |
| 2 | SYSTEM | System flag |
| 3 | CMD | Command/data select |
| 4 | KEYLOCK | Keyboard locked |
| 5 | AUX | Auxiliary device (mouse) |
| 6 | TIMEOUT | Timeout error |
| 7 | PARITY | Parity error |

### Keyboard Commands

| Command | Value | Description |
|---------|-------|-------------|
| LED Control | 0xED | Set keyboard LEDs |
| Echo | 0xEE | Request echo response |
| Scancode Set | 0xF0 | Get/set scancode set |
| Enable | 0xF4 | Enable keyboard scanning |
| Reset | 0xFF | Reset and self-test |

### LED Flags

| Flag | Value | LED |
|------|-------|-----|
| KEYBOARD_LED_SCROLL | 0x01 | Scroll Lock |
| KEYBOARD_LED_NUM | 0x02 | Num Lock |
| KEYBOARD_LED_CAPS | 0x04 | Caps Lock |

### VGA CRTC Registers

| Register | Port | Purpose |
|----------|------|---------|
| Cursor Start | 0x0A | Cursor scanline start |
| Cursor End | 0x0B | Cursor scanline end |
| Cursor Location High | 0x0E | Cursor position high byte |
| Cursor Location Low | 0x0F | Cursor position low byte |

### Buffer Sizes

| Buffer | Size | Purpose |
|--------|------|---------|
| Keyboard buffer | 256 chars | Interrupt-driven input queue |
| Input line | 256 chars | Maximum line length |
| Command history | 50 entries | Previous commands |
| Clipboard | 256 chars | Kill/yank operations |

---

## Future Enhancements

- **USB HID support**: Replace PS/2 with USB keyboard support
- **Unicode input**: Multi-byte character support
- **Tab completion**: Command and filename completion
- **Search history**: Reverse search with Ctrl+R
- **Multi-line editing**: Support for multi-line input
- **Key remapping**: Customizable key bindings
- **Macro recording**: Record and replay keystrokes
