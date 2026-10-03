/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include <proc_internal.h>
#include <mm/kheap.h>
#include <kernel.h>
#include <vga.h>
#include <lib/string.h>
#include <irq.h>
#include <klog.h>

/**
 * =============================================================================
 * table.c - Process Table Management
 * =============================================================================
 * Manages the process table array for O(1) process lookup.
 * Supports up to MAX_PROCESSES (256) concurrent processes.
 * =============================================================================
 */

#define MAX_PROCESSES 256

static process_t* process_table[MAX_PROCESSES];
static uint32_t process_count = 0;

void proc_table_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i] = NULL;
    }
    process_count = 0;
    
    terminal_writestring("[TABLE] Process table initialized, max=");
    phex(MAX_PROCESSES);
    terminal_writestring("\n");
}

process_t* proc_table_alloc(void) {
    /* Phase 12+: threads are created concurrently under preemption, so the
     * slot search + claim must be atomic (IRQs off), or two creators can
     * claim the same slot and one PCB silently drops out of the table. */
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) {
        terminal_writestring("[TABLE] ERROR: kmalloc failed\n");
        return NULL;
    }

    irqflags_t f = local_irq_save();
    if (process_count < MAX_PROCESSES) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (process_table[i] == NULL) {
                process_table[i] = proc;
                process_count++;
                local_irq_restore(f);
                return proc;
            }
        }
    }
    local_irq_restore(f);

    kfree(proc);
    terminal_writestring("[TABLE] ERROR: Process table full\n");
    return NULL;
}

/*
 * proc_table_insert - register a statically allocated PCB (idle, init).
 *
 * Every live thread MUST be in the table: proc_find() is how the tree code
 * resolves parent_pid, and a parent it cannot find silently skips the unlink.
 * init used to be missing, so each exiting child of init stayed linked in
 * init's children list after being freed; the next proc_add_child() wrote
 * through the dangling children_tail into whatever reused that memory (a TCP
 * control block, found by the Phase 14 net stress tests + a DR1 watchpoint).
 */
int proc_table_insert(process_t* proc) {
    if (!proc) return -1;
    irqflags_t f = local_irq_save();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) {
            process_table[i] = proc;
            process_count++;
            local_irq_restore(f);
            return 0;
        }
    }
    local_irq_restore(f);
    return -1;
}

void proc_table_free(process_t* proc) {
    if (!proc) return;

    irqflags_t f = local_irq_save();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == proc) {
            process_table[i] = NULL;
            process_count--;
            local_irq_restore(f);
            kfree(proc);
            return;
        }
    }
    local_irq_restore(f);
}

process_t* proc_find(int32_t pid) {
    if (pid < 0 || pid >= PID_MAX) return NULL;

    irqflags_t f = local_irq_save();
    process_t* found = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] && (int32_t)process_table[i]->pid == pid) {
            found = process_table[i];
            break;
        }
    }
    local_irq_restore(f);
    return found;
}

uint32_t proc_get_count(void) {
    return process_count;
}

/* Debug: dump the process table (independent of the all_processes list). */
void proc_table_dump(void) {
    static const char* st[] = { "EMBRYO", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };
    irqflags_t f = local_irq_save();
    kprintf("    table count=%u\n", process_count);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table[i];
        if (!p) continue;
        kprintf("    slot %d pid=%u %s %s all_prev=%p all_next=%p self=%p\n", i, p->pid,
                p->name, st[p->state], (void*)p->all_prev, (void*)p->all_next, (void*)p);
    }
    local_irq_restore(f);
}

/* Is `p` a live table entry? (IRQs must already be off.) */
static int in_table(const process_t* p) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (process_table[i] == p) return 1;
    return 0;
}

/*
 * proc_tree_check - invariant oracle for the process tree.
 *
 * Every child reachable from a parent's list must be a live table entry whose
 * parent_pid names that parent; children_tail must be the last node and
 * child_count must match. A dangling child pointer here is a use-after-free
 * waiting to happen (proc_add_child writes through children_tail).
 *
 * Returns 0 when consistent, else 1 + the table slot of the first bad parent.
 */
int proc_tree_check(void) {
    irqflags_t f = local_irq_save();
    int bad = 0;
    for (int i = 0; i < MAX_PROCESSES && !bad; i++) {
        process_t* p = process_table[i];
        if (!p) continue;
        uint32_t n = 0;
        process_t* last = NULL;
        for (process_t* c = p->children_head; c; c = c->sibling_next) {
            if (!in_table(c) || c->parent_pid != p->pid || ++n > MAX_PROCESSES) {
                bad = i + 1;
                break;
            }
            last = c;
        }
        if (!bad && (last != p->children_tail || n != p->child_count)) bad = i + 1;
    }
    local_irq_restore(f);
    return bad;
}
