#include <proc/fork.h>
#include <kernel.h>
#include <vga.h>
#include <mm/kheap.h>
#include <lib/string.h>

/**
 * =============================================================================
 * kernel/proc/fork/memory.c - Memory Copying for Fork
 * =============================================================================
 * FIXES APPLIED:
 *   BUG#11: rsp_offset calculation can overflow if parent->context.rsp points
 *           to user stack (because syscall didn't switch stacks, BUG#2).
 *           Added bounds check: if rsp is outside kernel stack, use stack top.
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

    // FIX BUG#11: Bounds-check the RSP before computing offset.
    // If RSP is outside the kernel stack range (e.g., pointing to user stack
    // because of BUG#2), use the top of the kernel stack as fallback.
    uint64_t kstack_base = parent->kernel_stack;
    uint64_t kstack_top  = parent->kernel_stack + parent->kernel_stack_size;

    if (parent->context.rsp >= kstack_base && parent->context.rsp < kstack_top) {
        // RSP is within kernel stack - safe to compute offset
        uint64_t rsp_offset = parent->context.rsp - kstack_base;
        child->context.rsp  = child->kernel_stack + rsp_offset;
    } else {
        // RSP is OUTSIDE kernel stack (user stack or invalid).
        // FIX: Use top of new kernel stack as safe default.
        // Child will need proper RSP set up by context setup code.
        terminal_writestring("[FORK] WARNING: parent RSP outside kernel stack, using stack top\n");
        child->context.rsp = child->kernel_stack + child->kernel_stack_size;
    }

    return 0;
}

int copy_user_memory(process_t* parent, process_t* child) {
    (void)parent;
    (void)child;

    // Simplified implementation - child shares parent's address space
    // Full COW will be in Phase 15
    return 0;
}
