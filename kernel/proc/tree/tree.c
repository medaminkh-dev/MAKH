#include <proc_internal.h>
#include <kernel.h>

/**
 * =============================================================================
 * tree.c - Process tree (parent/child relationships)
 * =============================================================================
 * Children are kept in a singly linked list off the parent (children_head/
 * children_tail, linked via sibling_next). When a process exits, its children
 * are reparented to init (PID 1) so the tree never dangles.
 *
 * Callers (thread_create/thread_exit) already hold IRQs disabled, so these
 * routines do no locking of their own.
 * =============================================================================
 */

void proc_add_child(process_t *parent, process_t *child) {
    if (!parent || !child) return;

    child->parent_pid = parent->pid;
    child->sibling_next = NULL;

    if (parent->children_tail)
        parent->children_tail->sibling_next = child;
    else
        parent->children_head = child;

    parent->children_tail = child;
    parent->child_count++;
}

void proc_remove_child(process_t *child) {
    if (!child) return;

    process_t *parent = proc_find(child->parent_pid);
    if (!parent) {
        /* A child whose parent can't be found would stay linked after it is
         * freed. Never skip that silently. */
        panic("proc_remove_child: pid %u has unknown parent %u",
              child->pid, child->parent_pid);
    }

    process_t *prev = NULL;
    process_t *cur = parent->children_head;
    while (cur) {
        if (cur == child) {
            if (prev)
                prev->sibling_next = cur->sibling_next;
            else
                parent->children_head = cur->sibling_next;

            if (cur == parent->children_tail)
                parent->children_tail = prev;

            if (parent->child_count) parent->child_count--;
            cur->sibling_next = NULL;
            return;
        }
        prev = cur;
        cur = cur->sibling_next;
    }
}

void proc_reparent_orphans(process_t *dead_parent) {
    if (!dead_parent) return;

    process_t *init = proc_find(1);
    process_t *child = dead_parent->children_head;

    while (child) {
        process_t *next = child->sibling_next;
        child->sibling_next = NULL;

        if (init && init != dead_parent) {
            child->parent_pid = 1;
            if (init->children_tail)
                init->children_tail->sibling_next = child;
            else
                init->children_head = child;
            init->children_tail = child;
            init->child_count++;
        } else {
            /* No init (or init itself is dying): drop the parent link. */
            child->parent_pid = 0;
        }
        child = next;
    }

    dead_parent->children_head = NULL;
    dead_parent->children_tail = NULL;
    dead_parent->child_count = 0;
}

