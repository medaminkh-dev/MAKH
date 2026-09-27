/**
 * MakhOS - klog.c
 * Kernel formatted logging (printf-style) for VGA + serial output.
 */

#include <klog.h>
#include <vga.h>

/* Freestanding varargs without <stdarg.h> (blocked by -nostdinc). */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

static klog_level_t klog_level = KLOG_INFO;

void klog_set_level(klog_level_t level) { klog_level = level; }
klog_level_t klog_get_level(void) { return klog_level; }

/* -------------------------------------------------------------------------- */
/* Number formatting helpers                                                  */
/* -------------------------------------------------------------------------- */

static void emit_char(char c) {
    char buf[2] = { c, '\0' };
    terminal_writestring(buf);
}

static void emit_str(const char* s) {
    if (!s) s = "(null)";
    terminal_writestring(s);
}

/* Format an unsigned value in the given base into buf (reversed then fixed). */
static int format_uint(uint64_t value, unsigned base, int upper, char* buf) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int i = 0;

    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value > 0 && i < (int)sizeof(tmp)) {
            tmp[i++] = digits[value % base];
            value /= base;
        }
    }

    int n = 0;
    while (i > 0) buf[n++] = tmp[--i];
    buf[n] = '\0';
    return n;
}

/* Emit a string left-justified in a field of `width`. */
static void emit_left(const char* str, int width) {
    int len = 0;
    while (str[len]) len++;
    emit_str(str);
    for (int i = len; i < width; i++) emit_char(' ');
}

/* Emit a numeric string with optional width and zero/space padding. */
static void emit_padded_impl(const char* num, int negative, int width, int zero_pad) {
    int len = 0;
    while (num[len]) len++;
    int total = len + (negative ? 1 : 0);

    if (zero_pad) {
        if (negative) emit_char('-');
        for (int i = total; i < width; i++) emit_char('0');
        emit_str(num);
    } else {
        for (int i = total; i < width; i++) emit_char(' ');
        if (negative) emit_char('-');
        emit_str(num);
    }
}

static int g_left;   /* set per-conversion by kvprintf */

static void emit_padded(const char* num, int negative, int width, int zero_pad) {
    if (g_left) {
        char tmp[40];
        int i = 0;
        if (negative) tmp[i++] = '-';
        for (int k = 0; num[k] && i < 39; k++) tmp[i++] = num[k];
        tmp[i] = '\0';
        emit_left(tmp, width);
        return;
    }
    emit_padded_impl(num, negative, width, zero_pad);
}

/* -------------------------------------------------------------------------- */
/* Core formatter                                                             */
/* -------------------------------------------------------------------------- */

void kvprintf(const char* fmt, va_list args) {
    char numbuf[32];

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            emit_char(*p);
            continue;
        }

        p++;  /* skip '%' */

        /* Flags: '0' (zero pad) and '-' (left justify). */
        int zero_pad = 0, left = 0;
        while (*p == '0' || *p == '-') {
            if (*p == '0') zero_pad = 1; else left = 1;
            p++;
        }
        if (left) zero_pad = 0;
        g_left = left;

        /* Field width (decimal). */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* Length modifiers. */
        int longness = 0;  /* 0=int, 1=long, 2=long long */
        while (*p == 'l') { longness++; p++; }
        if (*p == 'z') { longness = 2; p++; }  /* size_t */

        switch (*p) {
            case 's': {
                const char* str = va_arg(args, const char*);
                if (!str) str = "(null)";
                if (left) {
                    emit_left(str, width);
                } else {
                    int len = 0;
                    while (str[len]) len++;
                    for (int i = len; i < width; i++) emit_char(' ');
                    emit_str(str);
                }
                break;
            }
            case 'c':
                emit_char((char)va_arg(args, int));
                break;
            case 'd':
            case 'i': {
                int64_t v = (longness >= 2) ? va_arg(args, int64_t)
                          : (longness == 1) ? va_arg(args, long)
                                            : va_arg(args, int);
                int neg = v < 0;
                uint64_t mag = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
                format_uint(mag, 10, 0, numbuf);
                emit_padded(numbuf, neg, width, zero_pad);
                break;
            }
            case 'u': {
                uint64_t v = (longness >= 2) ? va_arg(args, uint64_t)
                           : (longness == 1) ? va_arg(args, unsigned long)
                                             : va_arg(args, unsigned int);
                format_uint(v, 10, 0, numbuf);
                emit_padded(numbuf, 0, width, zero_pad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = (longness >= 2) ? va_arg(args, uint64_t)
                           : (longness == 1) ? va_arg(args, unsigned long)
                                             : va_arg(args, unsigned int);
                format_uint(v, 16, *p == 'X', numbuf);
                emit_padded(numbuf, 0, width, zero_pad);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uintptr_t)va_arg(args, void*);
                emit_str("0x");
                format_uint(v, 16, 0, numbuf);
                emit_padded(numbuf, 0, width, zero_pad);
                break;
            }
            case '%':
                emit_char('%');
                break;
            case '\0':
                return;  /* trailing '%' */
            default:
                emit_char('%');
                emit_char(*p);
                break;
        }
    }
}

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}

void klog(klog_level_t level, const char* tag, const char* fmt, ...) {
    if (level > klog_level) return;

    if (tag) {
        emit_char('[');
        emit_str(tag);
        emit_str("] ");
    }

    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
