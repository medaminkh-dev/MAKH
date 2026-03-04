/**
 * MakhOS - pmm.h
 * Physical Memory Manager header
 * Manages allocation of physical pages (4KB each)
 */

#ifndef MAKHOS_PMM_H
#define MAKHOS_PMM_H

#include <types.h>
#include <multiboot.h>

/* Page size (4KB) */
#define PAGE_SIZE 4096
#define PAGE_ALIGN_MASK (~(PAGE_SIZE - 1))

/* Maximum memory supported (4GB) - for static bitmap sizing */
#define MAX_MEMORY_SIZE (4ULL * 1024 * 1024 * 1024)
#define MAX_BITMAP_SIZE (MAX_MEMORY_SIZE / PAGE_SIZE / 8)

/* Reserve first 4MB for kernel, VGA, BIOS */
#define RESERVED_LOW_MEMORY 0x400000

/* Kernel region (1MB - 2MB as mentioned) */
#define KERNEL_START 0x100000
#define KERNEL_END   0x200000

/**
 * pmm_init - Initialize PMM using multiboot memory map
 * @mmap_tag: Pointer to multiboot memory map tag
 */
void pmm_init(struct multiboot_tag_mmap *mmap_tag);

/**
 * pmm_alloc_page - Allocate a single physical page (4KB)
 * Returns: Physical address of allocated page, or NULL if out of memory
 */
void* pmm_alloc_page(void);

/**
 * pmm_free_page - Free a previously allocated physical page
 * @phys_addr: Physical address of page to free
 */
void pmm_free_page(void* phys_addr);

/**
 * pmm_get_free_memory - Get amount of free memory in bytes
 * Returns: Number of free bytes
 */
uint64_t pmm_get_free_memory(void);

/**
 * pmm_get_total_memory - Get total amount of managed memory in bytes
 * Returns: Total number of bytes managed by PMM
 */
uint64_t pmm_get_total_memory(void);

/**
 * pmm_get_used_page_count - Get number of allocated pages
 * Returns: Number of pages currently in use
 */
uint32_t pmm_get_used_page_count(void);

/**
 * pmm_get_total_page_count - Get total number of pages
 * Returns: Total number of pages managed by PMM
 */
uint32_t pmm_get_total_page_count(void);

#endif /* MAKHOS_PMM_H */
