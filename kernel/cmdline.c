/**
 * MakhOS - cmdline.c
 * Parse and query the Multiboot2 kernel command line.
 */

#include <cmdline.h>
#include <multiboot.h>
#include <klog.h>

static char cmdline_buf[256];

static void copy_cmdline(const char* src) {
    size_t i = 0;
    if (src) {
        while (src[i] && i < sizeof(cmdline_buf) - 1) {
            cmdline_buf[i] = src[i];
            i++;
        }
    }
    cmdline_buf[i] = '\0';
}

void cmdline_init(uint64_t mb_info_addr) {
    cmdline_buf[0] = '\0';
    if (mb_info_addr == 0) return;

    /* Walk the Multiboot2 tag list looking for the cmdline tag (type 1). */
    struct multiboot_info* info = (struct multiboot_info*)(uintptr_t)mb_info_addr;
    struct multiboot_tag* tag = (struct multiboot_tag*)((uintptr_t)info + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_CMDLINE) {
            struct multiboot_tag_string* s = (struct multiboot_tag_string*)tag;
            copy_cmdline(s->string);
            break;
        }
        uintptr_t next = ((uintptr_t)tag + ((tag->size + 7) & ~7u));
        tag = (struct multiboot_tag*)next;
    }

    if (cmdline_buf[0]) {
        KLOG_I("CMDLINE", "boot args: %s\n", cmdline_buf);
    }
}

const char* cmdline_get(void) {
    return cmdline_buf;
}

/* Compare token starting at `s` against `flag`; token ends at space or NUL. */
static int token_matches(const char* s, const char* flag) {
    size_t i = 0;
    while (flag[i]) {
        if (s[i] != flag[i]) return 0;
        i++;
    }
    return s[i] == '\0' || s[i] == ' ' || s[i] == '\t';
}

int cmdline_has(const char* flag) {
    const char* s = cmdline_buf;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') break;
        if (token_matches(s, flag)) return 1;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    return 0;
}
