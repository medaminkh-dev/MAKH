/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include <proc_internal.h>
#include <kernel.h>

/**
 * =============================================================================
 * list.c - Global "all processes" list
 * =============================================================================
 * The per-priority run queues moved into the scheduler (sched.c) in Phase 12.
 * This file now only maintains the doubly linked list of every live process
 * (linked via all_next/all_prev), used for lookup and iteration.
 * =============================================================================
 */

void all_list_add(process_t *proc) {
    if (!proc) return;

    proc->all_next = NULL;
    proc->all_prev = all_processes.tail;

    if (all_processes.tail)
        all_processes.tail->all_next = proc;
    else
        all_processes.head = proc;

    all_processes.tail = proc;
    all_processes.count++;
}

void all_list_remove(process_t *proc) {
    if (!proc) return;

    if (proc->all_prev)
        proc->all_prev->all_next = proc->all_next;
    else
        all_processes.head = proc->all_next;

    if (proc->all_next)
        proc->all_next->all_prev = proc->all_prev;
    else
        all_processes.tail = proc->all_prev;

    proc->all_next = NULL;
    proc->all_prev = NULL;
    if (all_processes.count) all_processes.count--;
}
