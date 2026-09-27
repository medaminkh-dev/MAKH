/**
 * MakhOS - sem.c
 * POSIX counting semaphores on scheduler wait queues.
 * Return 0 on success or -errno on failure.
 */

#include <semaphore.h>
#include <sched.h>
#include <irq.h>
#include <errno.h>
#include <ktime.h>
#include <drivers/timer.h>

static uint64_t abstime_to_ticks(const struct timespec* abstime) {
    uint64_t now = clock_now_ms();
    uint64_t dl  = timespec_to_ms(abstime);
    if (dl <= now) return 0;
    uint64_t rel_ms = dl - now;
    uint64_t ticks = (rel_ms * TIMER_FREQUENCY + 999) / 1000;
    return ticks ? ticks : 1;
}

int sem_init(sem_t* s, int pshared, unsigned value) {
    (void)pshared;
    if (!s) return -EINVAL;
    s->value = (int)value;
    wq_init(&s->wq);
    s->inited = 1;
    return 0;
}

int sem_destroy(sem_t* s) {
    if (!s) return -EINVAL;
    s->inited = 0;
    return 0;
}

int sem_wait(sem_t* s) {
    if (!s) return -EINVAL;
    irqflags_t f = local_irq_save();
    while (s->value == 0) {
        sched_wait_event(&s->wq, 0, f);
        f = local_irq_save();
    }
    s->value--;
    local_irq_restore(f);
    return 0;
}

int sem_trywait(sem_t* s) {
    if (!s) return -EINVAL;
    irqflags_t f = local_irq_save();
    if (s->value == 0) { local_irq_restore(f); return -EAGAIN; }
    s->value--;
    local_irq_restore(f);
    return 0;
}

int sem_timedwait(sem_t* s, const struct timespec* abstime) {
    if (!s || !abstime) return -EINVAL;
    irqflags_t f = local_irq_save();
    while (s->value == 0) {
        uint64_t ticks = abstime_to_ticks(abstime);
        if (ticks == 0) { local_irq_restore(f); return -ETIMEDOUT; }
        int timed = sched_wait_event(&s->wq, ticks, f);
        f = local_irq_save();
        if (timed && s->value == 0) { local_irq_restore(f); return -ETIMEDOUT; }
    }
    s->value--;
    local_irq_restore(f);
    return 0;
}

int sem_post(sem_t* s) {
    if (!s) return -EINVAL;
    irqflags_t f = local_irq_save();
    s->value++;
    wq_wake_one(&s->wq);
    local_irq_restore(f);
    return 0;
}

int sem_getvalue(sem_t* s, int* out) {
    if (!s || !out) return -EINVAL;
    irqflags_t f = local_irq_save();
    *out = s->value;
    local_irq_restore(f);
    return 0;
}
