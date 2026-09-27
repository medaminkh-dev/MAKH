/**
 * =============================================================================
 * pthread.c - POSIX threads API on the MakhOS scheduler
 * =============================================================================
 * Threads are kernel contexts (Phase 12). Synchronization objects are built
 * from wait queues + IRQ-off critical sections. All blocking waits recheck
 * their predicate in a loop (Mesa semantics), so spurious wakeups are safe.
 * =============================================================================
 */

#include <pthread.h>
#include <sched.h>
#include <proc.h>
#include <irq.h>
#include <errno.h>
#include <ktime.h>
#include <kernel.h>
#include <klog.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <drivers/timer.h>

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

/* Convert an absolute CLOCK_MONOTONIC deadline to a relative tick count.
 * Returns 0 if the deadline has already passed. */
static uint64_t abstime_to_ticks(const struct timespec* abstime) {
    uint64_t now = clock_now_ms();
    uint64_t dl  = timespec_to_ms(abstime);
    if (dl <= now) return 0;
    uint64_t rel_ms = dl - now;
    uint64_t ticks = (rel_ms * TIMER_FREQUENCY + 999) / 1000;
    return ticks ? ticks : 1;
}

/* -------------------------------------------------------------------------- */
/* Threads                                                                    */
/* -------------------------------------------------------------------------- */

static void pth_shim(void* arg) {
    process_t* self = proc_current();
    void* r = self->pth_start ? self->pth_start(arg) : (void*)0;
    pthread_exit(r);
}

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start)(void*), void* arg) {
    uint8_t prio = attr ? attr->priority : PRIO_DEFAULT;
    int detach   = attr ? attr->detachstate : PTHREAD_CREATE_JOINABLE;

    /* Hold off preemption so the child cannot start running pth_shim before we
     * have filled in pth_start / detach state. */
    preempt_disable();
    process_t* t = thread_create(pth_shim, arg, "pthread", prio);
    if (t) {
        t->pth_start = start;
        if (detach == PTHREAD_CREATE_DETACHED) t->detached = 1;
    }
    preempt_enable();

    if (!t) return EAGAIN;
    if (thread) *thread = t;
    return 0;
}

int pthread_join(pthread_t thread, void** retval) {
    if (!thread) return EINVAL;
    return sched_join(thread, NULL, retval) == 0 ? 0 : ESRCH;
}

int pthread_detach(pthread_t thread) {
    if (!thread) return EINVAL;
    return sched_detach(thread) == 0 ? 0 : EINVAL;
}

void pthread_exit(void* retval) {
    proc_current()->retval = retval;
    thread_exit(0);
}

pthread_t pthread_self(void)               { return proc_current(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
int pthread_yield(void)                     { thread_yield(); return 0; }
int sched_yield(void)                       { thread_yield(); return 0; }

/* -------------------------------------------------------------------------- */
/* Attributes                                                                 */
/* -------------------------------------------------------------------------- */

int pthread_attr_init(pthread_attr_t* a) {
    if (!a) return EINVAL;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->priority = PRIO_DEFAULT;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t* a) { (void)a; return 0; }

int pthread_attr_setdetachstate(pthread_attr_t* a, int state) {
    if (!a || (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED))
        return EINVAL;
    a->detachstate = state;
    return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t* a, int* state) {
    if (!a || !state) return EINVAL;
    *state = a->detachstate;
    return 0;
}
int pthread_attr_setpriority_np(pthread_attr_t* a, uint8_t prio) {
    if (!a) return EINVAL;
    a->priority = prio;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Mutex                                                                      */
/* -------------------------------------------------------------------------- */

int pthread_mutexattr_init(pthread_mutexattr_t* a) {
    if (!a) return EINVAL;
    a->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int type) {
    if (!a || type < 0 || type > PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    a->type = type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    if (!m) return EINVAL;
    m->locked = 0;
    m->owner = (void*)0;
    m->type = a ? a->type : PTHREAD_MUTEX_NORMAL;
    m->recursion = 0;
    wq_init(&m->wq);
    m->inited = 1;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    if (m->locked) return EBUSY;
    m->inited = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    process_t* self = proc_current();
    irqflags_t f = local_irq_save();
    for (;;) {
        if (!m->locked) {
            m->locked = 1;
            m->owner = self;
            m->recursion = 1;
            local_irq_restore(f);
            return 0;
        }
        if (m->owner == self) {
            if (m->type == PTHREAD_MUTEX_RECURSIVE) {
                m->recursion++;
                local_irq_restore(f);
                return 0;
            }
            /* NORMAL self-relock is a deadlock; report it rather than hang. */
            local_irq_restore(f);
            return EDEADLK;
        }
        sched_wait_event(&m->wq, 0, f);  /* block; returns IRQs restored to f */
        f = local_irq_save();
    }
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    process_t* self = proc_current();
    irqflags_t f = local_irq_save();
    if (!m->locked) {
        m->locked = 1; m->owner = self; m->recursion = 1;
        local_irq_restore(f);
        return 0;
    }
    if (m->owner == self && m->type == PTHREAD_MUTEX_RECURSIVE) {
        m->recursion++;
        local_irq_restore(f);
        return 0;
    }
    local_irq_restore(f);
    return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    process_t* self = proc_current();
    irqflags_t f = local_irq_save();
    if (!m->locked || (m->owner != self &&
        (m->type == PTHREAD_MUTEX_ERRORCHECK || m->type == PTHREAD_MUTEX_RECURSIVE))) {
        local_irq_restore(f);
        return EPERM;
    }
    if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->recursion > 0) {
        local_irq_restore(f);
        return 0;
    }
    m->recursion = 0;
    m->locked = 0;
    m->owner = (void*)0;
    wq_wake_one(&m->wq);
    local_irq_restore(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Condition variables                                                        */
/* -------------------------------------------------------------------------- */

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a) {
    (void)a;
    if (!c) return EINVAL;
    wq_init(&c->wq);
    c->inited = 1;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t* c) {
    if (!c) return EINVAL;
    c->inited = 0;
    return 0;
}

/* Internal: atomically release the mutex and block on the cond queue. */
static int cond_wait_common(pthread_cond_t* c, pthread_mutex_t* m, uint64_t ticks) {
    process_t* self = proc_current();
    irqflags_t f = local_irq_save();

    if (!m->locked || m->owner != self) {
        local_irq_restore(f);
        return EPERM;
    }

    /* Release the mutex (remember recursion to restore afterwards). */
    int saved_rec = m->recursion;
    m->recursion = 0;
    m->locked = 0;
    m->owner = (void*)0;
    wq_wake_one(&m->wq);

    int timed = sched_wait_event(&c->wq, ticks, f);  /* sleeps; IRQs restored */

    /* Reacquire the mutex before returning, per POSIX. */
    pthread_mutex_lock(m);
    if (m->type == PTHREAD_MUTEX_RECURSIVE) m->recursion = saved_rec ? saved_rec : 1;

    return timed ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    if (!c || !m) return EINVAL;
    return cond_wait_common(c, m, 0);
}

int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                           const struct timespec* abstime) {
    if (!c || !m || !abstime) return EINVAL;
    uint64_t ticks = abstime_to_ticks(abstime);
    if (ticks == 0) {
        /* Deadline already passed: behave as a zero-timeout wait. */
        return ETIMEDOUT;
    }
    return cond_wait_common(c, m, ticks);
}

int pthread_cond_signal(pthread_cond_t* c) {
    if (!c) return EINVAL;
    wq_wake_one(&c->wq);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t* c) {
    if (!c) return EINVAL;
    wq_wake_all(&c->wq);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Read/write lock (writer-preferring)                                        */
/* -------------------------------------------------------------------------- */

int pthread_rwlock_init(pthread_rwlock_t* rw, const void* a) {
    (void)a;
    if (!rw) return EINVAL;
    rw->state = 0;
    rw->waiting_writers = 0;
    wq_init(&rw->rq);
    wq_init(&rw->wq);
    rw->inited = 1;
    return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t* rw) {
    if (!rw) return EINVAL;
    if (rw->state != 0) return EBUSY;
    rw->inited = 0;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t* rw) {
    if (!rw) return EINVAL;
    irqflags_t f = local_irq_save();
    /* Wait while a writer holds it or writers are queued (avoid starvation). */
    while (rw->state < 0 || rw->waiting_writers > 0) {
        sched_wait_event(&rw->rq, 0, f);
        f = local_irq_save();
    }
    rw->state++;
    local_irq_restore(f);
    return 0;
}
int pthread_rwlock_tryrdlock(pthread_rwlock_t* rw) {
    if (!rw) return EINVAL;
    irqflags_t f = local_irq_save();
    if (rw->state < 0 || rw->waiting_writers > 0) { local_irq_restore(f); return EBUSY; }
    rw->state++;
    local_irq_restore(f);
    return 0;
}
int pthread_rwlock_wrlock(pthread_rwlock_t* rw) {
    if (!rw) return EINVAL;
    irqflags_t f = local_irq_save();
    while (rw->state != 0) {
        rw->waiting_writers++;
        sched_wait_event(&rw->wq, 0, f);
        f = local_irq_save();
        rw->waiting_writers--;
    }
    rw->state = -1;
    local_irq_restore(f);
    return 0;
}
int pthread_rwlock_trywrlock(pthread_rwlock_t* rw) {
    if (!rw) return EINVAL;
    irqflags_t f = local_irq_save();
    if (rw->state != 0) { local_irq_restore(f); return EBUSY; }
    rw->state = -1;
    local_irq_restore(f);
    return 0;
}
int pthread_rwlock_unlock(pthread_rwlock_t* rw) {
    if (!rw) return EINVAL;
    irqflags_t f = local_irq_save();
    if (rw->state < 0) {
        rw->state = 0;                 /* was write-locked */
    } else if (rw->state > 0) {
        rw->state--;                   /* drop one reader */
    } else {
        local_irq_restore(f);
        return EPERM;
    }
    if (rw->state == 0) {
        /* Prefer waking a writer; otherwise release all readers. */
        if (rw->waiting_writers > 0) wq_wake_one(&rw->wq);
        else                         wq_wake_all(&rw->rq);
    }
    local_irq_restore(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Barrier                                                                    */
/* -------------------------------------------------------------------------- */

int pthread_barrier_init(pthread_barrier_t* b, const void* a, unsigned count) {
    (void)a;
    if (!b || count == 0) return EINVAL;
    b->count = 0;
    b->total = count;
    b->generation = 0;
    wq_init(&b->wq);
    b->inited = 1;
    return 0;
}
int pthread_barrier_destroy(pthread_barrier_t* b) {
    if (!b) return EINVAL;
    b->inited = 0;
    return 0;
}
int pthread_barrier_wait(pthread_barrier_t* b) {
    if (!b) return EINVAL;
    irqflags_t f = local_irq_save();
    unsigned gen = b->generation;
    if (++b->count == b->total) {
        b->count = 0;
        b->generation++;
        wq_wake_all(&b->wq);
        local_irq_restore(f);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (gen == b->generation) {
        sched_wait_event(&b->wq, 0, f);
        f = local_irq_save();
    }
    local_irq_restore(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Once                                                                       */
/* -------------------------------------------------------------------------- */

int pthread_once(pthread_once_t* once, void (*init_routine)(void)) {
    if (!once || !init_routine) return EINVAL;
    irqflags_t f = local_irq_save();
    if (once->done) { local_irq_restore(f); return 0; }
    if (once->running) {
        while (!once->done) {
            sched_wait_event(&once->wq, 0, f);
            f = local_irq_save();
        }
        local_irq_restore(f);
        return 0;
    }
    once->running = 1;
    local_irq_restore(f);

    init_routine();

    f = local_irq_save();
    once->done = 1;
    wq_wake_all(&once->wq);
    local_irq_restore(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Thread-specific data (keys)                                                */
/* -------------------------------------------------------------------------- */

static struct {
    int   used;
    void (*destructor)(void*);
} key_table[PTHREAD_KEYS_MAX];

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    if (!key) return EINVAL;
    irqflags_t f = local_irq_save();
    for (int i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (!key_table[i].used) {
            key_table[i].used = 1;
            key_table[i].destructor = destructor;
            local_irq_restore(f);
            *key = i;
            return 0;
        }
    }
    local_irq_restore(f);
    return EAGAIN;
}
int pthread_key_delete(pthread_key_t key) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) return EINVAL;
    key_table[key].used = 0;
    key_table[key].destructor = (void*)0;
    return 0;
}
int pthread_setspecific(pthread_key_t key, const void* value) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX || !key_table[key].used) return EINVAL;
    process_t* self = proc_current();
    if (!self->tls) {
        self->tls = kcalloc(PTHREAD_KEYS_MAX, sizeof(void*));
        if (!self->tls) return ENOMEM;
    }
    self->tls[key] = (void*)value;
    return 0;
}
void* pthread_getspecific(pthread_key_t key) {
    if (key < 0 || key >= PTHREAD_KEYS_MAX) return (void*)0;
    process_t* self = proc_current();
    if (!self->tls) return (void*)0;
    return self->tls[key];
}

/* Called from thread_exit(): run destructors for this thread's set keys. */
void pthread_tls_cleanup(process_t* t) {
    if (!t || !t->tls) return;
    /* POSIX allows several iterations for destructors that set new values. */
    for (int iter = 0; iter < 4; iter++) {
        int any = 0;
        for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
            void* val = t->tls[k];
            if (val && key_table[k].used && key_table[k].destructor) {
                t->tls[k] = (void*)0;
                key_table[k].destructor(val);
                any = 1;
            }
        }
        if (!any) break;
    }
}

/* -------------------------------------------------------------------------- */
/* Spinlock (uniprocessor: mutual exclusion via preemption disable)           */
/* -------------------------------------------------------------------------- */

int pthread_spin_init(pthread_spinlock_t* s, int pshared) {
    (void)pshared;
    if (!s) return EINVAL;
    s->locked = 0;
    return 0;
}
int pthread_spin_destroy(pthread_spinlock_t* s) {
    if (!s) return EINVAL;
    return 0;
}
int pthread_spin_lock(pthread_spinlock_t* s) {
    if (!s) return EINVAL;
    /* On a single CPU, disabling preemption gives mutual exclusion without
     * the risk of spinning against a descheduled holder. */
    preempt_disable();
    s->locked = 1;
    return 0;
}
int pthread_spin_trylock(pthread_spinlock_t* s) {
    if (!s) return EINVAL;
    preempt_disable();
    if (s->locked) { preempt_enable(); return EBUSY; }
    s->locked = 1;
    return 0;
}
int pthread_spin_unlock(pthread_spinlock_t* s) {
    if (!s) return EINVAL;
    s->locked = 0;
    preempt_enable();
    return 0;
}
