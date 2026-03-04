/**
 * MakhOS - pmm.c
 * Physical Memory Manager implementation
 * Uses bitmap-based allocation with first-fit algorithm
 */

#include "mm/pmm.h"
#include "vga.h"

/* Static bitmap for page tracking - each bit represents one 4KB page */
static uint8_t bitmap[MAX_BITMAP_SIZE];

/* PMM statistics */
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;
static uint64_t total_memory = 0;

/* Maximum page number we can manage */
static uint64_t max_page_num = 0;

/* Helper function prototypes */
static void bitmap_set(uint64_t page_num);
static void bitmap_clear(uint64_t page_num);
static int bitmap_test(uint64_t page_num);
static uint64_t addr_to_page(void* phys_addr);
static void* page_to_addr(uint64_t page_num);

/**
 * uint64_to_string - Convert uint64 to decimal string
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
 * bitmap_set - Mark a page as used in the bitmap
 * @page_num: Page number to set
 */
static void bitmap_set(uint64_t page_num) {
    if (page_num >= max_page_num) return;
    uint64_t byte_idx = page_num / 8;
    uint8_t bit_pos = page_num % 8;
    bitmap[byte_idx] |= (1 << bit_pos);
}

/**
 * bitmap_clear - Mark a page as free in the bitmap
 * @page_num: Page number to clear
 */
static void bitmap_clear(uint64_t page_num) {
    if (page_num >= max_page_num) return;
    uint64_t byte_idx = page_num / 8;
    uint8_t bit_pos = page_num % 8;
    bitmap[byte_idx] &= ~(1 << bit_pos);
}

/**
 * bitmap_test - Check if a page is used
 * @page_num: Page number to test
 * Returns: 1 if used, 0 if free
 */
static int bitmap_test(uint64_t page_num) {
    if (page_num >= max_page_num) return 1; /* Out of range = used/reserved */
    uint64_t byte_idx = page_num / 8;
    uint8_t bit_pos = page_num % 8;
    return (bitmap[byte_idx] >> bit_pos) & 1;
}

/**
 * addr_to_page - Convert physical address to page number
 * @phys_addr: Physical address
 * Returns: Page number
 */
static uint64_t addr_to_page(void* phys_addr) {
    return (uint64_t)(uintptr_t)phys_addr / PAGE_SIZE;
}

/**
 * page_to_addr - Convert page number to physical address
 * @page_num: Page number
 * Returns: Physical address
 */
static void* page_to_addr(uint64_t page_num) {
    return (void*)(uintptr_t)(page_num * PAGE_SIZE);
}

/**
 * pmm_init - Initialize PMM using multiboot memory map
 * @mmap_tag: Pointer to multiboot memory map tag
 */
void pmm_init(struct multiboot_tag_mmap *mmap_tag) {
    char buf[32];
    
    /* Initialize bitmap to all used (1s) - safer default */
    uint64_t i;
    for (i = 0; i < MAX_BITMAP_SIZE; i++) {
        bitmap[i] = 0xFF;
    }
    
    if (mmap_tag == NULL) {
        terminal_writestring("[PMM] ERROR: No memory map provided\n");
        return;
    }
    
    /* Calculate number of entries in the memory map */
    uint32_t entry_count = (mmap_tag->size - 16) / mmap_tag->entry_size;
    
    /* First pass: find the highest address to determine max_page_num */
    uint64_t highest_addr = 0;
    for (i = 0; i < entry_count; i++) {
        struct multiboot_mmap_entry* entry = 
            (struct multiboot_mmap_entry*)((uintptr_t)mmap_tag->entries + i * mmap_tag->entry_size);
        
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t end_addr = entry->addr + entry->len;
            if (end_addr > highest_addr) {
                highest_addr = end_addr;
            }
        }
    }
    
    /* Cap at MAX_MEMORY_SIZE */
    if (highest_addr > MAX_MEMORY_SIZE) {
        highest_addr = MAX_MEMORY_SIZE;
    }
    
    max_page_num = highest_addr / PAGE_SIZE;
    total_memory = highest_addr;
    total_pages = (uint32_t)max_page_num;
    
    /* Second pass: mark available regions as free */
    for (i = 0; i < entry_count; i++) {
        struct multiboot_mmap_entry* entry = 
            (struct multiboot_mmap_entry*)((uintptr_t)mmap_tag->entries + i * mmap_tag->entry_size);
        
        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t start_page = entry->addr / PAGE_SIZE;
            uint64_t end_page = (entry->addr + entry->len) / PAGE_SIZE;
            
            /* Clamp to our max */
            if (end_page > max_page_num) {
                end_page = max_page_num;
            }
            
            /* Mark pages as free */
            uint64_t page;
            for (page = start_page; page < end_page; page++) {
                bitmap_clear(page);
            }
        }
    }
    
    /* Reserve first 4MB (0x0 - 0x400000) for kernel, VGA, BIOS */
    uint64_t reserved_pages = RESERVED_LOW_MEMORY / PAGE_SIZE;
    for (i = 0; i < reserved_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
        }
    }
    
    terminal_writestring("[PMM] Initialized - Total: ");
    uint64_to_string(total_memory / (1024 * 1024), buf);
    terminal_writestring(buf);
    terminal_writestring(" MB, Pages: ");
    uint64_to_string(total_pages, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

/**
 * pmm_alloc_page - Allocate a single physical page (4KB)
 * Uses first-fit algorithm
 * Returns: Physical address of allocated page, or NULL if out of memory
 */
void* pmm_alloc_page(void) {
    /* Search for first free page */
    uint64_t page;
    for (page = 0; page < max_page_num; page++) {
        if (!bitmap_test(page)) {
            /* Found a free page */
            bitmap_set(page);
            used_pages++;
            return page_to_addr(page);
        }
    }
    
    /* Out of memory */
    return NULL;
}

/**
 * pmm_free_page - Free a previously allocated physical page
 * @phys_addr: Physical address of page to free
 */
void pmm_free_page(void* phys_addr) {
    if (phys_addr == NULL) return;
    
    /* Check alignment */
    if ((uintptr_t)phys_addr & (PAGE_SIZE - 1)) {
        /* Not page-aligned - error */
        return;
    }
    
    uint64_t page = addr_to_page(phys_addr);
    
    if (page >= max_page_num) return;
    if (!bitmap_test(page)) return; /* Already free */
    
    bitmap_clear(page);
    used_pages--;
}

/**
 * pmm_get_free_memory - Get amount of free memory in bytes
 */
uint64_t pmm_get_free_memory(void) {
    return (uint64_t)(total_pages - used_pages) * PAGE_SIZE;
}

/**
 * pmm_get_total_memory - Get total amount of managed memory in bytes
 */
uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

/**
 * pmm_get_used_page_count - Get number of allocated pages
 */
uint32_t pmm_get_used_page_count(void) {
    return used_pages;
}

/**
 * pmm_get_total_page_count - Get total number of pages
 */
uint32_t pmm_get_total_page_count(void) {
    return total_pages;
}
