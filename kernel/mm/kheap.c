/**
 * MakhOS - kheap.c
 * Kernel Heap Manager implementation
 * Dynamic memory allocation using block allocator with header/footer
 */

#include "mm/kheap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "vga.h"
#include "kernel.h"

/* External helper from kernel.c for number conversion */
extern void uint64_to_string(uint64_t value, char* buf);

/* Heap state variables */
static block_header_t* free_list_head = NULL;   /* Head of free list */
static uint64_t heap_start = HEAP_START_VIRTUAL; /* Current heap start */
static uint64_t heap_end = HEAP_START_VIRTUAL;   /* Current heap end (next unallocated address) */
static uint64_t heap_max = HEAP_START_VIRTUAL;   /* Maximum heap address mapped */
static size_t heap_used = 0;                     /* Total used memory */

/* Helper function prototypes */
static void* kheap_expand(size_t pages);
static block_header_t* find_free_block(size_t size);
static void split_block(block_header_t* block, size_t size);
static block_footer_t* get_footer(block_header_t* header);
static void coalesce_block(block_header_t* block);
static void remove_from_free_list(block_header_t* block);
static void add_to_free_list(block_header_t* block);
static size_t align_up(size_t value, size_t alignment);

/**
 * align_up - Align value up to boundary
 */
static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/**
 * get_footer - Get footer for a block header
 */
static block_footer_t* get_footer(block_header_t* header) {
    return (block_footer_t*)((uint8_t*)header + header->size - sizeof(block_footer_t));
}


/**
 * remove_from_free_list - Remove block from free list
 */
static void remove_from_free_list(block_header_t* block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_list_head = block->next;
    }
    
    if (block->next) {
        block->next->prev = block->prev;
    }
    
    block->next = NULL;
    block->prev = NULL;
}

/**
 * add_to_free_list - Add block to free list (insert at head)
 */
static void add_to_free_list(block_header_t* block) {
    block->next = free_list_head;
    block->prev = NULL;
    
    if (free_list_head) {
        free_list_head->prev = block;
    }
    
    free_list_head = block;
}

/**
 * kheap_expand - Expand heap by mapping new pages
 * @pages: Number of pages to add
 * Returns: Pointer to newly allocated region, or NULL on failure
 */
static void* kheap_expand(size_t pages) {
    size_t bytes_needed = pages * PAGE_SIZE;
    uint64_t old_end = heap_end;
    uint64_t new_end = heap_end + bytes_needed;
    
    /* Map new pages */
    for (uint64_t addr = old_end; addr < new_end; addr += PAGE_SIZE) {
        /* Allocate physical page */
        void* phys_page = pmm_alloc_page();
        if (phys_page == NULL) {
            /* Failed - cleanup already mapped pages? */
            /* For simplicity, we just return NULL */
            return NULL;
        }
        
        /* Map to virtual address */
        if (vmm_map_page(addr, (uint64_t)(uintptr_t)phys_page, 
                         PAGE_PRESENT | PAGE_WRITABLE) != 0) {
            pmm_free_page(phys_page);
            return NULL;
        }
    }
    
    heap_end = new_end;
    if (new_end > heap_max) {
        heap_max = new_end;
    }
    
    return (void*)old_end;
}

/**
 * find_free_block - Find a free block using first-fit strategy
 * @size: Minimum block size needed (including headers)
 * Returns: Pointer to suitable block, or NULL if none found
 */
static block_header_t* find_free_block(size_t size) {
    block_header_t* current = free_list_head;
    
    while (current != NULL) {
        if (current->magic == KHEAP_MAGIC_FREE && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

/**
 * split_block - Split a large block into two blocks
 * @block: Block to split
 * @size: Size for first block (including headers)
 */
static void split_block(block_header_t* block, size_t size) {
    /* Calculate remaining size */
    size_t remaining = block->size - size;
    
    /* Only split if remaining is large enough for a new block */
    if (remaining >= sizeof(block_header_t) + sizeof(block_footer_t) + HEAP_MIN_BLOCK) {
        /* Create new block header at the split point */
        block_header_t* new_block = (block_header_t*)((uint8_t*)block + size);
        new_block->magic = KHEAP_MAGIC_FREE;
        new_block->size = remaining;
        new_block->allocated = 0;
        new_block->next = NULL;
        new_block->prev = NULL;
        
        /* Create footer for new block */
        block_footer_t* new_footer = get_footer(new_block);
        new_footer->magic = KHEAP_MAGIC_TAIL;
        new_footer->size = remaining;
        new_footer->header = new_block;
        
        /* Update original block size */
        block->size = size;
        
        /* Update original block footer */
        block_footer_t* footer = get_footer(block);
        footer->size = size;
        footer->header = block;
        
        /* Add new block to free list */
        add_to_free_list(new_block);
    }
}

/**
 * coalesce_block - Merge adjacent free blocks
 * @block: Block to coalesce (must be free)
 */
static void coalesce_block(block_header_t* block) {
    if (block->magic != KHEAP_MAGIC_FREE || block->allocated != 0) {
        return;
    }
    
    /* Try to coalesce with next block */
    block_header_t* next_block = (block_header_t*)((uint8_t*)block + block->size);
    
    if ((uint64_t)next_block < heap_end && 
        next_block->magic == KHEAP_MAGIC_FREE && 
        next_block->allocated == 0) {
        /* Remove next block from free list */
        remove_from_free_list(next_block);
        
        /* Expand current block */
        block->size += next_block->size;
        
        /* Update footer */
        block_footer_t* footer = get_footer(block);
        footer->magic = KHEAP_MAGIC_TAIL;
        footer->size = block->size;
        footer->header = block;
    }
    
    /* Try to coalesce with previous block */
    /* Check if there's a block before us */
    if ((uint64_t)block > heap_start) {
        /* Get footer of previous block */
        block_footer_t* prev_footer = (block_footer_t*)((uint8_t*)block - sizeof(block_footer_t));
        
        if (prev_footer->magic == KHEAP_MAGIC_TAIL) {
            block_header_t* prev_block = prev_footer->header;
            
            if (prev_block->magic == KHEAP_MAGIC_FREE && prev_block->allocated == 0) {
                /* Remove current block from free list */
                remove_from_free_list(block);
                
                /* Expand previous block */
                prev_block->size += block->size;
                
                /* Update footer */
                block_footer_t* footer = get_footer(prev_block);
                footer->magic = KHEAP_MAGIC_TAIL;
                footer->size = prev_block->size;
                footer->header = prev_block;
                
                /* Block is now part of previous block */
                block = prev_block;
            }
        }
    }
}

/**
 * kheap_init - Initialize the kernel heap
 * Maps initial pages and sets up the first free block
 */
void kheap_init(void) {
    terminal_writestring("[KHEAP] Initializing kernel heap...\n");
    
    /* Map initial pages */
    for (int i = 0; i < HEAP_INITIAL_PAGES; i++) {
        void* phys_page = pmm_alloc_page();
        if (phys_page == NULL) {
            kernel_panic("KHEAP: Failed to allocate initial page");
        }
        
        uint64_t virt_addr = HEAP_START_VIRTUAL + (i * PAGE_SIZE);
        if (vmm_map_page(virt_addr, (uint64_t)(uintptr_t)phys_page,
                         PAGE_PRESENT | PAGE_WRITABLE) != 0) {
            kernel_panic("KHEAP: Failed to map initial page");
        }
    }
    
    heap_start = HEAP_START_VIRTUAL;
    heap_end = HEAP_START_VIRTUAL + HEAP_INITIAL_SIZE;
    heap_max = heap_end;
    heap_used = 0;
    
    /* Create initial free block covering entire heap */
    block_header_t* initial_block = (block_header_t*)heap_start;
    initial_block->magic = KHEAP_MAGIC_FREE;
    initial_block->size = HEAP_INITIAL_SIZE;
    initial_block->allocated = 0;
    initial_block->next = NULL;
    initial_block->prev = NULL;
    
    /* Create footer for initial block */
    block_footer_t* footer = get_footer(initial_block);
    footer->magic = KHEAP_MAGIC_TAIL;
    footer->size = HEAP_INITIAL_SIZE;
    footer->header = initial_block;
    
    /* Add to free list */
    free_list_head = initial_block;
    
    terminal_writestring("[KHEAP] Heap initialized at 0x");
    char buf[32];
    /* Simple hex output */
    const char* hex = "0123456789ABCDEF";
    int i = 0;
    uint64_t value = heap_start;
    buf[0] = '0';
    buf[1] = 'x';
    i = 2;
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int nibble = (value >> shift) & 0xF;
        if (nibble != 0 || started || shift == 0) {
            buf[i++] = hex[nibble];
            started = 1;
        }
    }
    buf[i] = '\0';
    terminal_writestring(buf);
    terminal_writestring(" size=");
    uint64_to_string(HEAP_INITIAL_SIZE, buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
}

/**
 * kmalloc - Allocate memory from heap
 * @size: Number of bytes to allocate
 * Returns: Pointer to allocated memory, or NULL on failure
 */
void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    /* Calculate total size needed (header + data + footer) */
    size_t aligned_size = align_up(size, HEAP_ALIGNMENT);
    size_t total_size = sizeof(block_header_t) + aligned_size + sizeof(block_footer_t);
    
    /* Minimum block size */
    if (total_size < HEAP_MIN_BLOCK) {
        total_size = HEAP_MIN_BLOCK;
    }
    
    /* Find a suitable free block */
    block_header_t* block = find_free_block(total_size);
    
    /* If no block found, expand heap */
    if (block == NULL) {
        /* Calculate pages needed */
        size_t pages_needed = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages_needed < 1) pages_needed = 1;
        
        void* new_region = kheap_expand(pages_needed);
        if (new_region == NULL) {
            return NULL;  /* Out of memory */
        }
        
        /* Create new free block at the new region */
        block = (block_header_t*)new_region;
        block->magic = KHEAP_MAGIC_FREE;
        block->size = pages_needed * PAGE_SIZE;
        block->allocated = 0;
        block->next = NULL;
        block->prev = NULL;
        
        /* Create footer */
        block_footer_t* footer = get_footer(block);
        footer->magic = KHEAP_MAGIC_TAIL;
        footer->size = block->size;
        footer->header = block;
        
        /* Add to free list */
        add_to_free_list(block);
    }
    
    /* Remove block from free list */
    remove_from_free_list(block);
    
    /* Split block if it's much larger than needed */
    split_block(block, total_size);
    
    /* Mark block as used */
    block->magic = KHEAP_MAGIC_USED;
    block->allocated = 1;
    
    /* Update footer */
    block_footer_t* footer = get_footer(block);
    footer->magic = KHEAP_MAGIC_TAIL;
    
    /* Update heap statistics */
    heap_used += block->size;
    
    /* Return pointer to data area (after header) */
    return (void*)((uint8_t*)block + sizeof(block_header_t));
}

/**
 * kcalloc - Allocate zeroed memory
 * @num: Number of elements
 * @size: Size of each element
 * Returns: Pointer to zeroed memory, or NULL on failure
 */
void* kcalloc(size_t num, size_t size) {
    size_t total = num * size;
    
    /* Check for overflow */
    if (num != 0 && total / num != size) {
        return NULL;
    }
    
    void* ptr = kmalloc(total);
    if (ptr != NULL) {
        /* Zero the memory */
        uint8_t* p = (uint8_t*)ptr;
        for (size_t i = 0; i < total; i++) {
            p[i] = 0;
        }
    }
    
    return ptr;
}

/**
 * kmalloc_aligned - Allocate aligned memory
 * @size: Number of bytes to allocate
 * @alignment: Alignment requirement (must be power of 2)
 * Returns: Pointer to aligned memory, or NULL on failure
 */
void* kmalloc_aligned(size_t size, size_t alignment) {
    if (alignment < HEAP_ALIGNMENT) {
        alignment = HEAP_ALIGNMENT;
    }
    
    /* Allocate extra space for alignment and header */
    size_t total_size = size + alignment + sizeof(block_header_t) + sizeof(block_footer_t);
    
    void* raw = kmalloc(total_size);
    if (raw == NULL) return NULL;
    
    /* Calculate aligned address */
    uint64_t raw_addr = (uint64_t)(uintptr_t)raw;
    uint64_t aligned_addr = align_up(raw_addr, alignment);
    
    /* If already aligned, return as-is */
    if (aligned_addr == raw_addr) {
        return raw;
    }
    
    /* Create a dummy block for the padding before aligned address */
    /* This is a simplified implementation - full implementation would split */
    /* For now, we waste some memory but ensure alignment */
    
    return (void*)(uintptr_t)aligned_addr;
}

/**
 * krealloc - Reallocate memory
 * @ptr: Pointer to existing memory (can be NULL)
 * @new_size: New size in bytes
 * Returns: Pointer to reallocated memory, or NULL on failure
 */
void* krealloc(void* ptr, size_t new_size) {
    /* If ptr is NULL, behave like malloc */
    if (ptr == NULL) {
        return kmalloc(new_size);
    }
    
    /* If new_size is 0, behave like free */
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    /* Get the block header */
    block_header_t* header = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    
    /* Validate magic */
    if (header->magic != KHEAP_MAGIC_USED) {
        return NULL;  /* Invalid block */
    }
    
    /* Calculate current data size */
    size_t current_size = header->size - sizeof(block_header_t) - sizeof(block_footer_t);
    
    /* If new size is smaller, we could shrink but for simplicity we just return ptr */
    if (new_size <= current_size) {
        return ptr;
    }
    
    /* Allocate new block */
    void* new_ptr = kmalloc(new_size);
    if (new_ptr == NULL) {
        return NULL;
    }
    
    /* Copy data */
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < current_size; i++) {
        dst[i] = src[i];
    }
    
    /* Free old block */
    kfree(ptr);
    
    return new_ptr;
}

/**
 * kfree - Free allocated memory
 * @ptr: Pointer to memory to free
 */
void kfree(void* ptr) {
    if (ptr == NULL) return;
    
    /* Get block header */
    block_header_t* header = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    
    /* Validate magic */
    if (header->magic != KHEAP_MAGIC_USED) {
        /* Invalid block - could be double free or corruption */
        return;
    }
    
    /* Validate footer */
    block_footer_t* footer = get_footer(header);
    if (footer->magic != KHEAP_MAGIC_TAIL || footer->header != header) {
        /* Footer corruption */
        return;
    }
    
    /* Update heap statistics */
    heap_used -= header->size;
    
    /* Mark as free */
    header->magic = KHEAP_MAGIC_FREE;
    header->allocated = 0;
    
    /* Add to free list */
    add_to_free_list(header);
    
    /* Coalesce with adjacent blocks */
    coalesce_block(header);
}

/**
 * kheap_get_used - Get total used heap memory
 * Returns: Number of bytes currently allocated
 */
size_t kheap_get_used(void) {
    return heap_used;
}

/**
 * kheap_get_free - Get total free heap memory
 * Returns: Number of bytes available for allocation
 */
size_t kheap_get_free(void) {
    size_t free = 0;
    block_header_t* current = free_list_head;
    
    while (current != NULL) {
        if (current->magic == KHEAP_MAGIC_FREE) {
            free += current->size - sizeof(block_header_t) - sizeof(block_footer_t);
        }
        current = current->next;
    }
    
    return free;
}

/**
 * kheap_get_total - Get total heap size
 * Returns: Total heap size in bytes
 */
size_t kheap_get_total(void) {
    return heap_end - heap_start;
}
