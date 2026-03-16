#include <proc_internal.h>
#include <kernel.h>
#include <vga.h>

/**
 * =============================================================================
 * list.c - Process List Management
 * =============================================================================
 * Manages the ready queue and all processes list with separate pointers.
 *
 * BUG FIX: ready_remove() must guard against removing a process that is
 * not in the ready queue.
 *
 * A process is NOT in the ready queue when:
 *   ready_next == NULL  AND  ready_prev == NULL  AND  head != proc
 *
 * Without this guard, removing an already-dequeued (RUNNING) process:
 *   1. Sets head = NULL  (because prev==NULL)
 *   2. Sets tail = NULL  (because next==NULL)
 *   3. Decrements count (count stays > 0)
 *   → ready_dequeue() sees count>0, does proc=head=NULL, accesses NULL->next → GPF
 * =============================================================================
 */

// -----------------------------------------------------------------------------
// ALL PROCESSES LIST (uses all_next / all_prev)
// -----------------------------------------------------------------------------

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

    terminal_writestring("[ALL] Added PID 0x");
    phex(proc->pid);
    terminal_writestring(", total processes=0x");
    phex(all_processes.count);
    terminal_writestring("\n");
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
    all_processes.count--;
}

// -----------------------------------------------------------------------------
// READY QUEUE (uses ready_next / ready_prev)
// -----------------------------------------------------------------------------

void ready_enqueue(process_t *proc) {
    if (!proc) return;

    proc->state      = PROC_READY;
    proc->ready_next = NULL;
    proc->ready_prev = ready_queue.tail;

    terminal_writestring("[READY] Enqueuing PID 0x");
    phex(proc->pid);
    terminal_writestring(", count before=0x");
    phex(ready_queue.count);
    terminal_writestring("\n");

    if (ready_queue.tail) {
        ready_queue.tail->ready_next = proc;
        ready_queue.tail = proc;
    } else {
        ready_queue.head = proc;
        ready_queue.tail = proc;
    }

    ready_queue.count++;

    terminal_writestring("[READY] Enqueued PID 0x");
    phex(proc->pid);
    terminal_writestring(", count now=0x");
    phex(ready_queue.count);
    terminal_writestring("\n");
}

void ready_remove(process_t *proc) {
    if (!proc) return;

    /*
     * GUARD: Check if the process is actually in the ready queue.
     *
     * After ready_dequeue(), a process has:
     *   ready_next = NULL
     *   ready_prev = NULL
     *
     * The same state happens for a lone element in the queue.
     * Distinguish between the two cases by checking if we are the head.
     *
     * Not in queue ↔ (next==NULL AND prev==NULL AND head!=proc)
     */
    if (proc->ready_next == NULL &&
        proc->ready_prev == NULL &&
        ready_queue.head != proc) {
        /* Already dequeued — silently ignore, do NOT touch head/tail/count */
        return;
    }

    /* Unlink from list */
    if (proc->ready_prev)
        proc->ready_prev->ready_next = proc->ready_next;
    else
        ready_queue.head = proc->ready_next;

    if (proc->ready_next)
        proc->ready_next->ready_prev = proc->ready_prev;
    else
        ready_queue.tail = proc->ready_prev;

    proc->ready_next = NULL;
    proc->ready_prev = NULL;
    ready_queue.count--;

    terminal_writestring("[READY] Removed PID 0x");
    phex(proc->pid);
    terminal_writestring(", count now=0x");
    phex(ready_queue.count);
    terminal_writestring("\n");
}

process_t *ready_dequeue(void) {
    if (ready_queue.count == 0)
        return NULL;

    process_t *proc = ready_queue.head;

    ready_queue.head = proc->ready_next;
    if (ready_queue.head)
        ready_queue.head->ready_prev = NULL;
    else
        ready_queue.tail = NULL;

    proc->ready_next = NULL;
    proc->ready_prev = NULL;
    ready_queue.count--;

    terminal_writestring("[READY] Dequeued PID 0x");
    phex(proc->pid);
    terminal_writestring(", count now=0x");
    phex(ready_queue.count);
    terminal_writestring("\n");

    return proc;
}