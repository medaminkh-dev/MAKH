/**
 * MakhOS - input_line.h
 * Line editing buffer with history support
 * Similar to GNU Readline but simplified for kernel use
 */

#ifndef MAKHOS_INPUT_LINE_H
#define MAKHOS_INPUT_LINE_H

#include <types.h>

#define INPUT_LINE_MAX      256   // Maximum line length
#define INPUT_HISTORY_MAX   50    // Maximum history entries
#define INPUT_CLIPBOARD_MAX 256   // Maximum clipboard size

/**
 * Input line state structure
 */
typedef struct {
    char    buffer[INPUT_LINE_MAX];     // Line buffer
    int     length;                     // Current line length
    int     cursor;                     // Cursor position in buffer
    int     sel_start;                  // Selection start (-1 = no selection)
    int     sel_end;                    // Selection end
} input_line_t;

/**
 * Command history structure
 */
typedef struct {
    char    entries[INPUT_HISTORY_MAX][INPUT_LINE_MAX];  // History entries
    int     count;                      // Number of entries
    int     current;                    // Current position in history
} input_history_t;

// Clipboard for kill/yank operations
extern char input_clipboard[INPUT_CLIPBOARD_MAX];
extern int  input_clipboard_len;

// ===== Line editing functions =====

/**
 * Initialize input line
 */
void input_line_init(input_line_t* line);

/**
 * Insert character at cursor position
 */
void input_line_insert(input_line_t* line, char c);

/**
 * Backspace - delete character before cursor
 */
void input_line_backspace(input_line_t* line);

/**
 * Delete - delete character at cursor (forward delete)
 */
void input_line_delete(input_line_t* line);

/**
 * Move cursor left
 */
void input_line_move_left(input_line_t* line);

/**
 * Move cursor right
 */
void input_line_move_right(input_line_t* line);

/**
 * Move cursor to start of line (Home)
 */
void input_line_move_home(input_line_t* line);

/**
 * Move cursor to end of line (End)
 */
void input_line_move_end(input_line_t* line);

/**
 * Move cursor to previous word (Ctrl+Left)
 */
void input_line_move_word_left(input_line_t* line);

/**
 * Move cursor to next word (Ctrl+Right)
 */
void input_line_move_word_right(input_line_t* line);

/**
 * Clear the entire line
 */
void input_line_clear(input_line_t* line);

/**
 * Get the line content as string
 */
char* input_line_get(input_line_t* line);

// ===== Kill and Yank functions =====

/**
 * Kill (cut) from cursor to end of line (Ctrl+K)
 */
void input_line_kill_to_end(input_line_t* line);

/**
 * Kill (cut) from start to cursor (Ctrl+U)
 */
void input_line_kill_to_start(input_line_t* line);

/**
 * Kill (cut) word before cursor (Ctrl+W)
 */
void input_line_kill_word_back(input_line_t* line);

/**
 * Yank (paste) clipboard at cursor position (Ctrl+Y)
 */
void input_line_yank(input_line_t* line);

// ===== Selection functions =====

/**
 * Clear selection
 */
void input_line_sel_clear(input_line_t* line);

/**
 * Delete selected text
 */
void input_line_sel_delete(input_line_t* line);

/**
 * Extend selection left (Shift+Left)
 */
void input_line_sel_extend_left(input_line_t* line);

/**
 * Extend selection right (Shift+Right)
 */
void input_line_sel_extend_right(input_line_t* line);

/**
 * Check if there's an active selection
 */
int input_line_has_selection(input_line_t* line);

// ===== History functions =====

/**
 * Initialize history
 */
void input_history_init(input_history_t* hist);

/**
 * Add line to history
 */
void input_history_push(input_history_t* hist, const char* line);

/**
 * Get previous history entry (Up arrow)
 * Returns NULL if no previous entry
 */
char* input_history_prev(input_history_t* hist);

/**
 * Get next history entry (Down arrow)
 * Returns NULL if no next entry
 */
char* input_history_next(input_history_t* hist);

/**
 * Load a history entry into input line
 */
void input_history_load(input_line_t* line, const char* entry);

// ===== Rendering function =====

/**
 * Render input line to VGA screen
 * @param line: Input line to render
 * @param prompt: Prompt string (e.g., "MakhOS> ")
 * @param screen_row: Row to render on
 */
void input_line_render(input_line_t* line, const char* prompt, size_t screen_row);

#endif // MAKHOS_INPUT_LINE_H
