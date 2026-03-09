#include <proc/pcb.h>
#include <kernel.h>
#include <vga.h>

/**
 * =============================================================================
 * kernel/proc/pcb/tree.c - Process Tree Management
 * =============================================================================
 * Phase 11: Parent-child relationships and reparenting
 * =============================================================================
 */

extern process_t* proc_find(int32_t pid);

/**
 * proc_add_child - Add child to parent's children list
 */
void proc_add_child(process_t* parent, process_t* child) {
    if (!parent || !child) return;
    
    child->parent_pid = parent->pid;
    child->sibling_next = NULL;
    
    if (parent->children_tail) {
        parent->children_tail->sibling_next = child;
        parent->children_tail = child;
    } else {
        parent->children_head = child;
        parent->children_tail = child;
    }
    parent->child_count++;
}

/**
 * proc_remove_child - Remove child from parent's children list
 */
void proc_remove_child(process_t* child) {
    if (!child) return;
    
    process_t* parent = proc_find(child->parent_pid);
    if (!parent) return;
    
    process_t* prev = NULL;
    process_t* curr = parent->children_head;
    
    while (curr) {
        if (curr == child) {
            if (prev) {
                prev->sibling_next = curr->sibling_next;
            } else {
                parent->children_head = curr->sibling_next;
            }
            if (curr == parent->children_tail) {
                parent->children_tail = prev;
            }
            parent->child_count--;
            break;
        }
        prev = curr;
        curr = curr->sibling_next;
    }
}

/**
 * proc_reparent_orphans - Reparent children to init (PID 1) when parent dies
 */
void proc_reparent_orphans(process_t* dead_parent) {
    if (!dead_parent) return;
    
    process_t* child = dead_parent->children_head;
    process_t* init = proc_find(1);
    
    if (!init) return;
    
    while (child) {
        process_t* next = child->sibling_next;
        child->parent_pid = 1;
        child->sibling_next = NULL;
        
        if (init->children_tail) {
            init->children_tail->sibling_next = child;
            init->children_tail = child;
        } else {
            init->children_head = child;
            init->children_tail = child;
        }
        init->child_count++;
        
        child = next;
    }
    
    dead_parent->children_head = NULL;
    dead_parent->children_tail = NULL;
    dead_parent->child_count = 0;
}
