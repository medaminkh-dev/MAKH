/**
 * MakhOS - vga.c
 * VGA text mode driver implementation
 */

#include "include/vga.h"
#include "include/kernel.h"

/* Static terminal state */
static struct terminal_state terminal;

/**
 * terminal_enable_line_cursor - Set cursor to line style (instead of block/underscore)
 * Uses VGA scanline registers to create a line cursor appearance
 */
void terminal_enable_line_cursor(void) {
    /* Set cursor scanlines for a true single-line cursor at bottom of character */
    outb(0x3D4, 0x0A);                          /* Cursor Start Register */
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);      /* Start scanline = 14 */
    
    outb(0x3D4, 0x0B);                          /* Cursor End Register */
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 14);      /* End scanline = 14 (same = single line) */
}

/**
 * terminal_enable_block_cursor - Set cursor to full block style
 * Uses VGA scanline registers to create a block cursor (scanlines 0-15)
 */
void terminal_enable_block_cursor(void) {
    outb(0x3D4, 0x0A);                          /* Cursor Start Register */
    uint8_t start = inb(0x3D5);
    outb(0x3D5, (start & 0xC0) | 0);            /* scanline start = 0 */

    outb(0x3D4, 0x0B);                          /* Cursor End Register */
    uint8_t end = inb(0x3D5);
    outb(0x3D5, (end & 0xE0) | 15);             /* scanline end = 15 */
}

/**
 * terminal_update_cursor - Update hardware cursor position to match logical position
 */
static void terminal_update_cursor(void) {
    uint16_t pos = terminal.row * VGA_WIDTH + terminal.column;
    outb(0x3D4, 0x0F);          /* Cursor location low byte */
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);          /* Cursor location high byte */
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/**
 * terminal_initialize - Initialize the VGA terminal
 * Sets up the terminal at position (0,0) with light grey on black
 */
void terminal_initialize(void) {
    terminal.buffer = (uint16_t*)VGA_BUFFER_ADDR;
    terminal.row = 0;
    terminal.column = 0;
    terminal.color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal.selection_active = 0;
    terminal.sel_start_row = 0;
    terminal.sel_start_col = 0;
    terminal.sel_end_row = 0;
    terminal.sel_end_col = 0;
    
    /* Clear the screen */
    terminal_clear();
    
    /* Enable block cursor style */
    terminal_enable_block_cursor();
}

/**
 * terminal_initialize_noclear - Initialize VGA without clearing screen
 * Preserves existing content (useful for preserving bootloader messages)
 */
void terminal_initialize_noclear(void) {
    terminal.buffer = (uint16_t*)VGA_BUFFER_ADDR;
    terminal.row = 3;  /* Start after bootloader messages */
    terminal.column = 0;
    terminal.color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal.selection_active = 0;
    terminal.sel_start_row = 0;
    terminal.sel_start_col = 0;
    terminal.sel_end_row = 0;
    terminal.sel_end_col = 0;
    /* Intentionally NOT calling terminal_clear() */
    
    /* Enable block cursor style */
    terminal_enable_block_cursor();
}

/**
 * terminal_setcolor - Set the current text color
 * @color: The color byte (foreground | background << 4)
 */
void terminal_setcolor(uint8_t color) {
    terminal.color = color;
}

/**
 * terminal_clear - Clear the entire screen
 */
void terminal_clear(void) {
    size_t index;
    uint16_t blank = vga_entry(' ', terminal.color);
    
    for (index = 0; index < VGA_WIDTH * VGA_HEIGHT; index++) {
        terminal.buffer[index] = blank;
    }
    
    terminal.row = 0;
    terminal.column = 0;
}

/**
 * terminal_newline - Move to the next line
 */
void terminal_newline(void) {
    terminal.column = 0;
    
    if (++terminal.row == VGA_HEIGHT) {
        /* Scroll up by copying all lines up one position */
        size_t i;
        for (i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
            terminal.buffer[i] = terminal.buffer[i + VGA_WIDTH];
        }
        
        /* Clear the bottom line */
        for (i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            terminal.buffer[i] = vga_entry(' ', terminal.color);
        }
        
        terminal.row = VGA_HEIGHT - 1;
    }
}

/**
 * terminal_putchar - Write a character to the terminal
 * @c: The character to write
 *
 * Supports '\n' for newline, '\b' for simple backspace (no shift)
 * Insert mode: typing in middle of line shifts characters right
 */
void terminal_putchar(char c) {
    /* Handle newline */
    if (c == '\n') {
        terminal_newline();
        terminal_update_cursor();
        return;
    }
    
    /* Handle carriage return */
    if (c == '\r') {
        terminal.column = 0;
        terminal_update_cursor();
        return;
    }
    
    /* Handle backspace - shift text left */
    if (c == '\b') {
        if (terminal.column > 0) {
            terminal.column--;
            size_t row_base = terminal.row * VGA_WIDTH;
            size_t del_pos  = row_base + terminal.column;
            size_t row_end  = row_base + VGA_WIDTH - 1;
            
            /* Shift all characters after deletion point to the left */
            for (size_t i = del_pos; i < row_end; i++) {
                terminal.buffer[i] = terminal.buffer[i + 1];
            }
            /* Clear last character in row */
            terminal.buffer[row_end] = vga_entry(' ', terminal.color);
            
            terminal_update_cursor();
        }
        return;
    }
    
    /* Handle tab */
    if (c == '\t') {
        do {
            terminal_putchar(' ');
        } while (terminal.column % 8 != 0);
        /* Cursor already updated by recursive putchar calls */
        return;
    }
    
    /* Insert mode: shift characters to the right before writing */
    size_t index = terminal.row * VGA_WIDTH + terminal.column;
    
    /* Shift all characters from end of row to current position, right to left */
    for (size_t i = (terminal.row * VGA_WIDTH + VGA_WIDTH - 1); i > index; i--) {
        terminal.buffer[i] = terminal.buffer[i - 1];
    }
    
    /* Write new character */
    terminal.buffer[index] = vga_entry((unsigned char)c, terminal.color);
    
    /* Advance cursor */
    if (++terminal.column == VGA_WIDTH) {
        terminal_newline();
    }
    
    terminal_update_cursor();
}

/**
 * terminal_write - Write a buffer of characters
 * @str: String to write
 * @len: Number of characters to write
 */
void terminal_write(const char* str, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        terminal_putchar(str[i]);
    }
}

/**
 * terminal_writestring - Write a null-terminated string
 * @str: The string to write
 */
void terminal_writestring(const char* str) {
    size_t i = 0;
    while (str[i] != '\0') {
        terminal_putchar(str[i]);
        i++;
    }
}

/**
 * terminal_writestring_color - Write a string with a specific color
 * @str: The string to write
 * @color: The color to use
 */
void terminal_writestring_color(const char* str, uint8_t color) {
    uint8_t old_color = terminal.color;
    terminal_setcolor(color);
    terminal_writestring(str);
    terminal_setcolor(old_color);
}

/**
 * terminal_setcursor - Set cursor position
 * @row: Row (0-24)
 * @column: Column (0-79)
 */
void terminal_setcursor(size_t row, size_t column) {
    if (row < VGA_HEIGHT && column < VGA_WIDTH) {
        terminal.row = row;
        terminal.column = column;
        
        // Update VGA hardware cursor
        uint16_t pos = row * VGA_WIDTH + column;
        outb(0x3D4, 0x0F);          // Cursor location low byte
        outb(0x3D5, (uint8_t)(pos & 0xFF));
        outb(0x3D4, 0x0E);          // Cursor location high byte
        outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    }
}

/**
 * terminal_getcursor - Get current cursor position
 * @row: Pointer to store row
 * @column: Pointer to store column
 */
void terminal_getcursor(size_t* row, size_t* column) {
    if (row) *row = terminal.row;
    if (column) *column = terminal.column;
}

/**
 * terminal_start_selection - Start text selection at given position
 * @row: Starting row
 * @col: Starting column
 */
void terminal_start_selection(size_t row, size_t col) {
    terminal.selection_active = 1;
    terminal.sel_start_row = row;
    terminal.sel_start_col = col;
    terminal.sel_end_row = row;
    terminal.sel_end_col = col;
}

/**
 * terminal_extend_selection - Extend selection to new position
 * @row: Ending row
 * @col: Ending column
 */
void terminal_extend_selection(size_t row, size_t col) {
    if (!terminal.selection_active) return;
    terminal.sel_end_row = row;
    terminal.sel_end_col = col;
    terminal_highlight_selection();
}

/**
 * terminal_clear_selection - Clear the current selection
 */
void terminal_clear_selection(void) {
    if (terminal.selection_active) {
        terminal_clear_selection_highlight();
        terminal.selection_active = 0;
    }
}

/**
 * terminal_has_selection - Check if selection is active
 * Returns: 1 if selection is active, 0 otherwise
 */
int terminal_has_selection(void) {
    return terminal.selection_active;
}

/**
 * terminal_highlight_selection - Highlight the selected text on screen
 */
void terminal_highlight_selection(void) {
    size_t start_col = terminal.sel_start_col;
    size_t end_col = terminal.sel_end_col;
    size_t row = terminal.row;
    
    if (end_col < start_col) {
        size_t tmp = start_col;
        start_col = end_col;
        end_col = tmp;
    }
    
    uint8_t sel_color = vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_CYAN);
    
    for (size_t col = start_col; col < end_col && col < VGA_WIDTH; col++) {
        size_t idx = row * VGA_WIDTH + col;
        uint8_t ch = (uint8_t)(terminal.buffer[idx] & 0xFF);
        terminal.buffer[idx] = vga_entry(ch, sel_color);
    }
}

/**
 * terminal_clear_selection_highlight - Remove highlighting from selection
 */
void terminal_clear_selection_highlight(void) {
    size_t row = terminal.row;
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        size_t idx = row * VGA_WIDTH + col;
        uint8_t ch = (uint8_t)(terminal.buffer[idx] & 0xFF);
        terminal.buffer[idx] = vga_entry(ch, terminal.color);
    }
}

/**
 * terminal_delete_selection - Delete the selected text and shift remaining
 */
void terminal_delete_selection(void) {
    if (!terminal.selection_active) return;
    
    size_t start_col = terminal.sel_start_col;
    size_t end_col = terminal.sel_end_col;
    size_t row = terminal.row;
    
    if (end_col < start_col) {
        size_t tmp = start_col;
        start_col = end_col;
        end_col = tmp;
    }
    
    size_t row_base = row * VGA_WIDTH;
    size_t del_count = end_col - start_col;
    size_t row_end = row_base + VGA_WIDTH;
    
    /* Shift text after selection to the left */
    for (size_t i = row_base + start_col; i < row_end - del_count; i++) {
        terminal.buffer[i] = terminal.buffer[i + del_count];
    }
    
    /* Clear empty spaces at end of row */
    for (size_t i = row_end - del_count; i < row_end; i++) {
        terminal.buffer[i] = vga_entry(' ', terminal.color);
    }
    
    terminal_clear_selection_highlight();
    terminal.column = start_col;
    terminal.selection_active = 0;
    terminal_update_cursor();
}

/**
 * terminal_delete_forward - Delete character at cursor position (forward delete)
 * Similar to Delete key - removes character under cursor, shifts remaining left
 */
void terminal_delete_forward(void) {
    size_t row_base = terminal.row * VGA_WIDTH;
    size_t cur_pos  = row_base + terminal.column;
    size_t row_end  = row_base + VGA_WIDTH - 1;
    
    /* Don't delete if at end of row */
    if (terminal.column >= VGA_WIDTH - 1) {
        terminal.buffer[cur_pos] = vga_entry(' ', terminal.color);
        terminal_update_cursor();
        return;
    }
    
    /* Shift all characters after cursor to the left */
    for (size_t i = cur_pos; i < row_end; i++) {
        terminal.buffer[i] = terminal.buffer[i + 1];
    }
    /* Clear last character in row */
    terminal.buffer[row_end] = vga_entry(' ', terminal.color);
    
    terminal_update_cursor();
}
