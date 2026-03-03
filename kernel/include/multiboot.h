/**
 * MakhOS - multiboot.h
 * Multiboot2 header and structures
 * Based on the Multiboot2 Specification
 */

#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "types.h"

/* Multiboot2 magic numbers */
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

/* Multiboot2 tag types */
#define MULTIBOOT_TAG_TYPE_END          0
#define MULTIBOOT_TAG_TYPE_CMDLINE      1
#define MULTIBOOT_TAG_TYPE_BOOT_LOADER  2
#define MULTIBOOT_TAG_TYPE_MODULE       3
#define MULTIBOOT_TAG_TYPE_BASIC_MEM    4
#define MULTIBOOT_TAG_TYPE_BOOTDEV      5
#define MULTIBOOT_TAG_TYPE_MMAP         6
#define MULTIBOOT_TAG_TYPE_VBE          7
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER  8
#define MULTIBOOT_TAG_TYPE_ELF_SECTIONS 9
#define MULTIBOOT_TAG_TYPE_APM          10
#define MULTIBOOT_TAG_TYPE_EFI_32       11
#define MULTIBOOT_TAG_TYPE_EFI_64       12
#define MULTIBOOT_TAG_TYPE_SMBIOS       13
#define MULTIBOOT_TAG_TYPE_ACPI_OLD     14
#define MULTIBOOT_TAG_TYPE_ACPI_NEW     15
#define MULTIBOOT_TAG_TYPE_NETWORK      16
#define MULTIBOOT_TAG_TYPE_EFI_MMAP     17
#define MULTIBOOT_TAG_TYPE_EFI_BS       18
#define MULTIBOOT_TAG_TYPE_EFI_IMAGE    19
#define MULTIBOOT_TAG_TYPE_LOAD_BASE    20

/* Memory map entry types */
#define MULTIBOOT_MEMORY_AVAILABLE      1
#define MULTIBOOT_MEMORY_RESERVED       2
#define MULTIBOOT_MEMORY_ACPI_RECLAIM   3
#define MULTIBOOT_MEMORY_NVS            4
#define MULTIBOOT_MEMORY_BADRAM         5

/* Memory map entry - DEFINED FIRST */
struct multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} __attribute__((packed));

/* Multiboot2 tag header (all tags start with this) */
struct multiboot_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

/* Multiboot2 information structure */
struct multiboot_info {
    uint32_t total_size;
    uint32_t reserved;
    struct multiboot_tag tags[0];
} __attribute__((packed));

/* Basic memory information tag */
struct multiboot_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __attribute__((packed));

/* Memory map tag - uses previously defined multiboot_mmap_entry */
struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[0];
} __attribute__((packed));

/* Framebuffer tag */
struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
} __attribute__((packed));

/* Module tag */
struct multiboot_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     cmdline[0];
} __attribute__((packed));

/* Command line tag */
struct multiboot_tag_string {
    uint32_t type;
    uint32_t size;
    char     string[0];
} __attribute__((packed));

/* Parsed memory region */
struct memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

/* Memory map structure to store parsed regions */
#define MAX_MEMORY_REGIONS 32
struct memory_map {
    uint32_t count;
    struct memory_region regions[MAX_MEMORY_REGIONS];
    uint64_t total_available;
};

/* Function prototypes */
void multiboot_parse(uint64_t mb_info_addr);
struct multiboot_tag* multiboot_find_tag(uint32_t type);
void multiboot_parse_mmap(void);
void multiboot_print_info(void);

/* Getters */
const struct memory_map* get_memory_map(void);
uint64_t get_total_ram(void);

#endif /* MULTIBOOT_H */
