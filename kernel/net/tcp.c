/**
 * =============================================================================
 * MakhOS - net/tcp.c
 * =============================================================================
 * A compact but real TCP (RFC 793 state machine):
 *   - active + passive open, SYN MSS option, listen backlog / accept queue
 *   - byte-stream send/receive buffers with flow control (advertised window)
 *   - go-back-N retransmission from snd_una with exponential RTO backoff
 *   - in-order receive (out-of-order segments are dropped and dup-ACKed; the
 *     sender's retransmission fills the hole)
 *   - graceful close (FIN in both directions, TIME_WAIT), abortive close (RST
 *     when unread data is discarded), RST handling, zero-window probe
 *   - orphaned connections (socket closed, FIN still in flight) are reaped by
 *     the timer; nothing leaks
 *
 * Everything runs under net_lock. Segments are built into a fresh heap buffer
 * per call because ipv4_send() may drop the lock while waiting for ARP.
 * =============================================================================
 */

#include "sock.h"
#include <errno.h>
#include <ktime.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <klog.h>

extern uint32_t ipv4_src_for(uint32_t dst);

#define TCP_RTO_INIT_MS        300
#define TCP_RTO_MAX_MS         3000
#define TCP_MAX_RETRIES        10
#define TCP_TIMEWAIT_MS        500
#define TCP_ORPHAN_LINGER_MS   5000
#define TCP_CONNECT_TIMEOUT_MS 10000

static tcb_t* tcbs = NULL;
static uint32_t iss_state = 0x4D414B48u;   /* "MAKH" */

/* Sequence-space comparisons (mod 2^32). */
static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static uint32_t new_iss(void) {
    iss_state = iss_state * 1103515245u + 12345u + (uint32_t)clock_now_ms() * 2654435761u;
    return iss_state;
}

/* ---------------------------------------------------------------- tcb pool */

static tcb_t* tcb_alloc(int with_buffers) {
    tcb_t* t = kcalloc(1, sizeof(tcb_t));
    if (!t) return NULL;
    if (with_buffers) {
        t->sb = kmalloc(TCP_BUF_SIZE);
        t->rb = kmalloc(TCP_BUF_SIZE);
        if (!t->sb || !t->rb) {
            kfree(t->sb); kfree(t->rb); kfree(t);
            return NULL;
        }
    }
    t->rto_ms = TCP_RTO_INIT_MS;
    t->peer_mss = 536;              /* RFC default until the peer tells us */
    t->next = tcbs;
    tcbs = t;
    return t;
}

static void tcb_free(tcb_t* t) {
    for (tcb_t** pp = &tcbs; *pp; pp = &(*pp)->next) {
        if (*pp == t) { *pp = t->next; break; }
    }
    kfree(t->sb);
    kfree(t->rb);
    kfree(t);
}

int tcp_active_count(void) {
    int n = 0;
    for (tcb_t* t = tcbs; t; t = t->next) n++;
    return n;
}

static void tcb_signal(tcb_t* t) {
    if (t->cond) pthread_cond_broadcast(t->cond);
}

static uint32_t rcv_window(const tcb_t* t) {
    if (!t->rb) return 0;
    uint32_t w = TCP_BUF_SIZE - t->rb_len;
    return w > 65535 ? 65535 : w;
}

/* ---------------------------------------------------------------- output */

static int tcp_xmit(tcb_t* t, uint32_t seq, uint8_t flags,
                    const uint8_t* data, size_t len, int mss_opt) {
    size_t hlen = sizeof(tcp_hdr_t) + (mss_opt ? 4 : 0);
    size_t total = hlen + len;
    uint8_t* seg = kmalloc(total);
    if (!seg) return -ENOBUFS;

    tcp_hdr_t* h = (tcp_hdr_t*)seg;
    h->src_port = htons(t->lport);
    h->dst_port = htons(t->rport);
    h->seq = htonl(seq);
    h->ack = (flags & TCP_ACK) ? htonl(t->rcv_nxt) : 0;
    h->data_off = (uint8_t)((hlen / 4) << 4);
    h->flags = flags;
    uint32_t wnd = rcv_window(t);
    h->window = htons((uint16_t)wnd);
    t->last_adv_wnd = wnd;
    h->checksum = 0;
    h->urgent = 0;
    if (mss_opt) {
        uint8_t* o = seg + sizeof(tcp_hdr_t);
        o[0] = 2; o[1] = 4; o[2] = (uint8_t)(TCP_MSS >> 8); o[3] = (uint8_t)(TCP_MSS & 0xFF);
    }
    if (len) memcpy(seg + hlen, data, len);
    h->checksum = inet_pseudo_checksum(t->lip, t->rip, IPPROTO_TCP, seg, total);

    net_stats.tcp_tx++;
    if (flags & TCP_RST) net_stats.tcp_rst_tx++;
    int rc = ipv4_send(t->rip, IPPROTO_TCP, seg, total);
    kfree(seg);
    return rc;
}

/* RST for a segment that matches no connection (RFC 793 "reset generation"). */
static void tcp_rst_raw(uint32_t lip, uint32_t rip, uint16_t lport, uint16_t rport,
                        uint32_t seq, uint32_t ack, int with_ack) {
    tcp_hdr_t h;
    h.src_port = htons(lport);
    h.dst_port = htons(rport);
    h.seq = htonl(seq);
    h.ack = with_ack ? htonl(ack) : 0;
    h.data_off = (uint8_t)((sizeof(tcp_hdr_t) / 4) << 4);
    h.flags = (uint8_t)(TCP_RST | (with_ack ? TCP_ACK : 0));
    h.window = 0;
    h.checksum = 0;
    h.urgent = 0;
    h.checksum = inet_pseudo_checksum(lip, rip, IPPROTO_TCP, &h, sizeof(h));
    net_stats.tcp_tx++;
    net_stats.tcp_rst_tx++;
    ipv4_send(rip, IPPROTO_TCP, &h, sizeof(h));
}

static void arm_rtx(tcb_t* t) {
    if (!t->rtx_deadline) t->rtx_deadline = clock_now_ms() + t->rto_ms;
}

static int can_send_data(tcp_state_t s) {
    return s == TCP_ESTABLISHED || s == TCP_CLOSE_WAIT || s == TCP_FIN_WAIT_1 ||
           s == TCP_LAST_ACK || s == TCP_CLOSING;
}

/* Push as much buffered data as the peer's window allows, then FIN if queued. */
static void tcp_output(tcb_t* t) {
    if (!can_send_data(t->state)) return;

    while (!t->fin_sent) {
        uint32_t in_flight = t->snd_nxt - t->snd_una;
        uint32_t unsent = t->sb_len - in_flight;
        uint32_t wnd = t->snd_wnd;
        if (wnd == 0 && in_flight == 0 && unsent > 0) wnd = 1;   /* window probe */
        if (unsent == 0 || in_flight >= wnd) break;

        uint32_t n = unsent;
        if (n > t->peer_mss) n = t->peer_mss;
        if (n > wnd - in_flight) n = wnd - in_flight;

        uint32_t seq = t->snd_nxt;
        t->snd_nxt += n;
        arm_rtx(t);
        tcp_xmit(t, seq, TCP_ACK | TCP_PSH, t->sb + in_flight, n, 0);
    }

    if (t->fin_queued && !t->fin_sent && (t->snd_nxt - t->snd_una) == t->sb_len) {
        t->fin_seq = t->snd_nxt;
        t->snd_nxt++;
        t->fin_sent = 1;
        arm_rtx(t);
        tcp_xmit(t, t->fin_seq, TCP_FIN | TCP_ACK, NULL, 0, 0);
    }
}

static void send_ack(tcb_t* t) {
    tcp_xmit(t, t->snd_nxt, TCP_ACK, NULL, 0, 0);
}

/* Go back N: rewind to snd_una and resend everything outstanding. */
static void go_back_n(tcb_t* t) {
    t->snd_nxt = t->snd_una;
    if (t->fin_sent && seq_ge(t->fin_seq, t->snd_una)) t->fin_sent = 0;
    t->rtx_deadline = 0;
    tcp_output(t);
    if (!t->rtx_deadline && t->snd_una != t->snd_nxt) arm_rtx(t);
}

/* Enter CLOSED because of an error; wake the owner. */
static void tcp_abort(tcb_t* t, int err) {
    t->err = err;
    t->state = TCP_CLOSED;
    t->rtx_deadline = 0;
    tcb_signal(t);
    if (t->parent) tcb_signal(t->parent);
}

/* ---------------------------------------------------------------- lookup */

static tcb_t* find_conn(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport) {
    for (tcb_t* t = tcbs; t; t = t->next) {
        if (t->state == TCP_LISTEN || t->state == TCP_CLOSED) continue;
        if (t->lport == lport && t->rport == rport && t->rip == rip &&
            (t->lip == lip || t->lip == 0)) return t;
    }
    return NULL;
}

static tcb_t* find_listener(uint32_t lip, uint16_t lport) {
    tcb_t* wildcard = NULL;
    for (tcb_t* t = tcbs; t; t = t->next) {
        if (t->state != TCP_LISTEN || t->lport != lport) continue;
        if (t->lip == lip) return t;
        if (t->lip == 0) wildcard = t;
    }
    return wildcard;
}

static uint16_t parse_mss(const uint8_t* opt, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t kind = opt[i];
        if (kind == 0) break;                 /* end of options */
        if (kind == 1) { i++; continue; }     /* NOP */
        if (i + 1 >= len) break;
        uint8_t olen = opt[i + 1];
        if (olen < 2 || i + olen > len) break;
        if (kind == 2 && olen == 4) return (uint16_t)((opt[i + 2] << 8) | opt[i + 3]);
        i += olen;
    }
    return 0;
}

/* ---------------------------------------------------------------- input */

static void accept_data(tcb_t* t, const uint8_t* data, size_t len) {
    if (!t->rb) return;                     /* buffers already released */
    uint32_t space = TCP_BUF_SIZE - t->rb_len;
    uint32_t n = len < space ? (uint32_t)len : space;   /* excess is dropped */
    uint32_t tail = (t->rb_head + t->rb_len) % TCP_BUF_SIZE;
    for (uint32_t i = 0; i < n; i++) {
        t->rb[tail] = data[i];
        tail = (tail + 1) % TCP_BUF_SIZE;
    }
    t->rb_len += n;
    t->rcv_nxt += n;
}

static void handle_listen(tcb_t* l, uint32_t src, uint32_t dst, const tcp_hdr_t* h,
                          const uint8_t* opts, size_t optlen) {
    uint8_t flags = h->flags;
    if (flags & TCP_RST) return;
    if (flags & TCP_ACK) {
        tcp_rst_raw(dst, src, ntohs(h->dst_port), ntohs(h->src_port), ntohl(h->ack), 0, 0);
        return;
    }
    if (!(flags & TCP_SYN)) return;

    /* Backlog: established-but-unaccepted plus half-open children. */
    if (l->aq_len + l->half_open >= l->backlog) return;   /* drop; peer retries */

    tcb_t* c = tcb_alloc(1);
    if (!c) return;
    c->lip = dst;
    c->lport = l->lport;
    c->rip = src;
    c->rport = ntohs(h->src_port);
    c->irs = ntohl(h->seq);
    c->rcv_nxt = c->irs + 1;
    c->iss = new_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->snd_wnd = ntohs(h->window);
    uint16_t mss = parse_mss(opts, optlen);
    if (mss) c->peer_mss = mss < TCP_MSS ? mss : TCP_MSS;
    c->state = TCP_SYN_RCVD;
    c->parent = l;
    l->half_open++;

    arm_rtx(c);
    tcp_xmit(c, c->iss, TCP_SYN | TCP_ACK, NULL, 0, 1);
}

static void handle_syn_sent(tcb_t* t, const tcp_hdr_t* h, const uint8_t* opts, size_t optlen) {
    uint8_t flags = h->flags;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);

    if (flags & TCP_ACK) {
        if (ack != t->iss + 1) {                  /* unacceptable ACK */
            if (!(flags & TCP_RST)) tcp_rst_raw(t->lip, t->rip, t->lport, t->rport, ack, 0, 0);
            return;
        }
        if (flags & TCP_RST) { tcp_abort(t, ECONNREFUSED); return; }
    } else if (flags & TCP_RST) {
        return;
    }
    if (!(flags & TCP_SYN)) return;

    t->irs = seq;
    t->rcv_nxt = seq + 1;
    uint16_t mss = parse_mss(opts, optlen);
    if (mss) t->peer_mss = mss < TCP_MSS ? mss : TCP_MSS;
    t->snd_wnd = ntohs(h->window);

    if (flags & TCP_ACK) {
        t->snd_una = ack;
        t->state = TCP_ESTABLISHED;
        t->rtx_deadline = 0;
        t->rtx_count = 0;
        t->rto_ms = TCP_RTO_INIT_MS;
        send_ack(t);
        tcb_signal(t);
    }
    /* (Simultaneous open is not supported: a bare SYN is ignored.) */
}

static void process_ack(tcb_t* t, uint32_t ack, uint16_t window, int pure_ack) {
    if (seq_gt(ack, t->snd_nxt)) {            /* acks something we never sent */
        send_ack(t);
        return;
    }

    /* Duplicate ACK (same ack, same window, no payload, data outstanding): the
     * receiver saw a hole. Three in a row trigger a fast retransmit instead of
     * waiting for the RTO. Afterwards further dups are ignored until new data
     * is acknowledged, so one loss doesn't cause a retransmission storm. */
    if (ack == t->snd_una && pure_ack && window == t->snd_wnd &&
        t->snd_nxt != t->snd_una) {
        if (++t->dup_acks == 3) {
            net_stats.tcp_retrans++;
            t->dup_acks = -1000;
            go_back_n(t);
        }
        return;
    }

    if (seq_gt(ack, t->snd_una)) {
        t->dup_acks = 0;
        uint32_t acked = ack - t->snd_una;
        int fin_acked = t->fin_sent && seq_gt(ack, t->fin_seq);
        uint32_t data_acked = acked - (fin_acked ? 1 : 0);
        if (data_acked > t->sb_len) data_acked = t->sb_len;
        memmove(t->sb, t->sb + data_acked, t->sb_len - data_acked);
        t->sb_len -= data_acked;
        t->snd_una = ack;

        t->rtx_count = 0;
        t->rto_ms = TCP_RTO_INIT_MS;
        t->rtx_deadline = (t->snd_una == t->snd_nxt) ? 0 : clock_now_ms() + t->rto_ms;

        if (fin_acked) {
            if (t->state == TCP_FIN_WAIT_1) t->state = TCP_FIN_WAIT_2;
            else if (t->state == TCP_CLOSING) {
                t->state = TCP_TIME_WAIT;
                t->tw_deadline = clock_now_ms() + TCP_TIMEWAIT_MS;
            } else if (t->state == TCP_LAST_ACK) {
                t->state = TCP_CLOSED;
            }
        }
        tcb_signal(t);                         /* buffer space freed */
    }
    t->snd_wnd = window;
}

static void handle_synchronized(tcb_t* t, const tcp_hdr_t* h,
                                const uint8_t* data, size_t dlen) {
    uint8_t flags = h->flags;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    uint32_t wnd = rcv_window(t);
    if (wnd == 0) wnd = 1;

    /* RST: accept only if it falls in our receive window. */
    if (flags & TCP_RST) {
        if (seq_ge(seq, t->rcv_nxt) && seq_lt(seq, t->rcv_nxt + wnd)) {
            if (t->state == TCP_SYN_RCVD && t->parent) {
                t->parent->half_open--;
                t->parent = NULL;
                t->state = TCP_CLOSED;         /* orphan; reaped by the timer */
            } else {
                tcp_abort(t, ECONNRESET);
            }
        }
        return;
    }

    /* A SYN in a synchronized state. */
    if (flags & TCP_SYN) {
        if (t->state == TCP_SYN_RCVD && seq == t->irs) {
            tcp_xmit(t, t->iss, TCP_SYN | TCP_ACK, NULL, 0, 1);   /* our SYN-ACK was lost */
        } else {
            send_ack(t);                        /* challenge ACK */
        }
        return;
    }

    if (!(flags & TCP_ACK)) return;

    if (t->state == TCP_SYN_RCVD) {
        if (ack != t->iss + 1) {
            tcp_rst_raw(t->lip, t->rip, t->lport, t->rport, ack, 0, 0);
            return;
        }
        t->snd_una = ack;
        t->snd_wnd = ntohs(h->window);
        t->rtx_deadline = 0;
        t->rtx_count = 0;
        t->state = TCP_ESTABLISHED;
        tcb_t* l = t->parent;
        if (l) {
            l->half_open--;
            t->aq_next = NULL;
            if (l->aq_tail) l->aq_tail->aq_next = t; else l->aq_head = t;
            l->aq_tail = t;
            l->aq_len++;
            tcb_signal(l);
        }
    } else {
        int pure = (dlen == 0) && !(flags & (TCP_FIN | TCP_SYN));
        process_ack(t, ack, ntohs(h->window), pure);
        if (t->state == TCP_CLOSED) {
            tcb_signal(t);
            return;
        }
    }

    /* Payload and FIN: in-order only. */
    int has_fin = (flags & TCP_FIN) != 0;
    if (dlen > 0 || has_fin) {
        if (seq != t->rcv_nxt) {
            send_ack(t);                        /* dup ACK: out of order / retransmit */
            return;
        }
        int can_receive = t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
                          t->state == TCP_FIN_WAIT_2;
        size_t before = t->rb_len;
        if (dlen > 0 && can_receive && !t->fin_rcvd) {
            accept_data(t, data, dlen);
        }
        int all_data_taken = (t->rb_len - before) == dlen;

        if (has_fin && all_data_taken && !t->fin_rcvd) {
            t->rcv_nxt++;
            t->fin_rcvd = 1;
            switch (t->state) {
                case TCP_ESTABLISHED: t->state = TCP_CLOSE_WAIT; break;
                case TCP_FIN_WAIT_1:
                    if (t->fin_sent && t->snd_una == t->snd_nxt) {
                        t->state = TCP_TIME_WAIT;
                        t->tw_deadline = clock_now_ms() + TCP_TIMEWAIT_MS;
                    } else {
                        t->state = TCP_CLOSING;
                    }
                    break;
                case TCP_FIN_WAIT_2:
                    t->state = TCP_TIME_WAIT;
                    t->tw_deadline = clock_now_ms() + TCP_TIMEWAIT_MS;
                    break;
                default: break;
            }
        } else if (has_fin && t->state == TCP_TIME_WAIT) {
            t->tw_deadline = clock_now_ms() + TCP_TIMEWAIT_MS;   /* retransmitted FIN */
        }
        send_ack(t);
        tcb_signal(t);
    }

    /* The ACK may have opened the window: push pending data. */
    tcp_output(t);
}

void tcp_input(netdev_t* dev, uint32_t src, uint32_t dst, const uint8_t* pkt, size_t len) {
    (void)dev;
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t* h = (const tcp_hdr_t*)pkt;
    size_t hlen = (size_t)(h->data_off >> 4) * 4;
    if (hlen < sizeof(tcp_hdr_t) || hlen > len) return;
    if (inet_pseudo_checksum(src, dst, IPPROTO_TCP, pkt, len) != 0) return;
    net_stats.tcp_rx++;

    uint16_t sport = ntohs(h->src_port), dport = ntohs(h->dst_port);
    const uint8_t* opts = pkt + sizeof(tcp_hdr_t);
    size_t optlen = hlen - sizeof(tcp_hdr_t);
    const uint8_t* data = pkt + hlen;
    size_t dlen = len - hlen;

    tcb_t* t = find_conn(dst, dport, src, sport);
    if (!t) {
        tcb_t* l = find_listener(dst, dport);
        if (l) { handle_listen(l, src, dst, h, opts, optlen); return; }

        /* No such connection: answer with RST (never RST a RST). */
        if (h->flags & TCP_RST) return;
        if (h->flags & TCP_ACK) {
            tcp_rst_raw(dst, src, dport, sport, ntohl(h->ack), 0, 0);
        } else {
            uint32_t seg_len = (uint32_t)dlen + ((h->flags & TCP_SYN) ? 1 : 0) +
                               ((h->flags & TCP_FIN) ? 1 : 0);
            tcp_rst_raw(dst, src, dport, sport, 0, ntohl(h->seq) + seg_len, 1);
        }
        return;
    }

    if (t->state == TCP_SYN_SENT) handle_syn_sent(t, h, opts, optlen);
    else handle_synchronized(t, h, data, dlen);
}

/* ---------------------------------------------------------------- timers */

void tcp_timer(void) {
    uint64_t now = clock_now_ms();
    tcb_t** pp = &tcbs;
    while (*pp) {
        tcb_t* t = *pp;
        int orphan = (t->sock == NULL && t->parent == NULL && t->state != TCP_LISTEN);

        /* Retransmission. */
        if (t->rtx_deadline && now >= t->rtx_deadline) {
            if (++t->rtx_count > TCP_MAX_RETRIES) {
                /* Only half-open (SYN_RCVD) children are counted in the
                 * listener's half_open; an established child stays in the
                 * accept queue and accept() returns it with err set. */
                int was_half_open = (t->state == TCP_SYN_RCVD);
                tcp_abort(t, ETIMEDOUT);
                if (was_half_open && t->parent) {
                    t->parent->half_open--;
                    t->parent = NULL;
                }
            } else {
                t->rto_ms = t->rto_ms * 2 > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : t->rto_ms * 2;
                t->rtx_deadline = now + t->rto_ms;
                net_stats.tcp_retrans++;
                if (t->state == TCP_SYN_SENT) {
                    tcp_xmit(t, t->iss, TCP_SYN, NULL, 0, 1);
                } else if (t->state == TCP_SYN_RCVD) {
                    tcp_xmit(t, t->iss, TCP_SYN | TCP_ACK, NULL, 0, 1);
                } else {
                    go_back_n(t);   /* resend everything from snd_una */
                }
            }
        }

        /* TIME_WAIT expiry. */
        if (t->state == TCP_TIME_WAIT && now >= t->tw_deadline) {
            t->state = TCP_CLOSED;
            tcb_signal(t);
        }

        /* An orphan in TIME_WAIT can neither send nor deliver data again, so
         * hand its 2 x TCP_BUF_SIZE buffers back now rather than holding them
         * for the whole TIME_WAIT period (a busy server has many of these). */
        if (orphan && t->state == TCP_TIME_WAIT && (t->sb || t->rb)) {
            kfree(t->sb);
            kfree(t->rb);
            t->sb = t->rb = NULL;
            t->sb_len = t->rb_len = 0;
        }

        /* Reap closed or lingering orphans; owned tcbs are freed by close. */
        int reap = orphan && (t->state == TCP_CLOSED ||
                   (t->tw_deadline && now >= t->tw_deadline &&
                    t->state != TCP_TIME_WAIT));
        if (reap) {
            *pp = t->next;
            kfree(t->sb);
            kfree(t->rb);
            kfree(t);
            continue;
        }
        pp = &t->next;
    }
}

/* ---------------------------------------------------------------- socket ops */

int tcp_listen(sock_t* s, int backlog) {
    if (s->tcb) {
        if (s->tcb->state == TCP_LISTEN) { s->tcb->backlog = backlog > 0 ? backlog : 1; return 0; }
        return -EISCONN;
    }
    tcb_t* l = tcb_alloc(0);
    if (!l) return -ENOBUFS;
    l->state = TCP_LISTEN;
    l->lip = s->lip;
    l->lport = s->lport;
    l->backlog = backlog > 0 ? (backlog > 64 ? 64 : backlog) : 1;
    l->sock = s;
    l->cond = &s->cond;
    s->tcb = l;
    return 0;
}

int tcp_accept(sock_t* s, uint32_t* rip, uint16_t* rport) {
    tcb_t* l = s->tcb;
    if (!l || l->state != TCP_LISTEN) return -EINVAL;

    uint64_t deadline = s->rcvtimeo_ms ? clock_now_ms() + s->rcvtimeo_ms : 0;
    int fd = sock_fd(s);
    while (!l->aq_head) {
        uint64_t wait = 0;
        if (deadline) {
            uint64_t now = clock_now_ms();
            if (now >= deadline) return -EAGAIN;
            wait = deadline - now;
        }
        net_wait(&s->cond, wait);
        if (!s->used || sock_fd(s) != fd || s->tcb != l) return -EBADF;   /* closed */
    }

    tcb_t* c = l->aq_head;
    l->aq_head = c->aq_next;
    if (!l->aq_head) l->aq_tail = NULL;
    l->aq_len--;
    c->aq_next = NULL;
    c->parent = NULL;

    sock_t* ns = sock_alloc(SOCK_STREAM);
    if (!ns) {
        tcp_xmit(c, c->snd_nxt, TCP_RST | TCP_ACK, NULL, 0, 0);
        c->state = TCP_CLOSED;                  /* orphan: reaped by the timer */
        return -EMFILE;
    }
    ns->bound = 1;
    ns->lip = c->lip;
    ns->lport = c->lport;
    ns->rip = c->rip;
    ns->rport = c->rport;
    ns->tcb = c;
    c->sock = ns;
    c->cond = &ns->cond;

    *rip = c->rip;
    *rport = c->rport;
    return sock_fd(ns);
}

int tcp_connect(sock_t* s, uint32_t rip, uint16_t rport) {
    if (s->tcb) return s->tcb->state == TCP_LISTEN ? -EINVAL : -EISCONN;

    uint32_t lip = s->lip ? s->lip : ipv4_src_for(rip);
    if (!lip) return -ENETUNREACH;
    if (!s->bound) {
        s->lport = sock_ephemeral_port(SOCK_STREAM);
        if (!s->lport) return -EADDRINUSE;
        s->bound = 1;
    }

    tcb_t* t = tcb_alloc(1);
    if (!t) return -ENOBUFS;
    t->lip = lip;
    t->lport = s->lport;
    t->rip = rip;
    t->rport = rport;
    t->iss = new_iss();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->state = TCP_SYN_SENT;
    t->sock = s;
    t->cond = &s->cond;
    s->tcb = t;

    arm_rtx(t);
    tcp_xmit(t, t->iss, TCP_SYN, NULL, 0, 1);

    uint64_t timeout = s->sndtimeo_ms ? s->sndtimeo_ms : TCP_CONNECT_TIMEOUT_MS;
    uint64_t deadline = clock_now_ms() + timeout;
    while (t->state == TCP_SYN_SENT) {
        uint64_t now = clock_now_ms();
        if (now >= deadline) break;
        net_wait(&s->cond, deadline - now);
    }

    if (t->state == TCP_ESTABLISHED) return 0;

    int err = t->err ? t->err : ETIMEDOUT;
    s->tcb = NULL;
    tcb_free(t);
    return -err;
}

long tcp_send(sock_t* s, const uint8_t* buf, size_t len, int dontwait) {
    tcb_t* t = s->tcb;
    if (!t || t->state == TCP_LISTEN) return -ENOTCONN;
    if (s->shut_wr || t->fin_queued) return -EPIPE;

    uint64_t deadline = s->sndtimeo_ms ? clock_now_ms() + s->sndtimeo_ms : 0;
    size_t done = 0;
    while (done < len) {
        if (t->err) return done ? (long)done : -t->err;
        if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
            return done ? (long)done : -EPIPE;

        uint32_t space = TCP_BUF_SIZE - t->sb_len;
        if (space == 0) {
            if (dontwait) return done ? (long)done : -EAGAIN;
            uint64_t wait = 0;
            if (deadline) {
                uint64_t now = clock_now_ms();
                if (now >= deadline) return done ? (long)done : -EAGAIN;
                wait = deadline - now;
            }
            net_wait(&s->cond, wait);
            if (s->tcb != t) return done ? (long)done : -EBADF;
            continue;
        }

        size_t n = len - done < space ? len - done : space;
        memcpy(t->sb + t->sb_len, buf + done, n);
        t->sb_len += (uint32_t)n;
        done += n;
        tcp_output(t);
    }
    return (long)done;
}

long tcp_recv(sock_t* s, uint8_t* buf, size_t len, int dontwait) {
    tcb_t* t = s->tcb;
    if (!t) return -ENOTCONN;
    if (t->state == TCP_LISTEN) return -EINVAL;
    if (len == 0) return 0;

    uint64_t deadline = s->rcvtimeo_ms ? clock_now_ms() + s->rcvtimeo_ms : 0;
    while (t->rb_len == 0) {
        if (t->err) return -t->err;
        if (t->fin_rcvd || s->shut_rd || t->state == TCP_CLOSED) return 0;   /* EOF */
        if (dontwait) return -EAGAIN;
        uint64_t wait = 0;
        if (deadline) {
            uint64_t now = clock_now_ms();
            if (now >= deadline) return -EAGAIN;
            wait = deadline - now;
        }
        net_wait(&s->cond, wait);
        if (s->tcb != t) return -EBADF;
    }

    size_t n = len < t->rb_len ? len : t->rb_len;
    for (size_t i = 0; i < n; i++) {
        buf[i] = t->rb[t->rb_head];
        t->rb_head = (t->rb_head + 1) % TCP_BUF_SIZE;
    }
    t->rb_len -= (uint32_t)n;

    /* Window update if we had throttled the peer and now have room again. */
    uint32_t wnd = rcv_window(t);
    if (can_send_data(t->state) || t->state == TCP_FIN_WAIT_2) {
        if (t->last_adv_wnd < TCP_MSS && wnd >= TCP_MSS) send_ack(t);
    }
    return (long)n;
}

int tcp_shutdown(sock_t* s, int how) {
    tcb_t* t = s->tcb;
    if (!t) return -ENOTCONN;
    if (how == SHUT_WR || how == SHUT_RDWR) {
        if (t->state == TCP_ESTABLISHED) {
            t->fin_queued = 1;
            t->state = TCP_FIN_WAIT_1;
            tcp_output(t);
        } else if (t->state == TCP_CLOSE_WAIT) {
            t->fin_queued = 1;
            t->state = TCP_LAST_ACK;
            tcp_output(t);
        }
    }
    return 0;
}

void tcp_close(sock_t* s) {
    tcb_t* t = s->tcb;
    s->tcb = NULL;
    if (!t) return;

    if (t->state == TCP_LISTEN) {
        /* Tear down the listener and every connection it spawned. */
        for (tcb_t* c = tcbs; c; c = c->next) {
            if (c->parent == t) {
                if (c->state != TCP_CLOSED)
                    tcp_xmit(c, c->snd_nxt, TCP_RST | TCP_ACK, NULL, 0, 0);
                c->state = TCP_CLOSED;
                c->parent = NULL;               /* orphan: reaped by the timer */
            }
        }
        tcb_free(t);
        return;
    }

    if (t->state == TCP_SYN_SENT || t->state == TCP_CLOSED) {
        tcb_free(t);
        return;
    }

    /* Orphan the connection; the timer reaps it once it is finished. */
    t->sock = NULL;
    t->cond = NULL;
    t->tw_deadline = clock_now_ms() + TCP_ORPHAN_LINGER_MS;

    if (t->rb_len > 0 && !t->fin_rcvd) {
        /* Closing with unread data: abortive close (RFC 2525 / Linux). */
        tcp_xmit(t, t->snd_nxt, TCP_RST | TCP_ACK, NULL, 0, 0);
        t->state = TCP_CLOSED;
        return;
    }

    if (t->state == TCP_ESTABLISHED) {
        t->fin_queued = 1;
        t->state = TCP_FIN_WAIT_1;
        tcp_output(t);
    } else if (t->state == TCP_CLOSE_WAIT) {
        t->fin_queued = 1;
        t->state = TCP_LAST_ACK;
        tcp_output(t);
    }
}

/* netstat-style dump of every control block (the shell's `netstat`).
 * Caller holds net_lock. */
void tcp_debug_dump(void) {
    static const char* names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD",
        "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING",
        "LAST_ACK", "TIME_WAIT" };
    char a[16], b[16];
    kprintf("Proto Local                 Remote                State        Send-Q Recv-Q\n");
    for (tcb_t* t = tcbs; t; t = t->next) {
        char la[24], ra[24];
        ip_to_str(t->lip, a);
        ip_to_str(t->rip, b);
        /* "ip:port" into fixed buffers, then left-justify with %-21s. */
        int i = 0, j = 0;
        for (; a[j]; j++) la[i++] = a[j];
        la[i++] = ':';
        char pb[8]; int pn = 0; uint32_t pv = t->lport;
        do { pb[pn++] = (char)('0' + pv % 10); pv /= 10; } while (pv);
        while (pn) la[i++] = pb[--pn];
        la[i] = 0;
        i = 0;
        for (j = 0; b[j]; j++) ra[i++] = b[j];
        ra[i++] = ':';
        if (t->state == TCP_LISTEN) { ra[i++] = '*'; }
        else {
            pv = t->rport;
            do { pb[pn++] = (char)('0' + pv % 10); pv /= 10; } while (pv);
            while (pn) ra[i++] = pb[--pn];
        }
        ra[i] = 0;
        kprintf("tcp   %-21s %-21s %-12s %6u %6u%s\n", la, ra, names[t->state],
                t->sb_len, t->rb_len, t->sock ? "" : "  (orphan)");
    }
}
