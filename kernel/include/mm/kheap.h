/**
 * MakhOS - kheap.h
 * Kernel Heap Manager header
 * Dynamic memory allocation for the kernel using block allocator
 */

#ifndef MAKHOS_KHEAP_H
#define MAKHOS_KHEAP_H

#include <types.h>

/* Heap configuration */
#define HEAP_START_VIRTUAL  0xFFFF900000000000ULL
#define HEAP_INITIAL_PAGES  4
#define HEAP_INITIAL_SIZE   (HEAP_INITIAL_PAGES * PAGE_SIZE)  /* 16KB */
#define HEAP_MIN_BLOCK      64
#define HEAP_ALIGNMENT      16

/* Magic numbers for block validation */
#define KHEAP_MAGIC_FREE    0x1234F4EE  /* 'FREE' in hex form */
#define KHEAP_MAGIC_USED    0x5678D5ED  /* 'USED' in hex form */
#define KHEAP_MAGIC_TAIL    0x9ABCDA1L  /* 'TAIL' in hex form */

/* Block header structure (16 bytes) */
typedef struct block_header {
    uint32_t magic;              /* Verification (KHEAP_MAGIC_FREE/USED) */
    uint32_t size;               /* Block size (including headers) */
    uint8_t  allocated;          /* 1 = used, 0 = free */
    uint8_t  padding[3];         /* Alignment to 16 bytes */
    struct block_header* next;   /* Next free block */
    struct block_header* prev;   /* Previous free block */
} __attribute__((packed)) block_header_t;

/* Block footer structure (for coalescing) */
typedef struct block_footer {
    uint32_t magic;              /* KHEAP_MAGIC_TAIL */
    uint32_t size;               /* Same as header */
    struct block_header* header; /* Pointer to header */
} __attribute__((packed)) block_footer_t;

/* Initialize heap */
void kheap_init(void);

/* Allocate memory (malloc equivalent) */
void* kmalloc(size_t size);

/* Allocate zeroed memory (calloc equivalent) */
void* kcalloc(size_t num, size_t size);

/* Allocate aligned memory (for DMA) */
void* kmalloc_aligned(size_t size, size_t alignment);

/* Reallocate memory (realloc equivalent) */
void* krealloc(void* ptr, size_t new_size);

/* Free memory */
void kfree(void* ptr);

/* Get heap statistics */
size_t kheap_get_used(void);
size_t kheap_get_free(void);
size_t kheap_get_total(void);

#endif /* MAKHOS_KHEAP_H */
