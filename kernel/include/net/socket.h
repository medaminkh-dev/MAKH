/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_SOCKET_H
#define MAKHOS_SOCKET_H

#include <types.h>

/**
 * =============================================================================
 * socket.h - BSD sockets (in-kernel)
 * =============================================================================
 * POSIX-shaped API: functions return -1 and set the calling thread's errno on
 * failure (see errno.h). Addresses are struct sockaddr_in with the port and
 * address in NETWORK byte order, exactly as in POSIX.
 *
 * Deviation: there is no VFS yet, so sockets live in their own descriptor
 * space and are closed with sock_close() instead of close().
 * =============================================================================
 */

#define AF_INET        2
#define SOCK_STREAM    1
#define SOCK_DGRAM     2

#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21

#define MSG_DONTWAIT   0x40

#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7F000001u

typedef uint32_t socklen_t;

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct in_addr {
    uint32_t s_addr;            /* network byte order */
};

struct sockaddr_in {
    uint16_t       sin_family;  /* AF_INET */
    uint16_t       sin_port;    /* network byte order */
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr* addr, socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr* addr, socklen_t* len);
int     connect(int fd, const struct sockaddr* addr, socklen_t len);
ssize_t send(int fd, const void* buf, size_t len, int flags);
ssize_t recv(int fd, void* buf, size_t len, int flags);
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* to, socklen_t tolen);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* from, socklen_t* fromlen);
int     setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
int     getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int     shutdown(int fd, int how);
int     sock_close(int fd);

/* Number of open sockets (leak checks in tests). */
int     sock_open_count(void);

#endif /* MAKHOS_SOCKET_H */
