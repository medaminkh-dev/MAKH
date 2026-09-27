#ifndef MAKHOS_SEMAPHORE_H
#define MAKHOS_SEMAPHORE_H

#include <types.h>
#include <proc.h>
#include <ktime.h>

/**
 * semaphore.h - POSIX counting semaphores on the scheduler's wait queues.
 * Functions return 0 on success, or -1 with errno-style negative encoding via
 * the return of sem_* helpers documented below (0 / -EXXX).
 */

typedef struct sem {
    volatile int value;
    wait_queue_t wq;
    int          inited;
} sem_t;

int sem_init(sem_t* s, int pshared, unsigned value);
int sem_destroy(sem_t* s);
int sem_wait(sem_t* s);                 /* blocks until value > 0 */
int sem_trywait(sem_t* s);              /* 0, or -EAGAIN if would block */
int sem_timedwait(sem_t* s, const struct timespec* abstime); /* -ETIMEDOUT */
int sem_post(sem_t* s);
int sem_getvalue(sem_t* s, int* out);

#endif /* MAKHOS_SEMAPHORE_H */
