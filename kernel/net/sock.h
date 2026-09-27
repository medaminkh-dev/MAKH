#ifndef MAKHOS_NET_SOCK_H
#define MAKHOS_NET_SOCK_H

/**
 * sock.h - private socket / TCP control block definitions (kernel/net only).
 * Everything here is protected by net_lock.
 */

#include <net/net.h>
#include <net/socket.h>
#include <pthread.h>

#define SOCK_MAX        64
#define UDP_RXQ_MAX     64          /* queued datagrams per UDP socket */
#define TCP_BUF_SIZE    (16 * 1024) /* per-direction stream buffer */
#define TCP_MSS         (ETH_MTU - 40)

/* ---------------------------------------------------------------- UDP */

typedef struct dgram {
    struct dgram* next;
    uint32_t src_ip;        /* host order */
    uint16_t src_port;
    size_t   len;
    uint8_t  data[];
} dgram_t;

/* ---------------------------------------------------------------- TCP */

typedef enum {
    TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING,
    TCP_LAST_ACK, TCP_TIME_WAIT
} tcp_state_t;

struct sock;

typedef struct tcb {
    tcp_state_t state;
    uint32_t lip, rip;              /* host order */
    uint16_t lport, rport;

    /* send sequence space */
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint16_t peer_mss;
    /* receive sequence space */
    uint32_t irs, rcv_nxt;

    /* send buffer: sb[0] is the byte at sequence snd_una */
    uint8_t* sb;  uint32_t sb_len;
    /* receive buffer (ring) */
    uint8_t* rb;  uint32_t rb_head, rb_len;
    uint32_t last_adv_wnd;          /* window we last advertised */

    int fin_rcvd;                   /* peer closed its direction */
    int fin_queued;                 /* we want to send FIN after the data */
    int fin_sent;
    uint32_t fin_seq;

    /* retransmission (go-back-N from snd_una) */
    uint64_t rto_ms;
    uint64_t rtx_deadline;          /* 0 = timer off */
    int      rtx_count;
    int      dup_acks;              /* consecutive duplicate ACKs (fast rtx) */
    uint64_t tw_deadline;           /* TIME_WAIT / orphan linger expiry */

    int err;                        /* pending errno for the owner */

    /* passive open */
    int backlog;
    struct tcb* parent;             /* LISTEN tcb that spawned us */
    struct tcb* aq_next;            /* link in parent's accept queue */
    struct tcb* aq_head;            /* LISTEN: established, not yet accepted */
    struct tcb* aq_tail;
    int aq_len;
    int half_open;                  /* LISTEN: children still in SYN_RCVD */

    struct sock* sock;              /* owner; NULL once orphaned/unaccepted */
    pthread_cond_t* cond;           /* where state changes are signalled */
    struct tcb* next;               /* global tcb list */
} tcb_t;

/* ---------------------------------------------------------------- socket */

typedef struct sock {
    int      used;
    int      type;                  /* SOCK_STREAM / SOCK_DGRAM */
    int      bound;
    uint32_t lip;  uint16_t lport;  /* host order */
    uint32_t rip;  uint16_t rport;
    int      connected;             /* UDP default destination set */
    uint64_t rcvtimeo_ms;           /* 0 = block forever */
    uint64_t sndtimeo_ms;
    int      reuseaddr;
    int      shut_rd, shut_wr;
    pthread_cond_t cond;

    dgram_t* dq_head; dgram_t* dq_tail; int dq_len;   /* UDP rx queue */

    tcb_t*   tcb;                   /* TCP (connection or listener) */
} sock_t;

/* socket.c helpers used by the protocols */
sock_t*  sock_alloc(int type);                          /* returns slot, fd via sock_fd */
int      sock_fd(sock_t* s);
uint16_t sock_ephemeral_port(int type);
int      sock_port_in_use(int type, uint32_t ip, uint16_t port);
sock_t*  udp_lookup(uint32_t dst_ip, uint16_t dst_port);

/* udp.c */
int udp_output(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
               const void* data, size_t len);

/* tcp.c (all called with net_lock held; return 0/bytes or -errno) */
int  tcp_listen(sock_t* s, int backlog);
int  tcp_accept(sock_t* s, uint32_t* rip, uint16_t* rport);   /* returns new fd */
int  tcp_connect(sock_t* s, uint32_t rip, uint16_t rport);
long tcp_send(sock_t* s, const uint8_t* buf, size_t len, int dontwait);
long tcp_recv(sock_t* s, uint8_t* buf, size_t len, int dontwait);
int  tcp_shutdown(sock_t* s, int how);
void tcp_close(sock_t* s);
int  tcp_active_count(void);                                 /* live tcbs (tests) */

#endif /* MAKHOS_NET_SOCK_H */
