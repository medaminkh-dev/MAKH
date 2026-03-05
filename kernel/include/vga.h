/**
 * =============================================================================
 * vga.h - VGA Text Mode Driver Header for MakhOS
 * =============================================================================
 * Provides definitions for VGA text mode output
 *
 * Phase 9 Changes:
 *   - Added in_interrupt flag to terminal_state for reentrant printing
 * =============================================================================
 */

#ifndef VGA_H
#define VGA_H

#include "types.h"

/* VGA text buffer address */
#define VGA_BUFFER_ADDR     0xB8000
#define VGA_WIDTH           80
#define VGA_HEIGHT          25

/* VGA color codes */
enum vga_color {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN   = 14,
    VGA_COLOR_WHITE         = 15,
};

/**
 * Terminal state structure
 * 
 * PHASE 9 CHANGE: Added in_interrupt flag
 * This flag helps prevent reentrant terminal writes during interrupt handling
 */
struct terminal_state {
    uint16_t* buffer;
    size_t row;
    size_t column;
    uint8_t color;
    
    /**
     * PHASE 9 CHANGE (NEW):
     * Reentrant flag to prevent printing during interrupts
     * 
     * When set to 1, indicates terminal write is in progress.
     * Used to prevent nested terminal writes from timer interrupts.
     */
    volatile int in_interrupt;
    
    /* Selection state */
    int selection_active;
    size_t sel_start_row;
    size_t sel_start_col;
    size_t sel_end_row;
    size_t sel_end_col;
};

/* Function prototypes */
void terminal_initialize(void);
void terminal_initialize_noclear(void);
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_writestring(const char* str);
void terminal_write(const char* str, size_t len);
void terminal_writestring_color(const char* str, uint8_t color);
void terminal_newline(void);
void terminal_clear(void);
void terminal_enable_line_cursor(void);
void terminal_enable_block_cursor(void);

/* Cursor positioning */
void terminal_setcursor(size_t row, size_t column);
void terminal_getcursor(size_t* row, size_t* column);

/* Selection functions */
void terminal_start_selection(size_t row, size_t col);
void terminal_extend_selection(size_t row, size_t col);
void terminal_clear_selection(void);
void terminal_delete_selection(void);
void terminal_delete_forward(void);
int terminal_has_selection(void);
void terminal_highlight_selection(void);
void terminal_clear_selection_highlight(void);

/**
 * Helper to create VGA color byte
 * Combines foreground and background colors
 */
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

/**
 * Helper to create VGA entry
 * Combines character and color attribute
 */
static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | ((uint16_t)color << 8);
}

#endif /* VGA_H */
