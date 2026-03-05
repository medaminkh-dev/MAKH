/**
 * MakhOS - input_line.c
 * Line editing buffer implementation
 */

#include "include/input_line.h"
#include "include/vga.h"
#include "include/kernel.h"

// Clipboard for kill/yank operations
char input_clipboard[INPUT_CLIPBOARD_MAX];
int  input_clipboard_len = 0;

// ===== Line editing functions =====

void input_line_init(input_line_t* line) {
    for (int i = 0; i < INPUT_LINE_MAX; i++) {
        line->buffer[i] = '\0';
    }
    line->length = 0;
    line->cursor = 0;
    line->sel_start = -1;
    line->sel_end = -1;
}

void input_line_insert(input_line_t* line, char c) {
    // If there's a selection, delete it first
    if (line->sel_start != -1) {
        input_line_sel_delete(line);
    }
    
    // Check if buffer is full
    if (line->length >= INPUT_LINE_MAX - 1) {
        return;
    }
    
    // Shift characters to the right to make room
    for (int i = line->length; i > line->cursor; i--) {
        line->buffer[i] = line->buffer[i - 1];
    }
    
    // Insert character
    line->buffer[line->cursor] = c;
    line->cursor++;
    line->length++;
    line->buffer[line->length] = '\0';
}

void input_line_backspace(input_line_t* line) {
    // If there's a selection, delete it
    if (line->sel_start != -1) {
        input_line_sel_delete(line);
        return;
    }
    
    // Can't backspace at start
    if (line->cursor <= 0) {
        return;
    }
    
    // Shift characters left
    for (int i = line->cursor - 1; i < line->length - 1; i++) {
        line->buffer[i] = line->buffer[i + 1];
    }
    
    line->cursor--;
    line->length--;
    line->buffer[line->length] = '\0';
}

void input_line_delete(input_line_t* line) {
    // If there's a selection, delete it
    if (line->sel_start != -1) {
        input_line_sel_delete(line);
        return;
    }
    
    // Can't delete at end
    if (line->cursor >= line->length) {
        return;
    }
    
    // Shift characters left
    for (int i = line->cursor; i < line->length - 1; i++) {
        line->buffer[i] = line->buffer[i + 1];
    }
    
    line->length--;
    line->buffer[line->length] = '\0';
}

void input_line_move_left(input_line_t* line) {
    input_line_sel_clear(line);
    if (line->cursor > 0) {
        line->cursor--;
    }
}

void input_line_move_right(input_line_t* line) {
    input_line_sel_clear(line);
    if (line->cursor < line->length) {
        line->cursor++;
    }
}

void input_line_move_home(input_line_t* line) {
    input_line_sel_clear(line);
    line->cursor = 0;
}

void input_line_move_end(input_line_t* line) {
    input_line_sel_clear(line);
    line->cursor = line->length;
}

void input_line_move_word_left(input_line_t* line) {
    input_line_sel_clear(line);
    
    if (line->cursor == 0) {
        return;
    }
    
    int i = line->cursor - 1;
    
    // Skip whitespace
    while (i > 0 && line->buffer[i] == ' ') {
        i--;
    }
    
    // Skip word characters
    while (i > 0 && line->buffer[i - 1] != ' ') {
        i--;
    }
    
    line->cursor = i;
}

void input_line_move_word_right(input_line_t* line) {
    input_line_sel_clear(line);
    
    if (line->cursor >= line->length) {
        return;
    }
    
    int i = line->cursor;
    
    // Skip word characters
    while (i < line->length && line->buffer[i] != ' ') {
        i++;
    }
    
    // Skip whitespace
    while (i < line->length && line->buffer[i] == ' ') {
        i++;
    }
    
    line->cursor = i;
}

void input_line_clear(input_line_t* line) {
    input_line_init(line);
}

char* input_line_get(input_line_t* line) {
    return line->buffer;
}

// ===== Kill and Yank functions =====

void input_line_kill_to_end(input_line_t* line) {
    if (line->cursor >= line->length) {
        return;
    }
    
    int kill_len = line->length - line->cursor;
    
    // Save to clipboard
    input_clipboard_len = kill_len;
    if (input_clipboard_len > INPUT_CLIPBOARD_MAX - 1) {
        input_clipboard_len = INPUT_CLIPBOARD_MAX - 1;
    }
    
    for (int i = 0; i < input_clipboard_len; i++) {
        input_clipboard[i] = line->buffer[line->cursor + i];
    }
    input_clipboard[input_clipboard_len] = '\0';
    
    // Truncate line at cursor
    line->length = line->cursor;
    line->buffer[line->length] = '\0';
}

void input_line_kill_to_start(input_line_t* line) {
    if (line->cursor == 0) {
        return;
    }
    
    int kill_len = line->cursor;
    
    // Save to clipboard
    input_clipboard_len = kill_len;
    if (input_clipboard_len > INPUT_CLIPBOARD_MAX - 1) {
        input_clipboard_len = INPUT_CLIPBOARD_MAX - 1;
    }
    
    for (int i = 0; i < input_clipboard_len; i++) {
        input_clipboard[i] = line->buffer[i];
    }
    input_clipboard[input_clipboard_len] = '\0';
    
    // Shift remaining characters left
    for (int i = 0; i < line->length - kill_len; i++) {
        line->buffer[i] = line->buffer[i + kill_len];
    }
    
    line->length -= kill_len;
    line->cursor = 0;
    line->buffer[line->length] = '\0';
}

void input_line_kill_word_back(input_line_t* line) {
    if (line->cursor == 0) {
        return;
    }
    
    int end = line->cursor;
    int i = line->cursor - 1;
    
    // Skip whitespace
    while (i > 0 && line->buffer[i] == ' ') {
        i--;
    }
    
    // Find start of word
    while (i > 0 && line->buffer[i - 1] != ' ') {
        i--;
    }
    
    int kill_len = end - i;
    
    // Save to clipboard
    input_clipboard_len = kill_len;
    if (input_clipboard_len > INPUT_CLIPBOARD_MAX - 1) {
        input_clipboard_len = INPUT_CLIPBOARD_MAX - 1;
    }
    
    for (int j = 0; j < input_clipboard_len; j++) {
        input_clipboard[j] = line->buffer[i + j];
    }
    input_clipboard[input_clipboard_len] = '\0';
    
    // Shift remaining characters
    for (int j = i; j < line->length - kill_len; j++) {
        line->buffer[j] = line->buffer[j + kill_len];
    }
    
    line->length -= kill_len;
    line->cursor = i;
    line->buffer[line->length] = '\0';
}

void input_line_yank(input_line_t* line) {
    for (int i = 0; i < input_clipboard_len; i++) {
        input_line_insert(line, input_clipboard[i]);
    }
}

// ===== Selection functions =====

void input_line_sel_clear(input_line_t* line) {
    line->sel_start = -1;
    line->sel_end = -1;
}

void input_line_sel_delete(input_line_t* line) {
    if (line->sel_start == -1) {
        return;
    }
    
    int start = line->sel_start < line->sel_end ? line->sel_start : line->sel_end;
    int end = line->sel_start < line->sel_end ? line->sel_end : line->sel_start;
    
    // Save to clipboard
    int kill_len = end - start;
    input_clipboard_len = kill_len;
    if (input_clipboard_len > INPUT_CLIPBOARD_MAX - 1) {
        input_clipboard_len = INPUT_CLIPBOARD_MAX - 1;
    }
    
    for (int i = 0; i < input_clipboard_len; i++) {
        input_clipboard[i] = line->buffer[start + i];
    }
    input_clipboard[input_clipboard_len] = '\0';
    
    // Shift remaining characters
    for (int i = start; i < line->length - kill_len; i++) {
        line->buffer[i] = line->buffer[i + kill_len];
    }
    
    line->length -= kill_len;
    line->cursor = start;
    line->buffer[line->length] = '\0';
    input_line_sel_clear(line);
}

void input_line_sel_extend_left(input_line_t* line) {
    if (line->sel_start == -1) {
        line->sel_start = line->cursor;
        line->sel_end = line->cursor;
    }
    
    if (line->cursor > 0) {
        line->cursor--;
        line->sel_end = line->cursor;
    }
}

void input_line_sel_extend_right(input_line_t* line) {
    if (line->sel_start == -1) {
        line->sel_start = line->cursor;
        line->sel_end = line->cursor;
    }
    
    if (line->cursor < line->length) {
        line->cursor++;
        line->sel_end = line->cursor;
    }
}

int input_line_has_selection(input_line_t* line) {
    return line->sel_start != -1;
}

// ===== History functions =====

void input_history_init(input_history_t* hist) {
    hist->count = 0;
    hist->current = 0;
    
    for (int i = 0; i < INPUT_HISTORY_MAX; i++) {
        for (int j = 0; j < INPUT_LINE_MAX; j++) {
            hist->entries[i][j] = '\0';
        }
    }
}

void input_history_push(input_history_t* hist, const char* line) {
    if (hist->count >= INPUT_HISTORY_MAX) {
        // Shift all entries down to make room
        for (int i = 0; i < INPUT_HISTORY_MAX - 1; i++) {
            for (int j = 0; j < INPUT_LINE_MAX; j++) {
                hist->entries[i][j] = hist->entries[i + 1][j];
            }
        }
        hist->count = INPUT_HISTORY_MAX - 1;
    }
    
    // Copy line to history
    int i = 0;
    while (line[i] && i < INPUT_LINE_MAX - 1) {
        hist->entries[hist->count][i] = line[i];
        i++;
    }
    hist->entries[hist->count][i] = '\0';
    hist->count++;
    hist->current = hist->count;
}

char* input_history_prev(input_history_t* hist) {
    if (hist->current > 0) {
        hist->current--;
        return hist->entries[hist->current];
    }
    return NULL;
}

char* input_history_next(input_history_t* hist) {
    if (hist->current < hist->count - 1) {
        hist->current++;
        return hist->entries[hist->current];
    }
    hist->current = hist->count;
    return NULL;
}

void input_history_load(input_line_t* line, const char* entry) {
    input_line_init(line);
    
    int i = 0;
    while (entry[i] && i < INPUT_LINE_MAX - 1) {
        line->buffer[i] = entry[i];
        i++;
    }
    
    line->length = i;
    line->cursor = i;
    line->buffer[i] = '\0';
}

// ===== Rendering function =====

void input_line_render(input_line_t* line, const char* prompt, size_t screen_row) {
    uint8_t normal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    uint8_t select_color = vga_entry_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_CYAN);
    
    size_t prompt_len = 0;
    while (prompt[prompt_len]) {
        prompt_len++;
    }
    
    // Calculate selection range
    int sel_lo = -1, sel_hi = -1;
    if (line->sel_start != -1) {
        sel_lo = line->sel_start < line->sel_end ? line->sel_start : line->sel_end;
        sel_hi = line->sel_start < line->sel_end ? line->sel_end : line->sel_start;
    }
    
    // Clear the line area
    uint16_t* vga_buf = (uint16_t*)0xB8000;
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        vga_buf[screen_row * VGA_WIDTH + col] = vga_entry(' ', normal_color);
    }
    
    // Render prompt
    for (size_t i = 0; i < prompt_len && i < VGA_WIDTH; i++) {
        vga_buf[screen_row * VGA_WIDTH + i] = vga_entry(prompt[i], normal_color);
    }
    
    // Render line content
    for (int i = 0; i < line->length && (prompt_len + i) < VGA_WIDTH; i++) {
        uint8_t color = (sel_lo != -1 && i >= sel_lo && i < sel_hi) ? select_color : normal_color;
        vga_buf[screen_row * VGA_WIDTH + prompt_len + i] = vga_entry(line->buffer[i], color);
    }
    
    // Set cursor position
    terminal_setcursor(screen_row, prompt_len + line->cursor);
}
