/**
 * MakhOS - vga.c
 * VGA text mode driver implementation
 */

#include "include/vga.h"

/* Static terminal state */
static struct terminal_state terminal;

/**
 * terminal_initialize - Initialize the VGA terminal
 * Sets up the terminal at position (0,0) with light grey on black
 */
void terminal_initialize(void) {
    terminal.buffer = (uint16_t*)VGA_BUFFER_ADDR;
    terminal.row = 0;
    terminal.column = 0;
    terminal.color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    
    /* Clear the screen */
    terminal_clear();
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
 * Supports '\n' for newline
 */
void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_newline();
        return;
    }
    
    /* Handle carriage return */
    if (c == '\r') {
        terminal.column = 0;
        return;
    }
    
    /* Handle tab */
    if (c == '\t') {
        do {
            terminal_putchar(' ');
        } while (terminal.column % 8 != 0);
        return;
    }
    
    /* Write character to buffer */
    size_t index = terminal.row * VGA_WIDTH + terminal.column;
    terminal.buffer[index] = vga_entry((unsigned char)c, terminal.color);
    
    /* Advance cursor */
    if (++terminal.column == VGA_WIDTH) {
        terminal_newline();
    }
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
