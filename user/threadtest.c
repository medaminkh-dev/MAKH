/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* clone + futex + join: a worker thread and main each bump a shared counter
 * 100000 times under a futex mutex. If sharing, mutual exclusion, and join all
 * work, the counter is exactly 200000 and we exit 55. */
#include "usys.h"

static volatile int counter = 0;
static volatile int lock = 0;        /* 0 = free, 1 = held */
static volatile int tid_word = 0;    /* set to tid on clone, cleared on exit */
static char tstack[8192] __attribute__((aligned(16)));

static void flock(void) {
    while (__sync_lock_test_and_set(&lock, 1))    /* atomic xchg; old value */
        ufutex(&lock, FUTEX_WAIT, 1, 0);          /* contended: sleep */
}
static void funlock(void) {
    __sync_lock_release(&lock);                   /* lock = 0 */
    ufutex(&lock, FUTEX_WAKE, 1, 0);              /* wake one waiter */
}

#define ITERS 20000

static void worker(void* arg) {
    (void)arg;
    for (int i = 0; i < ITERS; i++) { flock(); counter++; funlock(); }
}

int umain(void) {
    long tid = __clone_thread(worker, tstack + sizeof(tstack), 0, (int*)&tid_word,
                              CLONE_VM | CLONE_FILES |
                              CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID);
    if (tid <= 0) return 1;

    for (int i = 0; i < ITERS; i++) { flock(); counter++; funlock(); }

    while (tid_word != 0)                           /* join: wait for CLEARTID */
        ufutex(&tid_word, FUTEX_WAIT, tid_word, 0);

    return (counter == 2 * ITERS) ? 55 : 2;         /* 2 => a lost update (race) */
}
