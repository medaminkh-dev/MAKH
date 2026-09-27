#ifndef MAKHOS_PTHREAD_H
#define MAKHOS_PTHREAD_H

#include <types.h>
#include <proc.h>
#include <ktime.h>

/**
 * =============================================================================
 * pthread.h - POSIX threads API (in-kernel, ring 0)
 * =============================================================================
 * MakhOS threads are kernel contexts sharing one address space, so this is a
 * faithful pthreads surface implemented directly on the Phase 12 scheduler and
 * wait queues. Functions return 0 on success or a positive errno on failure,
 * exactly like glibc's pthread_*.
 *
 * Timed variants take an ABSOLUTE deadline from CLOCK_MONOTONIC (see ktime.h).
 * =============================================================================
 */

typedef struct process* pthread_t;

/* ---- attributes ---- */
#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

typedef struct {
    int      detachstate;
    uint8_t  priority;
} pthread_attr_t;

/* ---- mutex ---- */
#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_ERRORCHECK  2

typedef struct { int type; } pthread_mutexattr_t;

typedef struct pthread_mutex {
    volatile int  locked;
    process_t*    owner;
    int           type;
    int           recursion;
    wait_queue_t  wq;
    int           inited;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, PTHREAD_MUTEX_NORMAL, 0, {0,0}, 1 }

/* ---- condition variable ---- */
typedef struct { int dummy; } pthread_condattr_t;

typedef struct pthread_cond {
    wait_queue_t wq;
    int          inited;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { {0,0}, 1 }

/* ---- rwlock ---- */
typedef struct pthread_rwlock {
    int          state;            /* 0 free, >0 reader count, -1 writer */
    int          waiting_writers;
    wait_queue_t rq;               /* blocked readers */
    wait_queue_t wq;               /* blocked writers */
    int          inited;
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER { 0, 0, {0,0}, {0,0}, 1 }

/* ---- barrier ---- */
#define PTHREAD_BARRIER_SERIAL_THREAD 1

typedef struct pthread_barrier {
    unsigned     count;
    unsigned     total;
    unsigned     generation;
    wait_queue_t wq;
    int          inited;
} pthread_barrier_t;

/* ---- once ---- */
typedef struct pthread_once {
    volatile int done;
    volatile int running;
    wait_queue_t wq;
} pthread_once_t;

#define PTHREAD_ONCE_INIT { 0, 0, {0,0} }

/* ---- keys / TLS ---- */
#define PTHREAD_KEYS_MAX 64
typedef int pthread_key_t;

/* ---- spinlock (uniprocessor: disables preemption) ---- */
typedef struct { int locked; } pthread_spinlock_t;

/* ============================ API ============================ */

/* threads */
int  pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                    void* (*start)(void*), void* arg);
int  pthread_join(pthread_t thread, void** retval);
int  pthread_detach(pthread_t thread);
void pthread_exit(void* retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);
int  pthread_yield(void);
int  sched_yield(void);

/* attrs */
int  pthread_attr_init(pthread_attr_t* attr);
int  pthread_attr_destroy(pthread_attr_t* attr);
int  pthread_attr_setdetachstate(pthread_attr_t* attr, int state);
int  pthread_attr_getdetachstate(const pthread_attr_t* attr, int* state);
int  pthread_attr_setpriority_np(pthread_attr_t* attr, uint8_t prio);

/* mutex */
int  pthread_mutexattr_init(pthread_mutexattr_t* a);
int  pthread_mutexattr_settype(pthread_mutexattr_t* a, int type);
int  pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a);
int  pthread_mutex_destroy(pthread_mutex_t* m);
int  pthread_mutex_lock(pthread_mutex_t* m);
int  pthread_mutex_trylock(pthread_mutex_t* m);
int  pthread_mutex_unlock(pthread_mutex_t* m);

/* condition variables */
int  pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a);
int  pthread_cond_destroy(pthread_cond_t* c);
int  pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int  pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                            const struct timespec* abstime);
int  pthread_cond_signal(pthread_cond_t* c);
int  pthread_cond_broadcast(pthread_cond_t* c);

/* rwlock */
int  pthread_rwlock_init(pthread_rwlock_t* rw, const void* a);
int  pthread_rwlock_destroy(pthread_rwlock_t* rw);
int  pthread_rwlock_rdlock(pthread_rwlock_t* rw);
int  pthread_rwlock_tryrdlock(pthread_rwlock_t* rw);
int  pthread_rwlock_wrlock(pthread_rwlock_t* rw);
int  pthread_rwlock_trywrlock(pthread_rwlock_t* rw);
int  pthread_rwlock_unlock(pthread_rwlock_t* rw);

/* barrier */
int  pthread_barrier_init(pthread_barrier_t* b, const void* a, unsigned count);
int  pthread_barrier_destroy(pthread_barrier_t* b);
int  pthread_barrier_wait(pthread_barrier_t* b);

/* once */
int  pthread_once(pthread_once_t* once, void (*init_routine)(void));

/* keys / TLS */
int    pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int    pthread_key_delete(pthread_key_t key);
int    pthread_setspecific(pthread_key_t key, const void* value);
void*  pthread_getspecific(pthread_key_t key);

/* spinlock */
int  pthread_spin_init(pthread_spinlock_t* s, int pshared);
int  pthread_spin_destroy(pthread_spinlock_t* s);
int  pthread_spin_lock(pthread_spinlock_t* s);
int  pthread_spin_trylock(pthread_spinlock_t* s);
int  pthread_spin_unlock(pthread_spinlock_t* s);

/* Called by the scheduler when a thread exits, to run TLS destructors. */
void pthread_tls_cleanup(process_t* t);

#endif /* MAKHOS_PTHREAD_H */
