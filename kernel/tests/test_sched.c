/**
 * MakhOS - test_sched.c
 * Brutal tests for the Phase 12 preemptive scheduler:
 * thread create/join, preemption, fairness, sleep, wait queues, and churn
 * without leaks. These run as the init thread with preemption live.
 */

#include <ktest.h>
#include <sched.h>
#include <proc.h>
#include <irq.h>
#include <kernel.h>
#include <mm/kheap.h>
#include <drivers/timer.h>

/* -------- shared state for worker threads -------- */

static volatile uint64_t g_counter;
static volatile int      g_stop;

/* Atomic-on-uniprocessor increment: guard the RMW with IF cleared. */
static void atomic_add(volatile uint64_t* p, uint64_t v) {
    irqflags_t f = local_irq_save();
    *p += v;
    local_irq_restore(f);
}

/* -------------------------------------------------------------------------- */

static void worker_exit_code(void* arg) {
    thread_exit((int)(long)arg);
}

KTEST(sched, join_returns_exit_code) {
    process_t* t = thread_create(worker_exit_code, (void*)(long)123,
                                 "exit123", PRIO_DEFAULT);
    KASSERT_TEST(t != (void*)0);
    int code = -1;
    int rc = thread_join(t, &code);
    KEXPECT_EQ(rc, 0);
    KEXPECT_EQ(code, 123);
}

/* -------------------------------------------------------------------------- */

static void worker_count(void* arg) {
    uint64_t iters = (uint64_t)(long)arg;
    for (uint64_t i = 0; i < iters; i++) {
        atomic_add(&g_counter, 1);
        if ((i & 0x3FF) == 0) thread_yield();  /* interleave with peers */
    }
    thread_exit(0);
}

KTEST(sched, many_threads_all_make_progress) {
    enum { N = 8, ITERS = 5000 };
    g_counter = 0;
    process_t* t[N];
    for (int i = 0; i < N; i++) {
        t[i] = thread_create(worker_count, (void*)(long)ITERS, "count", PRIO_DEFAULT);
        KASSERT_TEST(t[i] != (void*)0);
    }
    for (int i = 0; i < N; i++) KEXPECT_EQ(thread_join(t[i], (void*)0), 0);
    /* Every increment from every thread must be accounted for. */
    KEXPECT_EQ((int64_t)g_counter, (int64_t)(N * ITERS));
}

/* -------------------------------------------------------------------------- */

static void worker_spin(void* arg) {
    (void)arg;
    while (!g_stop) {
        atomic_add(&g_counter, 1);
    }
    thread_exit(0);
}

KTEST(sched, preemption_runs_a_busy_thread_while_we_sleep) {
    /* A CPU-bound thread that never yields must still make progress while the
     * init thread sleeps - that only happens if the timer preempts it. */
    g_counter = 0;
    g_stop = 0;
    process_t* t = thread_create(worker_spin, (void*)0, "spin", PRIO_DEFAULT);
    KASSERT_TEST(t != (void*)0);

    sched_sleep_ticks(10);              /* ~100ms of real time */
    uint64_t seen = g_counter;
    KEXPECT(seen > 0);                  /* it ran without ever yielding */

    g_stop = 1;
    KEXPECT_EQ(thread_join(t, (void*)0), 0);
}

/* -------------------------------------------------------------------------- */

KTEST(sched, sleep_waits_at_least_requested) {
    uint64_t t0 = timer_get_ticks();
    sched_sleep_ticks(15);
    uint64_t elapsed = timer_get_ticks() - t0;
    KEXPECT(elapsed >= 15);
    KEXPECT(elapsed < 15 + 50);         /* generous upper bound */
}

/* -------------------------------------------------------------------------- */
/* Producer/consumer over a wait queue + ring buffer.                          */

static wait_queue_t pc_not_empty;
static volatile int pc_buf[16];
static volatile int pc_head, pc_tail, pc_count;
static volatile uint64_t pc_sum_produced, pc_sum_consumed;
static volatile int pc_done_producing;

static void producer(void* arg) {
    uint64_t n = (uint64_t)(long)arg;
    for (uint64_t i = 1; i <= n; i++) {
        irqflags_t f = local_irq_save();
        while (pc_count == 16) { local_irq_restore(f); thread_yield(); f = local_irq_save(); }
        pc_buf[pc_tail] = (int)i;
        pc_tail = (pc_tail + 1) % 16;
        pc_count++;
        pc_sum_produced += i;
        local_irq_restore(f);
        wq_wake_one(&pc_not_empty);
    }
    pc_done_producing = 1;
    wq_wake_all(&pc_not_empty);
    thread_exit(0);
}

static void consumer(void* arg) {
    (void)arg;
    for (;;) {
        irqflags_t f = local_irq_save();
        while (pc_count == 0) {
            if (pc_done_producing) { local_irq_restore(f); thread_exit(0); }
            wq_block(&pc_not_empty, f);   /* releases IRQs, re-enters with them held-off logic */
            f = local_irq_save();
        }
        int v = pc_buf[pc_head];
        pc_head = (pc_head + 1) % 16;
        pc_count--;
        pc_sum_consumed += (uint64_t)v;
        local_irq_restore(f);
    }
}

KTEST(sched, producer_consumer_wait_queue) {
    wq_init(&pc_not_empty);
    pc_head = pc_tail = pc_count = 0;
    pc_sum_produced = pc_sum_consumed = 0;
    pc_done_producing = 0;

    const uint64_t N = 2000;
    process_t* prod = thread_create(producer, (void*)(long)N, "producer", PRIO_DEFAULT);
    process_t* cons = thread_create(consumer, (void*)0, "consumer", PRIO_DEFAULT);
    KASSERT_TEST(prod && cons);

    KEXPECT_EQ(thread_join(prod, (void*)0), 0);
    KEXPECT_EQ(thread_join(cons, (void*)0), 0);

    /* Nothing lost or duplicated: consumed exactly what was produced. */
    KEXPECT_EQ((int64_t)pc_sum_consumed, (int64_t)pc_sum_produced);
    KEXPECT_EQ((int64_t)pc_sum_produced, (int64_t)(N * (N + 1) / 2));
}

/* -------------------------------------------------------------------------- */

static void worker_noop(void* arg) {
    thread_exit((int)(long)arg);
}

KTEST(sched, create_join_churn_no_leak) {
    /* Repeatedly create and join far more threads than the table holds. If
     * PIDs, PCBs or stacks leaked, this would exhaust the table or the heap. */
    size_t heap_before = kheap_get_used();

    for (int i = 0; i < 1000; i++) {
        process_t* t = thread_create(worker_noop, (void*)(long)i, "churn", PRIO_DEFAULT);
        KASSERT_TEST(t != (void*)0);
        int code = -1;
        KEXPECT_EQ(thread_join(t, &code), 0);
        KEXPECT_EQ(code, i & 0x7FFFFFFF);
    }

    size_t heap_after = kheap_get_used();
    /* Heap use must return to baseline (every stack + PCB was reclaimed). */
    KEXPECT_EQ((int64_t)heap_after, (int64_t)heap_before);
}

/* -------------------------------------------------------------------------- */

static volatile int prio_order[3];
static volatile int prio_idx;

static void prio_worker(void* arg) {
    int id = (int)(long)arg;
    irqflags_t f = local_irq_save();
    if (prio_idx < 3) prio_order[prio_idx++] = id;
    local_irq_restore(f);
    thread_exit(0);
}

KTEST(sched, higher_priority_runs_first) {
    /* Create three ready threads at distinct priorities while we hold the CPU,
     * then release it. The scheduler must pick the highest priority first. */
    prio_idx = 0;
    preempt_disable();   /* keep them all merely READY until we yield */
    process_t* lo  = thread_create(prio_worker, (void*)(long)3, "lo",  PRIO_DEFAULT + 4);
    process_t* mid = thread_create(prio_worker, (void*)(long)2, "mid", PRIO_DEFAULT);
    process_t* hi  = thread_create(prio_worker, (void*)(long)1, "hi",  PRIO_DEFAULT - 4);
    KASSERT_TEST(lo && mid && hi);
    preempt_enable();

    KEXPECT_EQ(thread_join(hi,  (void*)0), 0);
    KEXPECT_EQ(thread_join(mid, (void*)0), 0);
    KEXPECT_EQ(thread_join(lo,  (void*)0), 0);

    /* First to run was the highest priority (id 1). */
    KEXPECT_EQ(prio_order[0], 1);
}
