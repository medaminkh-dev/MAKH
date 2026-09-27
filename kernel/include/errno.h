#ifndef MAKHOS_ERRNO_H
#define MAKHOS_ERRNO_H

/**
 * errno.h - error numbers (Linux-compatible values) and per-thread errno.
 *
 * pthread_* return these directly (0 == success). The BSD socket API follows
 * POSIX: it returns -1 and stores the code in the calling thread's errno,
 * which lives in the thread's PCB (see __errno_location in pthread.c).
 */

#define EPERM            1   /* Operation not permitted */
#define ENOENT           2   /* No such entry */
#define ESRCH            3   /* No such process/thread */
#define EINTR            4   /* Interrupted */
#define EBADF            9   /* Bad file (socket) descriptor */
#define EAGAIN          11   /* Try again / would block */
#define EWOULDBLOCK     EAGAIN
#define ENOMEM          12   /* Out of memory */
#define EFAULT          14   /* Bad address */
#define EBUSY           16   /* Device or resource busy */
#define EINVAL          22   /* Invalid argument */
#define EMFILE          24   /* Too many open descriptors */
#define EPIPE           32   /* Broken pipe */
#define EDEADLK         35   /* Resource deadlock would occur */
#define ENOSYS          38   /* Not implemented */
#define ENOTSOCK        88   /* Not a socket */
#define EDESTADDRREQ    89   /* Destination address required */
#define EMSGSIZE        90   /* Message too long */
#define EPROTONOSUPPORT 93   /* Protocol not supported */
#define EOPNOTSUPP      95   /* Operation not supported */
#define EAFNOSUPPORT    97   /* Address family not supported */
#define EADDRINUSE      98   /* Address already in use */
#define ENETUNREACH    101   /* Network unreachable */
#define ECONNRESET     104   /* Connection reset by peer */
#define ENOBUFS        105   /* No buffer space */
#define EISCONN        106   /* Already connected */
#define ENOTCONN       107   /* Not connected */
#define ETIMEDOUT      110   /* Operation timed out */
#define ECONNREFUSED   111   /* Connection refused */
#define EHOSTUNREACH   113   /* No route to host */
#define EALREADY       114   /* Operation already in progress */
#define EINPROGRESS    115   /* Operation now in progress */

/* Per-thread errno (stored in the current thread's PCB). */
int* __errno_location(void);
#define errno (*__errno_location())

#endif /* MAKHOS_ERRNO_H */
