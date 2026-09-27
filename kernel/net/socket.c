/**
 * MakhOS - net/socket.c
 * BSD socket layer: descriptor table, address/port management, and the POSIX
 * entry points. Every call takes net_lock for its duration (blocking waits
 * release it via net_wait) and reports failures through errno.
 */

#include "sock.h"
#include <errno.h>
#include <ktime.h>
#include <mm/kheap.h>
#include <lib/string.h>

static sock_t socks[SOCK_MAX];
static uint16_t next_ephemeral = 49152;

#define FAIL(e) do { errno = (e); return -1; } while (0)

/* ---------------------------------------------------------------- table */

sock_t* sock_alloc(int type) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!socks[i].used) {
            sock_t* s = &socks[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->type = type;
            pthread_cond_init(&s->cond, NULL);
            return s;
        }
    }
    return NULL;
}

int sock_fd(sock_t* s) { return (int)(s - socks); }

static sock_t* get(int fd) {
    if (fd < 0 || fd >= SOCK_MAX || !socks[fd].used) return NULL;
    return &socks[fd];
}

int sock_open_count(void) {
    int n = 0;
    net_lock();
    for (int i = 0; i < SOCK_MAX; i++) n += socks[i].used;
    net_unlock();
    return n;
}

int sock_port_in_use(int type, uint32_t ip, uint16_t port) {
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t* s = &socks[i];
        if (!s->used || !s->bound || s->type != type || s->lport != port) continue;
        if (s->lip == 0 || ip == 0 || s->lip == ip) return 1;
    }
    return 0;
}

uint16_t sock_ephemeral_port(int type) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_ephemeral++;
        if (next_ephemeral == 0) next_ephemeral = 49152;
        if (!sock_port_in_use(type, 0, p)) return p;
    }
    return 0;
}

sock_t* udp_lookup(uint32_t dst_ip, uint16_t dst_port) {
    sock_t* wildcard = NULL;
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->type != SOCK_DGRAM || !s->bound || s->lport != dst_port) continue;
        if (s->lip == dst_ip) return s;          /* exact binding wins */
        if (s->lip == 0) wildcard = s;
    }
    return wildcard;
}

static int parse_addr(const struct sockaddr* a, socklen_t len, uint32_t* ip, uint16_t* port) {
    if (!a || len < sizeof(struct sockaddr_in)) return -EINVAL;
    const struct sockaddr_in* in = (const struct sockaddr_in*)a;
    if (in->sin_family != AF_INET) return -EAFNOSUPPORT;
    *ip = ntohl(in->sin_addr.s_addr);
    *port = ntohs(in->sin_port);
    return 0;
}

static void fill_addr(struct sockaddr* a, socklen_t* len, uint32_t ip, uint16_t port) {
    if (!a) return;
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    in.sin_addr.s_addr = htonl(ip);
    size_t n = sizeof(in);
    if (len && *len < n) n = *len;
    memcpy(a, &in, n);
    if (len) *len = sizeof(in);
}

/* ---------------------------------------------------------------- API */

int socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) FAIL(EAFNOSUPPORT);
    if (type != SOCK_STREAM && type != SOCK_DGRAM) FAIL(EPROTONOSUPPORT);
    net_lock();
    sock_t* s = sock_alloc(type);
    int fd = s ? sock_fd(s) : -1;
    net_unlock();
    if (fd < 0) FAIL(EMFILE);
    return fd;
}

int bind(int fd, const struct sockaddr* addr, socklen_t len) {
    uint32_t ip; uint16_t port;
    int rc = parse_addr(addr, len, &ip, &port);
    if (rc < 0) FAIL(-rc);

    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }
    if (s->bound) { net_unlock(); FAIL(EINVAL); }
    if (port == 0) port = sock_ephemeral_port(s->type);
    if (port == 0 || (!s->reuseaddr && sock_port_in_use(s->type, ip, port))) {
        net_unlock();
        FAIL(EADDRINUSE);
    }
    s->lip = ip;
    s->lport = port;
    s->bound = 1;
    net_unlock();
    return 0;
}

int listen(int fd, int backlog) {
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }
    if (s->type != SOCK_STREAM) { net_unlock(); FAIL(EOPNOTSUPP); }
    if (!s->bound) {
        s->lport = sock_ephemeral_port(SOCK_STREAM);
        s->bound = 1;
    }
    int rc = tcp_listen(s, backlog);
    net_unlock();
    if (rc < 0) FAIL(-rc);
    return 0;
}

int accept(int fd, struct sockaddr* addr, socklen_t* len) {
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }
    if (s->type != SOCK_STREAM) { net_unlock(); FAIL(EOPNOTSUPP); }
    uint32_t rip = 0; uint16_t rport = 0;
    int nfd = tcp_accept(s, &rip, &rport);
    if (nfd >= 0) fill_addr(addr, len, rip, rport);
    net_unlock();
    if (nfd < 0) FAIL(-nfd);
    return nfd;
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
    uint32_t ip; uint16_t port;
    int rc = parse_addr(addr, len, &ip, &port);
    if (rc < 0) FAIL(-rc);

    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }

    if (s->type == SOCK_DGRAM) {
        /* UDP connect just fixes the default destination. */
        if (!s->bound) {
            s->lport = sock_ephemeral_port(SOCK_DGRAM);
            s->bound = 1;
        }
        s->rip = ip; s->rport = port; s->connected = 1;
        net_unlock();
        return 0;
    }

    if (port == 0 || ip == 0) { net_unlock(); FAIL(EINVAL); }
    rc = tcp_connect(s, ip, port);
    net_unlock();
    if (rc < 0) FAIL(-rc);
    return 0;
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* to, socklen_t tolen) {
    if (!buf && len) FAIL(EFAULT);
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }

    if (s->type == SOCK_STREAM) {
        long n = tcp_send(s, (const uint8_t*)buf, len, flags & MSG_DONTWAIT);
        net_unlock();
        if (n < 0) FAIL((int)-n);
        return n;
    }

    /* UDP */
    uint32_t ip; uint16_t port;
    if (to) {
        int rc = parse_addr(to, tolen, &ip, &port);
        if (rc < 0) { net_unlock(); FAIL(-rc); }
    } else if (s->connected) {
        ip = s->rip; port = s->rport;
    } else {
        net_unlock();
        FAIL(EDESTADDRREQ);
    }
    if (len > ETH_MTU - sizeof(ipv4_hdr_t) - sizeof(udp_hdr_t)) { net_unlock(); FAIL(EMSGSIZE); }
    if (!s->bound) {
        s->lport = sock_ephemeral_port(SOCK_DGRAM);
        s->bound = 1;
    }
    int rc = udp_output(s->lip, s->lport, ip, port, buf, len);
    net_unlock();
    if (rc < 0) FAIL(-rc);
    return (ssize_t)len;
}

ssize_t send(int fd, const void* buf, size_t len, int flags) {
    return sendto(fd, buf, len, flags, NULL, 0);
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* from, socklen_t* fromlen) {
    if (!buf && len) FAIL(EFAULT);
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }

    if (s->type == SOCK_STREAM) {
        long n = tcp_recv(s, (uint8_t*)buf, len, flags & MSG_DONTWAIT);
        if (n >= 0 && s->tcb) fill_addr(from, fromlen, s->tcb->rip, s->tcb->rport);
        net_unlock();
        if (n < 0) FAIL((int)-n);
        return n;
    }

    /* UDP: wait for a datagram. */
    uint64_t deadline = s->rcvtimeo_ms ? clock_now_ms() + s->rcvtimeo_ms : 0;
    while (!s->dq_head) {
        if (flags & MSG_DONTWAIT) { net_unlock(); FAIL(EAGAIN); }
        if (s->shut_rd) { net_unlock(); return 0; }
        uint64_t wait = 0;
        if (deadline) {
            uint64_t now = clock_now_ms();
            if (now >= deadline) { net_unlock(); FAIL(EAGAIN); }
            wait = deadline - now;
        }
        net_wait(&s->cond, wait);
        if (!socks[fd].used) { net_unlock(); FAIL(EBADF); }   /* closed under us */
    }

    dgram_t* d = s->dq_head;
    s->dq_head = d->next;
    if (!s->dq_head) s->dq_tail = NULL;
    s->dq_len--;

    size_t n = d->len < len ? d->len : len;   /* excess is truncated (POSIX) */
    memcpy(buf, d->data, n);
    fill_addr(from, fromlen, d->src_ip, d->src_port);
    kfree(d);
    net_unlock();
    return (ssize_t)n;
}

ssize_t recv(int fd, void* buf, size_t len, int flags) {
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}

int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) {
    if (level != SOL_SOCKET) FAIL(EINVAL);
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }
    int rc = 0;
    switch (optname) {
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            if (!optval || optlen < sizeof(struct timeval)) { rc = EINVAL; break; }
            const struct timeval* tv = (const struct timeval*)optval;
            uint64_t ms = (uint64_t)tv->tv_sec * 1000 + (uint64_t)tv->tv_usec / 1000;
            if (optname == SO_RCVTIMEO) s->rcvtimeo_ms = ms; else s->sndtimeo_ms = ms;
            break;
        }
        case SO_REUSEADDR:
            if (!optval || optlen < sizeof(int)) { rc = EINVAL; break; }
            s->reuseaddr = *(const int*)optval != 0;
            break;
        default:
            rc = EINVAL;
    }
    net_unlock();
    if (rc) FAIL(rc);
    return 0;
}

int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }
    uint32_t ip = s->lip;
    if (s->tcb && s->tcb->lip) ip = s->tcb->lip;
    fill_addr(addr, len, ip, s->lport);
    net_unlock();
    return 0;
}

int shutdown(int fd, int how) {
    if (how < SHUT_RD || how > SHUT_RDWR) FAIL(EINVAL);
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }
    int rc = 0;
    if (how == SHUT_RD || how == SHUT_RDWR) s->shut_rd = 1;
    if (how == SHUT_WR || how == SHUT_RDWR) s->shut_wr = 1;
    if (s->type == SOCK_STREAM) rc = tcp_shutdown(s, how);
    pthread_cond_broadcast(&s->cond);
    net_unlock();
    if (rc < 0) FAIL(-rc);
    return 0;
}

int sock_close(int fd) {
    net_lock();
    sock_t* s = get(fd);
    if (!s) { net_unlock(); FAIL(EBADF); }

    if (s->type == SOCK_STREAM) {
        tcp_close(s);                         /* detaches/orphans the tcb */
    } else {
        while (s->dq_head) {
            dgram_t* d = s->dq_head;
            s->dq_head = d->next;
            kfree(d);
        }
    }
    /* Wake anyone blocked on this socket; they'll observe it's gone. */
    pthread_cond_broadcast(&s->cond);
    s->used = 0;
    net_unlock();
    return 0;
}
