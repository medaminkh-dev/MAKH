/**
 * MakhOS - test_pthread.c
 * Brutal tests for the POSIX threads layer: mutual exclusion under preemption,
 * recursive/errorcheck mutexes, condition variables (incl. timedwait), rwlock
 * invariants, barriers, once, TLS keys + destructors, semaphores, spinlocks.
 */

#include <ktest.h>
#include <pthread.h>
#include <semaphore.h>
#include <ktime.h>
#include <irq.h>
#include <errno.h>
#include <kernel.h>
#include <sched.h>
#include <mm/kheap.h>

/* -------------------------------------------------------------------------- */

static void* ret_arg(void* a) { return a; }

KTEST(pthread, create_join_returns_value) {
    pthread_t t;
    KEXPECT_EQ(pthread_create(&t, (void*)0, ret_arg, (void*)0xABCD), 0);
    void* r = (void*)0;
    KEXPECT_EQ(pthread_join(t, &r), 0);
    KEXPECT_EQ((int64_t)(uintptr_t)r, (int64_t)0xABCD);
}

/* -------------------------------------------------------------------------- */

static pthread_mutex_t mx_lock;
static volatile uint64_t mx_counter;

static void* mx_worker(void* arg) {
    uint64_t iters = (uint64_t)(long)arg;
    for (uint64_t i = 0; i < iters; i++) {
        pthread_mutex_lock(&mx_lock);
        mx_counter++;                 /* protected only by the mutex */
        pthread_mutex_unlock(&mx_lock);
    }
    return (void*)0;
}

KTEST(pthread, mutex_mutual_exclusion) {
    enum { N = 6, ITERS = 4000 };
    pthread_mutex_init(&mx_lock, (void*)0);
    mx_counter = 0;
    pthread_t t[N];
    for (int i = 0; i < N; i++)
        KEXPECT_EQ(pthread_create(&t[i], (void*)0, mx_worker, (void*)(long)ITERS), 0);
    for (int i = 0; i < N; i++) KEXPECT_EQ(pthread_join(t[i], (void*)0), 0);
    /* If the mutex failed to serialize the RMW, this would be < N*ITERS. */
    KEXPECT_EQ((int64_t)mx_counter, (int64_t)(N * ITERS));
    pthread_mutex_destroy(&mx_lock);
}

KTEST(pthread, mutex_recursive_and_errorcheck) {
    pthread_mutexattr_t a;
    pthread_mutex_t rm;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rm, &a);
    KEXPECT_EQ(pthread_mutex_lock(&rm), 0);
    KEXPECT_EQ(pthread_mutex_lock(&rm), 0);   /* recursive relock ok */
    KEXPECT_EQ(pthread_mutex_unlock(&rm), 0);
    KEXPECT_EQ(pthread_mutex_unlock(&rm), 0);
    pthread_mutex_destroy(&rm);

    pthread_mutex_t em;
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&em, &a);
    KEXPECT_EQ(pthread_mutex_lock(&em), 0);
    KEXPECT_EQ(pthread_mutex_lock(&em), EDEADLK);  /* self-relock detected */
    KEXPECT_EQ(pthread_mutex_unlock(&em), 0);
    KEXPECT_EQ(pthread_mutex_unlock(&em), EPERM);  /* not owner / not locked */
    pthread_mutex_destroy(&em);
}

KTEST(pthread, mutex_trylock_busy) {
    pthread_mutex_t m;
    pthread_mutex_init(&m, (void*)0);
    KEXPECT_EQ(pthread_mutex_trylock(&m), 0);
    KEXPECT_EQ(pthread_mutex_trylock(&m), EBUSY);
    KEXPECT_EQ(pthread_mutex_unlock(&m), 0);
    pthread_mutex_destroy(&m);
}

/* -------------------------------------------------------------------------- */
/* Condition variable: bounded buffer producer/consumer.                       */

static pthread_mutex_t cv_m;
static pthread_cond_t  cv_notempty, cv_notfull;
static int cv_buf[8], cv_head, cv_tail, cv_cnt;
static uint64_t cv_sum_in, cv_sum_out;

static void* cv_producer(void* arg) {
    uint64_t n = (uint64_t)(long)arg;
    for (uint64_t i = 1; i <= n; i++) {
        pthread_mutex_lock(&cv_m);
        while (cv_cnt == 8) pthread_cond_wait(&cv_notfull, &cv_m);
        cv_buf[cv_tail] = (int)i; cv_tail = (cv_tail + 1) % 8; cv_cnt++;
        cv_sum_in += i;
        pthread_cond_signal(&cv_notempty);
        pthread_mutex_unlock(&cv_m);
    }
    return (void*)0;
}
static void* cv_consumer(void* arg) {
    uint64_t n = (uint64_t)(long)arg;
    for (uint64_t i = 0; i < n; i++) {
        pthread_mutex_lock(&cv_m);
        while (cv_cnt == 0) pthread_cond_wait(&cv_notempty, &cv_m);
        int v = cv_buf[cv_head]; cv_head = (cv_head + 1) % 8; cv_cnt--;
        cv_sum_out += (uint64_t)v;
        pthread_cond_signal(&cv_notfull);
        pthread_mutex_unlock(&cv_m);
    }
    return (void*)0;
}

KTEST(pthread, cond_producer_consumer) {
    const uint64_t N = 3000;
    pthread_mutex_init(&cv_m, (void*)0);
    pthread_cond_init(&cv_notempty, (void*)0);
    pthread_cond_init(&cv_notfull, (void*)0);
    cv_head = cv_tail = cv_cnt = 0; cv_sum_in = cv_sum_out = 0;

    pthread_t p, c;
    KEXPECT_EQ(pthread_create(&p, (void*)0, cv_producer, (void*)(long)N), 0);
    KEXPECT_EQ(pthread_create(&c, (void*)0, cv_consumer, (void*)(long)N), 0);
    KEXPECT_EQ(pthread_join(p, (void*)0), 0);
    KEXPECT_EQ(pthread_join(c, (void*)0), 0);

    KEXPECT_EQ((int64_t)cv_sum_out, (int64_t)cv_sum_in);
    KEXPECT_EQ((int64_t)cv_sum_in, (int64_t)(N * (N + 1) / 2));
    pthread_mutex_destroy(&cv_m);
}

KTEST(pthread, cond_timedwait_times_out) {
    pthread_mutex_t m; pthread_cond_t c;
    pthread_mutex_init(&m, (void*)0);
    pthread_cond_init(&c, (void*)0);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec deadline = now;
    deadline.tv_nsec += 60 * 1000000L;            /* +60ms */
    deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;

    uint64_t t0 = clock_now_ms();
    pthread_mutex_lock(&m);
    int rc = pthread_cond_timedwait(&c, &m, &deadline);  /* nobody signals */
    pthread_mutex_unlock(&m);
    uint64_t elapsed = clock_now_ms() - t0;

    KEXPECT_EQ(rc, ETIMEDOUT);
    KEXPECT(elapsed >= 50);
    pthread_mutex_destroy(&m);
}

/* -------------------------------------------------------------------------- */
/* rwlock: readers may overlap; a writer excludes everyone.                    */

static pthread_rwlock_t rw;
static volatile int rw_readers, rw_writers, rw_violation, rw_max_readers;

static void bump(volatile int* p, int d) {
    irqflags_t f = local_irq_save(); *p += d; local_irq_restore(f);
}

static void* rw_reader(void* arg) {
    (void)arg;
    for (int i = 0; i < 200; i++) {
        pthread_rwlock_rdlock(&rw);
        bump(&rw_readers, 1);
        if (rw_writers != 0) rw_violation = 1;
        if (rw_readers > rw_max_readers) rw_max_readers = rw_readers;
        for (volatile int k = 0; k < 200; k++) { }
        bump(&rw_readers, -1);
        pthread_rwlock_unlock(&rw);
    }
    return (void*)0;
}
static void* rw_writer(void* arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        pthread_rwlock_wrlock(&rw);
        bump(&rw_writers, 1);
        if (rw_readers != 0 || rw_writers != 1) rw_violation = 1;
        for (volatile int k = 0; k < 200; k++) { }
        bump(&rw_writers, -1);
        pthread_rwlock_unlock(&rw);
    }
    return (void*)0;
}

KTEST(pthread, rwlock_reader_writer_invariants) {
    pthread_rwlock_init(&rw, (void*)0);
    rw_readers = rw_writers = rw_violation = rw_max_readers = 0;
    pthread_t r1, r2, r3, w1;
    pthread_create(&r1, (void*)0, rw_reader, (void*)0);
    pthread_create(&r2, (void*)0, rw_reader, (void*)0);
    pthread_create(&r3, (void*)0, rw_reader, (void*)0);
    pthread_create(&w1, (void*)0, rw_writer, (void*)0);
    pthread_join(r1, (void*)0); pthread_join(r2, (void*)0);
    pthread_join(r3, (void*)0); pthread_join(w1, (void*)0);
    KEXPECT_EQ(rw_violation, 0);
    pthread_rwlock_destroy(&rw);
}

/* -------------------------------------------------------------------------- */
/* Barrier: all threads meet; exactly one is the serial thread.                */

static pthread_barrier_t bar;
static volatile int bar_serial_count, bar_passed;

static void* bar_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        int rc = pthread_barrier_wait(&bar);
        if (rc == PTHREAD_BARRIER_SERIAL_THREAD) bump(&bar_serial_count, 1);
        else if (rc != 0) bump(&bar_passed, 1000000);  /* error marker */
        bump(&bar_passed, 1);
    }
    return (void*)0;
}

KTEST(pthread, barrier_releases_all) {
    enum { N = 4, ROUNDS = 20 };
    pthread_barrier_init(&bar, (void*)0, N);
    bar_serial_count = 0; bar_passed = 0;
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], (void*)0, bar_worker, (void*)0);
    for (int i = 0; i < N; i++) pthread_join(t[i], (void*)0);
    /* Exactly one serial thread per round, and everyone passed each round. */
    KEXPECT_EQ(bar_serial_count, ROUNDS);
    KEXPECT_EQ(bar_passed, N * ROUNDS);
    pthread_barrier_destroy(&bar);
}

/* -------------------------------------------------------------------------- */
/* Once: init runs exactly one time across many threads.                       */

static pthread_once_t once_ctl = PTHREAD_ONCE_INIT;
static volatile int once_runs;
static void once_init(void) { bump(&once_runs, 1); }
static void* once_worker(void* a) { (void)a; pthread_once(&once_ctl, once_init); return (void*)0; }

KTEST(pthread, once_runs_exactly_once) {
    enum { N = 8 };
    once_runs = 0;
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], (void*)0, once_worker, (void*)0);
    for (int i = 0; i < N; i++) pthread_join(t[i], (void*)0);
    KEXPECT_EQ(once_runs, 1);
}

/* -------------------------------------------------------------------------- */
/* TLS keys: per-thread isolation + destructor on exit.                        */

static pthread_key_t tkey;
static volatile int destr_calls;
static void tls_destr(void* v) { (void)v; bump(&destr_calls, 1); }

static void* tls_worker(void* arg) {
    pthread_setspecific(tkey, arg);
    /* Value we read back must be our own, not another thread's. */
    void* got = pthread_getspecific(tkey);
    return got;
}

KTEST(pthread, tls_isolation_and_destructor) {
    KEXPECT_EQ(pthread_key_create(&tkey, tls_destr), 0);
    destr_calls = 0;
    enum { N = 5 };
    pthread_t t[N];
    for (int i = 0; i < N; i++)
        pthread_create(&t[i], (void*)0, tls_worker, (void*)(long)(i + 1));
    for (int i = 0; i < N; i++) {
        void* r = (void*)0;
        pthread_join(t[i], &r);
        KEXPECT_EQ((int64_t)(uintptr_t)r, (int64_t)(i + 1));  /* got its own value */
    }
    /* Each thread had a non-NULL value, so the destructor ran N times. */
    KEXPECT_EQ(destr_calls, N);
    pthread_key_delete(tkey);
}

/* -------------------------------------------------------------------------- */
/* Semaphores as a counting resource pool.                                     */

static sem_t sem_slots;
static pthread_mutex_t sem_m;
static volatile int sem_inflight, sem_max_inflight;

static void* sem_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        sem_wait(&sem_slots);
        pthread_mutex_lock(&sem_m);
        sem_inflight++;
        if (sem_inflight > sem_max_inflight) sem_max_inflight = sem_inflight;
        pthread_mutex_unlock(&sem_m);

        for (volatile int k = 0; k < 100; k++) { }

        pthread_mutex_lock(&sem_m);
        sem_inflight--;
        pthread_mutex_unlock(&sem_m);
        sem_post(&sem_slots);
    }
    return (void*)0;
}

KTEST(pthread, semaphore_limits_concurrency) {
    enum { N = 6, LIMIT = 2 };
    sem_init(&sem_slots, 0, LIMIT);
    pthread_mutex_init(&sem_m, (void*)0);
    sem_inflight = 0; sem_max_inflight = 0;
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], (void*)0, sem_worker, (void*)0);
    for (int i = 0; i < N; i++) pthread_join(t[i], (void*)0);
    /* Never more than LIMIT threads in the critical section at once. */
    KEXPECT(sem_max_inflight >= 1);
    KEXPECT(sem_max_inflight <= LIMIT);
    sem_destroy(&sem_slots);
    pthread_mutex_destroy(&sem_m);
}

KTEST(pthread, semaphore_trywait) {
    sem_t s;
    sem_init(&s, 0, 1);
    KEXPECT_EQ(sem_trywait(&s), 0);
    KEXPECT_EQ(sem_trywait(&s), -EAGAIN);
    sem_post(&s);
    int v = -1;
    sem_getvalue(&s, &v);
    KEXPECT_EQ(v, 1);
    sem_destroy(&s);
}

/* -------------------------------------------------------------------------- */
/* Spinlock mutual exclusion.                                                  */

static pthread_spinlock_t spin;
static volatile uint64_t spin_counter;

static void* spin_worker(void* arg) {
    uint64_t iters = (uint64_t)(long)arg;
    for (uint64_t i = 0; i < iters; i++) {
        pthread_spin_lock(&spin);
        spin_counter++;
        pthread_spin_unlock(&spin);
    }
    return (void*)0;
}

KTEST(pthread, spinlock_mutual_exclusion) {
    enum { N = 4, ITERS = 3000 };
    pthread_spin_init(&spin, 0);
    spin_counter = 0;
    pthread_t t[N];
    for (int i = 0; i < N; i++)
        pthread_create(&t[i], (void*)0, spin_worker, (void*)(long)ITERS);
    for (int i = 0; i < N; i++) pthread_join(t[i], (void*)0);
    KEXPECT_EQ((int64_t)spin_counter, (int64_t)(N * ITERS));
    pthread_spin_destroy(&spin);
}

/* -------------------------------------------------------------------------- */
/* Broadcast wakes every waiter exactly once.                                  */

static pthread_mutex_t bc_m;
static pthread_cond_t  bc_c;
static volatile int bc_go, bc_waiting, bc_woken;

static void* bc_waiter(void* a) {
    (void)a;
    pthread_mutex_lock(&bc_m);
    bc_waiting++;
    while (!bc_go) pthread_cond_wait(&bc_c, &bc_m);
    bc_woken++;
    pthread_mutex_unlock(&bc_m);
    return (void*)0;
}

KTEST(pthread, cond_broadcast_wakes_all) {
    enum { N = 10 };
    pthread_mutex_init(&bc_m, (void*)0);
    pthread_cond_init(&bc_c, (void*)0);
    bc_go = 0; bc_waiting = 0; bc_woken = 0;

    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], (void*)0, bc_waiter, (void*)0);

    /* Wait until every waiter is parked on the condition. */
    for (int spins = 0; spins < 2000; spins++) {
        pthread_mutex_lock(&bc_m);
        int w = bc_waiting;
        pthread_mutex_unlock(&bc_m);
        if (w == N) break;
        sched_yield();
    }

    pthread_mutex_lock(&bc_m);
    KEXPECT_EQ(bc_waiting, N);
    bc_go = 1;
    pthread_cond_broadcast(&bc_c);
    pthread_mutex_unlock(&bc_m);

    for (int i = 0; i < N; i++) KEXPECT_EQ(pthread_join(t[i], (void*)0), 0);
    KEXPECT_EQ(bc_woken, N);
}

/* -------------------------------------------------------------------------- */
/* Joining an already-exited thread returns immediately with its value.        */

static void* quick_exit(void* a) { pthread_exit(a); }

KTEST(pthread, join_after_exit_and_retval_churn) {
    uint64_t expect = 0, got_sum = 0;
    for (int i = 1; i <= 300; i++) {
        pthread_t t;
        KASSERT_TEST(pthread_create(&t, (void*)0, quick_exit, (void*)(long)i) == 0);
        if ((i & 3) == 0) sched_yield();   /* sometimes let it exit first */
        void* r = (void*)0;
        KEXPECT_EQ(pthread_join(t, &r), 0);
        got_sum += (uint64_t)(uintptr_t)r;
        expect += (uint64_t)i;
    }
    KEXPECT_EQ((int64_t)got_sum, (int64_t)expect);
}

/* -------------------------------------------------------------------------- */
/* Detached threads are reaped by the idle thread - no leak.                   */

static void* detached_body(void* a) { (void)a; return (void*)0; }

KTEST(pthread, detached_threads_are_reaped) {
    size_t heap_before = kheap_get_used();
    size_t bad_before  = kheap_get_bad_frees();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < 50; i++) {
        pthread_t t;
        KASSERT_TEST(pthread_create(&t, &attr, detached_body, (void*)0) == 0);
    }

    /* Sleep so the detached threads finish and the idle thread reaps them. */
    for (int i = 0; i < 20 && kheap_get_used() != heap_before; i++) {
        /* Let idle run: block briefly. */
        sched_sleep_ticks(2);
    }

    KEXPECT_EQ((int64_t)kheap_get_used(), (int64_t)heap_before);
    KEXPECT_EQ((int64_t)kheap_get_bad_frees(), (int64_t)bad_before);
}

/* -------------------------------------------------------------------------- */
/* sem_timedwait honours its deadline.                                         */

KTEST(pthread, sem_timedwait_times_out) {
    sem_t s;
    sem_init(&s, 0, 0);
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_nsec += 40 * 1000000L;
    dl.tv_sec  += dl.tv_nsec / 1000000000L;
    dl.tv_nsec %= 1000000000L;
    uint64_t t0 = clock_now_ms();
    KEXPECT_EQ(sem_timedwait(&s, &dl), -ETIMEDOUT);
    KEXPECT(clock_now_ms() - t0 >= 30);
    sem_post(&s);
    KEXPECT_EQ(sem_timedwait(&s, &dl), 0);   /* value available: no wait */
    sem_destroy(&s);
}

/* -------------------------------------------------------------------------- */
/* Every API rejects NULL / invalid arguments instead of crashing.             */

KTEST(pthread, invalid_arguments_rejected) {
    KEXPECT_EQ(pthread_mutex_init((void*)0, (void*)0), EINVAL);
    KEXPECT_EQ(pthread_mutex_lock((void*)0), EINVAL);
    KEXPECT_EQ(pthread_mutex_trylock((void*)0), EINVAL);
    KEXPECT_EQ(pthread_mutex_unlock((void*)0), EINVAL);
    KEXPECT_EQ(pthread_cond_init((void*)0, (void*)0), EINVAL);
    KEXPECT_EQ(pthread_cond_wait((void*)0, (void*)0), EINVAL);
    KEXPECT_EQ(pthread_cond_signal((void*)0), EINVAL);
    KEXPECT_EQ(pthread_cond_broadcast((void*)0), EINVAL);
    KEXPECT_EQ(pthread_rwlock_rdlock((void*)0), EINVAL);
    KEXPECT_EQ(pthread_rwlock_wrlock((void*)0), EINVAL);
    KEXPECT_EQ(pthread_barrier_init((void*)0, (void*)0, 1), EINVAL);
    pthread_barrier_t b;
    KEXPECT_EQ(pthread_barrier_init(&b, (void*)0, 0), EINVAL);  /* count 0 */
    KEXPECT_EQ(pthread_once((void*)0, (void*)0), EINVAL);
    KEXPECT_EQ(pthread_key_create((void*)0, (void*)0), EINVAL);
    KEXPECT_EQ(pthread_setspecific(-1, (void*)0), EINVAL);
    KEXPECT_EQ(pthread_setspecific(PTHREAD_KEYS_MAX, (void*)0), EINVAL);
    KEXPECT((void*)pthread_getspecific(-1) == (void*)0);
    KEXPECT_EQ(pthread_join((void*)0, (void*)0), EINVAL);
    KEXPECT_EQ(pthread_attr_setdetachstate((void*)0, 0), EINVAL);
    pthread_attr_t a; pthread_attr_init(&a);
    KEXPECT_EQ(pthread_attr_setdetachstate(&a, 7), EINVAL);
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma);
    KEXPECT_EQ(pthread_mutexattr_settype(&ma, 99), EINVAL);
    KEXPECT_EQ(sem_init((void*)0, 0, 1), -EINVAL);
    KEXPECT_EQ(sem_wait((void*)0), -EINVAL);
    KEXPECT_EQ(sem_post((void*)0), -EINVAL);
    KEXPECT_EQ(pthread_spin_lock((void*)0), EINVAL);

    /* Destroying a locked mutex / held rwlock is refused. */
    pthread_mutex_t m; pthread_mutex_init(&m, (void*)0);
    pthread_mutex_lock(&m);
    KEXPECT_EQ(pthread_mutex_destroy(&m), EBUSY);
    pthread_mutex_unlock(&m);
    KEXPECT_EQ(pthread_mutex_destroy(&m), 0);
    pthread_rwlock_t rwl; pthread_rwlock_init(&rwl, (void*)0);
    pthread_rwlock_rdlock(&rwl);
    KEXPECT_EQ(pthread_rwlock_destroy(&rwl), EBUSY);
    KEXPECT_EQ(pthread_rwlock_trywrlock(&rwl), EBUSY);
    pthread_rwlock_unlock(&rwl);
    KEXPECT_EQ(pthread_rwlock_unlock(&rwl), EPERM);   /* not locked */
    KEXPECT_EQ(pthread_rwlock_destroy(&rwl), 0);
}
