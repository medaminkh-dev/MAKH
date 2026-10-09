/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fb.c
 * Framebuffer graphics console: antialiased 8x16 text over a linear-RGB
 * framebuffer, with a warm 16-colour palette. See drivers/fb.h for the role
 * this plays alongside the legacy VGA-text console.
 */

#include <drivers/fb.h>
#include <mm/vmm.h>
#include <lib/string.h>
#include <klog.h>
#include <fb_font.h>
#include <fb_logo.h>

/* A small gutter around the text area so glyphs don't hug the bezel. */
#define FB_MARGIN_X 8
#define FB_MARGIN_Y 8

/*
 * Warm, mellow 16-colour palette (0xRRGGBB), indexed by the VGA colour code.
 * It keeps the same 16 slots the text console already colour-codes with, so
 * existing output just looks richer: a soft charcoal background instead of hard
 * black, cream text instead of grey-white, and gentler accents throughout. This
 * is MAKH's own look — not a clone of any proprietary terminal theme.
 */
static const uint32_t vga_rgb[16] = {
    0x1b1816,  /*  0 black        -> warm charcoal (default background) */
    0x5577d4,  /*  1 blue         */
    0x7fb069,  /*  2 green        */
    0x3fb5ac,  /*  3 cyan         */
    0xd6625a,  /*  4 red          */
    0xb06cc6,  /*  5 magenta      */
    0xcc9357,  /*  6 brown/amber  */
    0xd8d0c2,  /*  7 light grey   -> cream (default foreground) */
    0x6b6358,  /*  8 dark grey    */
    0x89a9ff,  /*  9 light blue   */
    0xa6e085,  /* 10 light green  */
    0x86e0d8,  /* 11 light cyan   */
    0xff8a80,  /* 12 light red    */
    0xe3a7ff,  /* 13 light magenta*/
    0xf2d06b,  /* 14 yellow       */
    0xfdf8f0,  /* 15 white        -> warm white */
};

static struct {
    volatile uint8_t* base;     /* mapped framebuffer virtual base */
    uint32_t width, height;     /* pixels */
    uint32_t pitch;             /* bytes per scanline */
    uint32_t bytespp;           /* bytes per pixel (3 or 4) */
    uint8_t  r_pos, g_pos, b_pos;
    uint8_t  r_sz,  g_sz,  b_sz;
    uint32_t cols, rows;        /* text geometry, in character cells */
    uint32_t cx, cy;            /* cursor cell position */
    uint32_t pal_native[16];    /* palette pre-packed into native pixels */
    int active;                 /* a usable framebuffer was mapped */
    int enabled;                /* text console should render to it */
} fb;

uint32_t fb_width(void)  { return fb.width;  }
uint32_t fb_height(void) { return fb.height; }
uint32_t fb_cols(void)   { return fb.cols;   }
uint32_t fb_rows(void)   { return fb.rows;   }
int      fb_active(void) { return fb.active && fb.enabled; }

int fb_console_set_enabled(int on) {
    int prev = fb.enabled;
    fb.enabled = on ? 1 : 0;
    return prev;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    /* Scale each channel to its field width, then shift into position. With the
     * usual 8-bit channels the scale is a no-op. */
    uint32_t rv = (fb.r_sz >= 8) ? r : (uint32_t)(r >> (8 - fb.r_sz));
    uint32_t gv = (fb.g_sz >= 8) ? g : (uint32_t)(g >> (8 - fb.g_sz));
    uint32_t bv = (fb.b_sz >= 8) ? b : (uint32_t)(b >> (8 - fb.b_sz));
    return (rv << fb.r_pos) | (gv << fb.g_pos) | (bv << fb.b_pos);
}

/* Unchecked pixel store (callers clamp). */
static inline void putpx(uint32_t x, uint32_t y, uint32_t native) {
    uint8_t* p = (uint8_t*)fb.base + (size_t)y * fb.pitch + (size_t)x * fb.bytespp;
    if (fb.bytespp == 4) {
        *(volatile uint32_t*)p = native;
    } else { /* 3 bytes, little-endian of the packed value */
        p[0] = (uint8_t)native;
        p[1] = (uint8_t)(native >> 8);
        p[2] = (uint8_t)(native >> 16);
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t native) {
    if (!fb.active || x >= fb.width || y >= fb.height) return;
    putpx(x, y, native);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t native) {
    if (!fb.active) return;
    if (x >= fb.width || y >= fb.height) return;
    if (x + w > fb.width)  w = fb.width  - x;
    if (y + h > fb.height) h = fb.height - y;
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            putpx(x + i, y + j, native);
}

void fb_clear(uint32_t native) {
    fb_fill_rect(0, 0, fb.width, fb.height, native);
}

/* Blend one glyph at pixel origin (px0,py0). fg/bg are 0xRRGGBB (not native). */
static void blit_glyph(uint32_t px0, uint32_t py0, unsigned char ch,
                       uint32_t fg, uint32_t bg) {
    int fr = (fg >> 16) & 0xFF, fg_ = (fg >> 8) & 0xFF, fb_ = fg & 0xFF;
    int br = (bg >> 16) & 0xFF, bg_ = (bg >> 8) & 0xFF, bb_ = bg & 0xFF;
    uint32_t bg_native = fb_rgb((uint8_t)br, (uint8_t)bg_, (uint8_t)bb_);

    const unsigned char* g =
        (ch >= FB_FONT_FIRST && ch < FB_FONT_FIRST + FB_FONT_COUNT)
            ? fb_font[ch - FB_FONT_FIRST] : NULL;

    for (uint32_t yy = 0; yy < FB_FONT_H; yy++) {
        for (uint32_t xx = 0; xx < FB_FONT_W; xx++) {
            uint32_t native;
            unsigned a = g ? g[yy * FB_FONT_W + xx] : 0;
            if (a == 0) {
                native = bg_native;
            } else {
                a += a >> 7;  /* 0..255 -> 0..256 so full coverage reaches fg */
                int r = br + (((fr - br) * (int)a) >> 8);
                int gg = bg_ + (((fg_ - bg_) * (int)a) >> 8);
                int bl = bb_ + (((fb_ - bb_) * (int)a) >> 8);
                native = fb_rgb((uint8_t)r, (uint8_t)gg, (uint8_t)bl);
            }
            putpx(px0 + xx, py0 + yy, native);
        }
    }
}

/* Blend one glyph into text cell (col,row). */
static void draw_glyph(uint32_t col, uint32_t row, unsigned char ch,
                       uint32_t fg, uint32_t bg) {
    blit_glyph(FB_MARGIN_X + col * FB_FONT_W, FB_MARGIN_Y + row * FB_FONT_H,
               ch, fg, bg);
}

/* Scroll the text area up one row and clear the freed line to the background.
 * The copy is destination-ahead-of-source, so a simple forward word copy is
 * safe; 32-bit units cut the uncached framebuffer traffic 4x versus a byte
 * memmove. */
static void fb_scroll(uint32_t bg_native) {
    uint32_t top = FB_MARGIN_Y;
    size_t line_bytes = (size_t)FB_FONT_H * fb.pitch;
    uint8_t* dstb = (uint8_t*)fb.base + (size_t)top * fb.pitch;
    uint8_t* srcb = dstb + line_bytes;
    size_t bytes = (size_t)(fb.rows - 1) * line_bytes;
    if ((bytes & 3) == 0 &&
        (((uintptr_t)dstb | (uintptr_t)srcb) & 3) == 0) {
        volatile uint32_t* dst = (volatile uint32_t*)dstb;
        volatile uint32_t* src = (volatile uint32_t*)srcb;
        for (size_t i = 0; i < bytes / 4; i++)
            dst[i] = src[i];
    } else {
        memmove(dstb, srcb, bytes);     /* 24bpp / odd pitch fallback */
    }
    fb_fill_rect(0, top + (fb.rows - 1) * FB_FONT_H, fb.width, FB_FONT_H, bg_native);
}

void fb_console_newline(void) {
    fb.cx = 0;
    if (++fb.cy >= fb.rows) {
        fb_scroll(fb.pal_native[0]);
        fb.cy = fb.rows - 1;
    }
}

void fb_console_clear(uint8_t vga_color) {
    uint8_t bg = (vga_color >> 4) & 0x0F;
    fb_clear(fb.pal_native[bg]);
    fb.cx = fb.cy = 0;
}

void fb_console_get_cursor(uint32_t* col, uint32_t* row) {
    if (col) *col = fb.cx;
    if (row) *row = fb.cy;
}

void fb_console_set_cursor(uint32_t col, uint32_t row) {
    if (fb.cols && col >= fb.cols) col = fb.cols - 1;
    if (fb.rows && row >= fb.rows) row = fb.rows - 1;
    fb.cx = col;
    fb.cy = row;
}

void fb_console_draw_cell(uint32_t col, uint32_t row, char c, uint8_t vga_color) {
    if (!fb.active || col >= fb.cols || row >= fb.rows) return;
    draw_glyph(col, row, (unsigned char)c,
               vga_rgb[vga_color & 0x0F], vga_rgb[(vga_color >> 4) & 0x0F]);
}

void fb_console_clear_row(uint32_t row, uint8_t vga_color) {
    if (!fb.active || row >= fb.rows) return;
    uint8_t bg = (vga_color >> 4) & 0x0F;
    fb_fill_rect(0, FB_MARGIN_Y + row * FB_FONT_H, fb.width, FB_FONT_H,
                 fb.pal_native[bg]);
}

/* Erase from the cursor to the end of its row (ANSI ESC[K). */
void fb_console_clear_to_eol(uint8_t vga_color) {
    if (!fb.active) return;
    uint8_t bg = (vga_color >> 4) & 0x0F;
    uint32_t x = FB_MARGIN_X + fb.cx * FB_FONT_W;
    uint32_t y = FB_MARGIN_Y + fb.cy * FB_FONT_H;
    if (x < fb.width)
        fb_fill_rect(x, y, fb.width - x, FB_FONT_H, fb.pal_native[bg]);
}

void fb_console_draw_cursor(uint32_t col, uint32_t row, uint8_t vga_fg) {
    if (!fb.active || col >= fb.cols || row >= fb.rows) return;
    uint32_t x = FB_MARGIN_X + col * FB_FONT_W;
    uint32_t y = FB_MARGIN_Y + row * FB_FONT_H;
    fb_fill_rect(x, y + FB_FONT_H - 2, FB_FONT_W, 2, fb.pal_native[vga_fg & 0x0F]);
}

void fb_console_putchar(char c, uint8_t vga_color) {
    if (!fb.active) return;
    uint32_t fg = vga_rgb[vga_color & 0x0F];
    uint32_t bg = vga_rgb[(vga_color >> 4) & 0x0F];

    switch (c) {
    case '\n':
        fb_console_newline();
        return;
    case '\r':
        fb.cx = 0;
        return;
    case '\b':
        if (fb.cx > 0) {
            fb.cx--;
            draw_glyph(fb.cx, fb.cy, ' ', fg, bg);  /* erase char to the left */
        }
        return;
    case '\t':
        do {
            draw_glyph(fb.cx, fb.cy, ' ', fg, bg);
            if (++fb.cx >= fb.cols) { fb_console_newline(); break; }
        } while (fb.cx % 8 != 0);
        return;
    default:
        break;
    }

    if ((unsigned char)c < FB_FONT_FIRST)   /* unhandled control char */
        return;

    draw_glyph(fb.cx, fb.cy, (unsigned char)c, fg, bg);
    if (++fb.cx >= fb.cols)
        fb_console_newline();
}

/* ---------------------------------------------------------------------------
 * Boot splash (FB-2): the fennec logo over the dark field, a mellow spinning
 * loader, and a caption — shown while the kernel finishes bringing itself up,
 * then wiped so the terminal starts on a clean screen. All integer math (no
 * FPU/SSE in the kernel); the spinner's dot directions are a fixed /1000 table.
 * ------------------------------------------------------------------------- */

/* cos,sin * 1000 at 30-degree steps (screen y grows downward -> clockwise). */
static const int spin_dx[12] = { 1000, 866, 500, 0, -500, -866,
                                 -1000, -866, -500, 0, 500, 866 };
static const int spin_dy[12] = { 0, 500, 866, 1000, 866, 500,
                                 0, -500, -866, -1000, -866, -500 };

/* Blend (r,g,b) over the charcoal background at coverage a (0..255). */
static uint32_t cream_over_bg(int r, int g, int b, unsigned a) {
    int br = 0x1b, bg = 0x18, bb = 0x16;
    a += a >> 7;
    int rr = br + (((r - br) * (int)a) >> 8);
    int gg = bg + (((g - bg) * (int)a) >> 8);
    int bb2 = bb + (((b - bb) * (int)a) >> 8);
    return fb_rgb((uint8_t)rr, (uint8_t)gg, (uint8_t)bb2);
}

static void fill_disc(int cx, int cy, int r, uint32_t native) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                fb_put_pixel((uint32_t)(cx + dx), (uint32_t)(cy + dy), native);
}

/* Centre of the spinner, just below the logo. */
static void splash_spinner_center(int* cx, int* cy) {
    *cx = (int)fb.width / 2;
    *cy = (int)fb.height / 2 + FB_LOGO_H / 2 + 34;
}

void fb_splash_tick(int frame) {
    if (!fb.active) return;
    int cx, cy;
    splash_spinner_center(&cx, &cy);
    int R = 26;
    fb_fill_rect((uint32_t)(cx - R - 6), (uint32_t)(cy - R - 6),
                 (uint32_t)((R + 6) * 2), (uint32_t)((R + 6) * 2),
                 fb.pal_native[0]);
    int head = ((frame % 12) + 12) % 12;
    for (int i = 0; i < 12; i++) {
        int k = (head - i + 12) % 12;
        int level = 255 - k * 20;
        if (level < 36) level = 36;
        int x = cx + spin_dx[i] * R / 1000;
        int y = cy + spin_dy[i] * R / 1000;
        fill_disc(x, y, 3, cream_over_bg(0xfd, 0xf8, 0xf0, (unsigned)level));
    }
}

/* Clear to the dark field and paint the logo + caption + first spinner frame. */
void fb_splash_show(void) {
    if (!fb.active) return;
    fb_clear(fb.pal_native[0]);

    int lx = ((int)fb.width - FB_LOGO_W) / 2;
    int ly = (int)fb.height / 2 - FB_LOGO_H / 2 - 36;
    for (int y = 0; y < FB_LOGO_H; y++) {
        for (int x = 0; x < FB_LOGO_W; x++) {
            unsigned a = fb_logo[y * FB_LOGO_W + x];
            if (!a) continue;
            fb_put_pixel((uint32_t)(lx + x), (uint32_t)(ly + y),
                         cream_over_bg(0xfd, 0xf8, 0xf0, a));
        }
    }

    /* Dim caption, centred below the spinner. */
    static const char cap[] = "starting MAKH";
    int n = (int)(sizeof(cap) - 1);
    int tx = ((int)fb.width - n * FB_FONT_W) / 2;
    int cx, cy;
    splash_spinner_center(&cx, &cy);
    int ty = cy + 34;
    for (int i = 0; i < n; i++)
        blit_glyph((uint32_t)(tx + i * FB_FONT_W), (uint32_t)ty,
                   (unsigned char)cap[i], 0x8a8378, 0x1b1816);

    fb_splash_tick(0);
}

int fb_init(const struct multiboot_tag_framebuffer* tag) {
    if (!tag)
        return 0;
    if (tag->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
        KLOG_I("FB", "framebuffer type %u is not direct RGB; using VGA text\n",
               (unsigned)tag->framebuffer_type);
        return 0;
    }
    uint32_t bpp = tag->framebuffer_bpp;
    if (bpp != 32 && bpp != 24) {
        KLOG_I("FB", "unsupported %u bpp; using VGA text\n", (unsigned)bpp);
        return 0;
    }
    uint64_t phys  = tag->framebuffer_addr;
    uint32_t pitch = tag->framebuffer_pitch;
    uint32_t w = tag->framebuffer_width, h = tag->framebuffer_height;
    if (!phys || !pitch || !w || !h)
        return 0;

    uint64_t size = (uint64_t)pitch * h;
    uint64_t va = vmm_map_mmio(phys, size);
    if (!va) {
        KLOG_W("FB", "could not map %lu-byte framebuffer at 0x%lx\n",
               (unsigned long)size, (unsigned long)phys);
        return 0;
    }

    fb.base    = (volatile uint8_t*)(uintptr_t)va;
    fb.width   = w;
    fb.height  = h;
    fb.pitch   = pitch;
    fb.bytespp = bpp / 8;
    fb.r_pos = tag->fb_red_field_position;   fb.r_sz = tag->fb_red_mask_size;
    fb.g_pos = tag->fb_green_field_position; fb.g_sz = tag->fb_green_mask_size;
    fb.b_pos = tag->fb_blue_field_position;  fb.b_sz = tag->fb_blue_mask_size;
    /* Guard against a bootloader reporting zero-width masks. */
    if (!fb.r_sz) { fb.r_pos = 16; fb.r_sz = 8; }
    if (!fb.g_sz) { fb.g_pos = 8;  fb.g_sz = 8; }
    if (!fb.b_sz) { fb.b_pos = 0;  fb.b_sz = 8; }

    for (int i = 0; i < 16; i++)
        fb.pal_native[i] = fb_rgb((uint8_t)(vga_rgb[i] >> 16),
                                  (uint8_t)(vga_rgb[i] >> 8),
                                  (uint8_t)(vga_rgb[i]));

    fb.cols = (w - 2 * FB_MARGIN_X) / FB_FONT_W;
    fb.rows = (h - 2 * FB_MARGIN_Y) / FB_FONT_H;
    if (!fb.cols || !fb.rows)
        return 0;

    fb.cx = fb.cy = 0;
    fb.active = 1;
    fb.enabled = 1;
    fb_clear(fb.pal_native[0]);

    KLOG_I("FB", "%ux%u %ubpp, %ux%u text cells\n",
           (unsigned)w, (unsigned)h, (unsigned)bpp,
           (unsigned)fb.cols, (unsigned)fb.rows);
    return 1;
}
