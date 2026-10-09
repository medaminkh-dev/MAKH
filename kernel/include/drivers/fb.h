/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKH_DRIVERS_FB_H
#define MAKH_DRIVERS_FB_H

#include <types.h>
#include <multiboot.h>

/*
 * =============================================================================
 * fb.h - Framebuffer graphics console
 * =============================================================================
 * When GRUB hands us a linear-RGB framebuffer (requested through the multiboot2
 * header in boot/boot.asm), fb_init() maps it into the higher-half MMIO window
 * and turns MAKH's text console into a pixel-rendered one: an antialiased 8x16
 * font over a warm, mellow 16-colour palette. The classic VGA-text console
 * (vga.c) delegates to the functions here whenever fb_active() is true; when no
 * usable framebuffer is present it falls back to 80x25 text mode. Either way the
 * serial mirror is untouched, so the headless self-tests (which assert on serial,
 * never on the screen) behave identically with or without a framebuffer.
 * =============================================================================
 */

/* Map and initialise from the multiboot2 framebuffer tag (may be NULL). Returns
 * 1 if a usable linear-RGB framebuffer was brought up, 0 otherwise. */
int  fb_init(const struct multiboot_tag_framebuffer* tag);

/* Is the pixel console live AND selected for text output? vga.c checks this to
 * decide whether to render glyphs or write the legacy 0xB8000 text buffer. */
int  fb_active(void);

/* Select (1) or deselect (0) the pixel console for text output without tearing
 * down the mapping. The headless test path deselects it so heavy log output
 * never pays the per-glyph / per-scroll cost. Returns the previous state. */
int  fb_console_set_enabled(int on);

/* --- text console, driven by vga.c's terminal_* layer --- */
/* Emit one character at the console cursor, honouring \n \r \b \t. `vga_color`
 * is the VGA attribute byte (fg | bg<<4); the palette maps each nibble to RGB. */
void fb_console_putchar(char c, uint8_t vga_color);
void fb_console_newline(void);
void fb_console_clear(uint8_t vga_color);

/* Console geometry in character cells (meaningful once fb_active()). */
uint32_t fb_cols(void);
uint32_t fb_rows(void);

/* Cursor cell position (shared coordinate system with the VGA text console: when
 * fb_active(), terminal_get/setcursor map straight onto these). */
void fb_console_get_cursor(uint32_t* col, uint32_t* row);
void fb_console_set_cursor(uint32_t col, uint32_t row);

/* Random-access rendering for a single-line editor (input_line.c): paint one
 * cell at an absolute position, clear a whole text row, or draw an underline
 * cursor — all without disturbing the streaming cursor. `vga_color` is a VGA
 * attribute byte. */
void fb_console_draw_cell(uint32_t col, uint32_t row, char c, uint8_t vga_color);
void fb_console_clear_row(uint32_t row, uint8_t vga_color);
void fb_console_draw_cursor(uint32_t col, uint32_t row, uint8_t vga_fg);

/* --- raw drawing primitives (also used by the boot splash, FB-2) --- */
/* Pack an (r,g,b) triple into the framebuffer's native pixel format. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);
void     fb_put_pixel(uint32_t x, uint32_t y, uint32_t native_pixel);
void     fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t native_pixel);
void     fb_clear(uint32_t native_pixel);
uint32_t fb_width(void);
uint32_t fb_height(void);

#endif /* MAKH_DRIVERS_FB_H */
