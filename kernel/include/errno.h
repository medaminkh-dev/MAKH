#ifndef MAKHOS_ERRNO_H
#define MAKHOS_ERRNO_H

/**
 * errno.h - error numbers (Linux-compatible values).
 * The POSIX threads layer returns these directly (0 == success), matching
 * pthread_* semantics.
 */

#define EPERM        1   /* Operation not permitted */
#define ESRCH        3   /* No such process/thread */
#define EINTR        4   /* Interrupted */
#define EAGAIN      11   /* Try again / resource temporarily unavailable */
#define ENOMEM      12   /* Out of memory */
#define EBUSY       16   /* Device or resource busy */
#define EINVAL      22   /* Invalid argument */
#define EDEADLK     35   /* Resource deadlock would occur */
#define ETIMEDOUT  110   /* Operation timed out */

#endif /* MAKHOS_ERRNO_H */
