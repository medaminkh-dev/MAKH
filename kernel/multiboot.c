/**
 * MakhOS - multiboot.c
 * Multiboot2 information parser implementation
 */

#include "include/multiboot.h"
#include "include/vga.h"

/* Internal state */
static struct multiboot_info* mb_info = NULL;
static struct memory_map mem_map = {0};

/**
 * uint64_to_string - Convert uint64 to decimal string
 * Helper function since we don't have printf
 */
static void uint64_to_string(uint64_t value, char* buf) {
    char temp[24];
    int i = 0;
    
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    
    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    int j;
    for (j = 0; j < i; j++) {
        buf[j] = temp[i - 1 - j];
    }
    buf[i] = '\0';
}

/**
 * uint64_to_hex - Convert uint64 to hex string
 */
static void uint64_to_hex(uint64_t value, char* buf) {
    const char* hex = "0123456789ABCDEF";
    char temp[20];
    int i = 0;
    
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    
    while (value > 0) {
        temp[i++] = hex[value & 0xF];
        value >>= 4;
    }
    
    buf[0] = '0';
    buf[1] = 'x';
    int j;
    for (j = 0; j < i; j++) {
        buf[j + 2] = temp[i - 1 - j];
    }
    buf[i + 2] = '\0';
}

/**
 * multiboot_parse - Parse multiboot2 information structure
 * @mb_info_addr: Physical address of multiboot info (from EBX/RDI)
 */
void multiboot_parse(uint64_t mb_info_addr) {
    mb_info = (struct multiboot_info*)(uintptr_t)mb_info_addr;
    
    /* Parse memory map if available */
    multiboot_parse_mmap();
}

/**
 * multiboot_find_tag - Find a specific tag in multiboot info
 * @type: Tag type to find
 * Returns: Pointer to tag or NULL if not found
 */
struct multiboot_tag* multiboot_find_tag(uint32_t type) {
    if (mb_info == NULL) {
        return NULL;
    }
    
    /* Tags start after the fixed header (8 bytes) */
    struct multiboot_tag* tag = (struct multiboot_tag*)((uintptr_t)mb_info + 8);
    
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == type) {
            return tag;
        }
        
        /* Move to next tag (8-byte aligned) */
        uintptr_t next_addr = ((uintptr_t)tag + ((tag->size + 7) & ~7));
        tag = (struct multiboot_tag*)next_addr;
        
        /* Safety check - don't go past total_size */
        if ((uintptr_t)tag >= (uintptr_t)mb_info + mb_info->total_size) {
            break;
        }
    }
    
    return NULL;
}

/**
 * multiboot_parse_mmap - Parse memory map from multiboot info
 */
void multiboot_parse_mmap(void) {
    struct multiboot_tag_mmap* mmap_tag = 
        (struct multiboot_tag_mmap*)multiboot_find_tag(MULTIBOOT_TAG_TYPE_MMAP);
    
    if (mmap_tag == NULL) {
        return;
    }
    
    mem_map.count = 0;
    mem_map.total_available = 0;
    
    /* Calculate number of entries */
    uint32_t entry_count = (mmap_tag->size - 16) / mmap_tag->entry_size;
    
    uint32_t i;
    for (i = 0; i < entry_count && i < MAX_MEMORY_REGIONS; i++) {
        struct multiboot_mmap_entry* entry = 
            (struct multiboot_mmap_entry*)((uintptr_t)mmap_tag->entries + i * mmap_tag->entry_size);
        
        mem_map.regions[i].base = entry->addr;
        mem_map.regions[i].length = entry->len;
        mem_map.regions[i].type = entry->type;
        
        /* Calculate total available RAM */
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            mem_map.total_available += entry->len;
        }
        
        mem_map.count++;
    }
}

/**
 * multiboot_print_info - Print multiboot information
 */
void multiboot_print_info(void) {
    char buf[32];
    
    terminal_writestring("Memory Map:\n");
    
    if (mem_map.count == 0) {
        terminal_writestring("  [ERROR] No memory map available\n");
        return;
    }
    
    uint32_t i;
    for (i = 0; i < mem_map.count; i++) {
        struct memory_region* region = &mem_map.regions[i];
        
        /* Print base address */
        terminal_writestring("  Region ");
        uint64_to_string(i, buf);
        terminal_writestring(buf);
        terminal_writestring(": 0x");
        
        /* Base */
        uint64_to_hex(region->base, buf);
        terminal_writestring(buf);
        terminal_writestring(" - Size: ");
        
        /* Size in KB/MB/GB */
        uint64_t size = region->length;
        if (size >= 1024 * 1024 * 1024) {
            uint64_to_string(size / (1024 * 1024 * 1024), buf);
            terminal_writestring(buf);
            terminal_writestring(" GB");
        } else if (size >= 1024 * 1024) {
            uint64_to_string(size / (1024 * 1024), buf);
            terminal_writestring(buf);
            terminal_writestring(" MB");
        } else {
            uint64_to_string(size / 1024, buf);
            terminal_writestring(buf);
            terminal_writestring(" KB");
        }
        
        terminal_writestring(" - Type: ");
        
        switch (region->type) {
            case MULTIBOOT_MEMORY_AVAILABLE:
                terminal_writestring_color("Available", vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                break;
            case MULTIBOOT_MEMORY_RESERVED:
                terminal_writestring("Reserved");
                break;
            case MULTIBOOT_MEMORY_ACPI_RECLAIM:
                terminal_writestring("ACPI Reclaimable");
                break;
            case MULTIBOOT_MEMORY_NVS:
                terminal_writestring("ACPI NVS");
                break;
            case MULTIBOOT_MEMORY_BADRAM:
                terminal_writestring_color("BAD RAM", vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLACK));
                break;
            default:
                terminal_writestring("Unknown");
                break;
        }
        
        terminal_putchar('\n');
    }
    
    /* Print total available RAM */
    terminal_writestring("\nTotal Available RAM: ");
    
    uint64_t total = mem_map.total_available;
    if (total >= 1024 * 1024 * 1024) {
        uint64_to_string(total / (1024 * 1024 * 1024), buf);
        terminal_writestring(buf);
        terminal_writestring(" GB ");
        /* Remainder in MB */
        uint64_to_string((total % (1024 * 1024 * 1024)) / (1024 * 1024), buf);
        terminal_writestring("(");
        terminal_writestring(buf);
        terminal_writestring(" MB)");
    } else {
        uint64_to_string(total / (1024 * 1024), buf);
        terminal_writestring(buf);
        terminal_writestring(" MB");
    }
    terminal_putchar('\n');
}

/**
 * get_memory_map - Get the parsed memory map
 */
const struct memory_map* get_memory_map(void) {
    return &mem_map;
}

/**
 * get_total_ram - Get total available RAM in bytes
 */
uint64_t get_total_ram(void) {
    return mem_map.total_available;
}
