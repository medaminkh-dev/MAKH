#include <proc/fork.h>
#include <kernel.h>
#include <mm/kheap.h>
#include <lib/string.h>

/**
 * =============================================================================
 * kernel/proc/fork/memory.c - Memory Copying for Fork
 * =============================================================================
 * Phase 12: Kernel stack and user memory copying
 * =============================================================================
 */

int copy_kernel_stack(process_t* parent, process_t* child) {
    if (!parent || !child) return -1;
    
    // Allocate new kernel stack for child
    child->kernel_stack = (uint64_t)kmalloc(parent->kernel_stack_size);
    if (!child->kernel_stack) return -1;
    
    child->kernel_stack_size = parent->kernel_stack_size;
    
    // Copy stack contents
    uint8_t* src = (uint8_t*)parent->kernel_stack;
    uint8_t* dst = (uint8_t*)child->kernel_stack;
    for (uint64_t i = 0; i < parent->kernel_stack_size; i++) {
        dst[i] = src[i];
    }
    
    // Adjust RSP offset (calculate from base)
    uint64_t rsp_offset = parent->context.rsp - parent->kernel_stack;
    child->context.rsp = child->kernel_stack + rsp_offset;
    
    return 0;
}

int copy_user_memory(process_t* parent, process_t* child) {
    (void)parent;
    (void)child;
    
    // Simplified implementation - child shares parent's address space
    // Full COW will be in Phase 15
    return 0;
}
