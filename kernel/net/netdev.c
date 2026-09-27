/**
 * MakhOS - net/netdev.c
 * Network core: device registry, the netd processing thread, the global
 * net_lock monitor, the loopback device, and small helpers.
 */

#include <net/net.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <ktime.h>
#include <errno.h>
#include <irq.h>
#include <klog.h>
#include <kernel.h>
#include <mm/kheap.h>
#include <lib/string.h>

net_stats_t net_stats;

static netdev_t* devs[NET_MAX_DEVS];
static int ndevs = 0;
static netdev_t* default_dev = NULL;

static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t rx_sem;
static volatile int rx_pending = 0;
static pthread_t netd_thread = NULL;

/* netd wakes at least this often to run protocol timers. */
#define NET_TICK_MS 50

/* ---------------------------------------------------------------- locking */

void net_lock(void)   { pthread_mutex_lock(&g_net_lock); }
void net_unlock(void) { pthread_mutex_unlock(&g_net_lock); }

int net_wait(void* cond, uint64_t timeout_ms) {
    pthread_cond_t* c = (pthread_cond_t*)cond;
    if (timeout_ms == 0) {
        return pthread_cond_wait(c, &g_net_lock);
    }
    struct timespec dl;
    uint64_t when = clock_now_ms() + timeout_ms;
    dl.tv_sec = (int64_t)(when / 1000);
    dl.tv_nsec = (int64_t)((when % 1000) * 1000000);
    return pthread_cond_timedwait(c, &g_net_lock, &dl);
}

void net_signal_all(void* cond) {
    pthread_cond_broadcast((pthread_cond_t*)cond);
}

int net_in_netd(void) {
    return netd_thread != NULL && pthread_self() == netd_thread;
}

/* ---------------------------------------------------------------- devices */

int netdev_register(netdev_t* dev) {
    if (!dev || ndevs >= NET_MAX_DEVS) return -1;
    devs[ndevs++] = dev;
    if (!default_dev && !dev->is_loopback) default_dev = dev;
    char ipb[16];
    KLOG_I("NET", "%s registered: mac %02x:%02x:%02x:%02x:%02x:%02x ip %s\n",
           dev->name, dev->mac[0], dev->mac[1], dev->mac[2],
           dev->mac[3], dev->mac[4], dev->mac[5], ip_to_str(dev->ip, ipb));
    return 0;
}

netdev_t* netdev_get(int i) { return (i >= 0 && i < ndevs) ? devs[i] : NULL; }
int netdev_count(void) { return ndevs; }
void netdev_set_default(netdev_t* dev) { default_dev = dev; }
netdev_t* netdev_default(void) { return default_dev; }

netdev_t* netdev_by_name(const char* name) {
    for (int i = 0; i < ndevs; i++)
        if (strcmp(devs[i]->name, name) == 0) return devs[i];
    return NULL;
}

/* ---------------------------------------------------------------- netd */

void net_rx_notify(void) {
    /* Safe from IRQ context: sem_post never blocks. Coalesce notifications so
     * a burst of IRQs doesn't grow the semaphore without bound. */
    irqflags_t f = local_irq_save();
    int post = !rx_pending;
    rx_pending = 1;
    local_irq_restore(f);
    if (post) sem_post(&rx_sem);
}

static void* netd_main(void* arg) {
    (void)arg;
    uint64_t next_tick = clock_now_ms() + NET_TICK_MS;
    for (;;) {
        struct timespec dl;
        dl.tv_sec = (int64_t)(next_tick / 1000);
        dl.tv_nsec = (int64_t)((next_tick % 1000) * 1000000);
        sem_timedwait(&rx_sem, &dl);

        irqflags_t f = local_irq_save();
        rx_pending = 0;
        local_irq_restore(f);

        net_lock();
        for (int i = 0; i < ndevs; i++) {
            if (devs[i]->up && devs[i]->poll) devs[i]->poll(devs[i]);
        }
        uint64_t now = clock_now_ms();
        if (now >= next_tick) {
            arp_timer();
            tcp_timer();
            next_tick = now + NET_TICK_MS;
        }
        net_unlock();
    }
    return NULL;
}

/* ---------------------------------------------------------------- loopback */

#define LO_QUEUE 64

typedef struct lo_frame { uint8_t* data; size_t len; } lo_frame_t;

static struct {
    lo_frame_t q[LO_QUEUE];
    int head, tail, count;
    unsigned drop_one_in;
    uint32_t drop_counter;
} lo;

static netdev_t lo_dev;

void net_loopback_set_drop(unsigned one_in_n) {
    irqflags_t f = local_irq_save();
    lo.drop_one_in = one_in_n;
    lo.drop_counter = 0;
    local_irq_restore(f);
}

static int lo_send(netdev_t* dev, const void* frame, size_t len) {
    if (len > ETH_FRAME_MAX) { dev->tx_dropped++; return -1; }

    /* Fault injection: deterministically drop every Nth frame. */
    if (lo.drop_one_in && (++lo.drop_counter % lo.drop_one_in) == 0) {
        dev->tx_dropped++;
        return 0;   /* "sent", but lost on the wire */
    }

    uint8_t* copy = kmalloc(len);
    if (!copy) { dev->tx_dropped++; return -1; }
    memcpy(copy, frame, len);

    irqflags_t f = local_irq_save();
    if (lo.count == LO_QUEUE) {
        local_irq_restore(f);
        kfree(copy);
        dev->tx_dropped++;
        return -1;
    }
    lo.q[lo.tail].data = copy;
    lo.q[lo.tail].len = len;
    lo.tail = (lo.tail + 1) % LO_QUEUE;
    lo.count++;
    local_irq_restore(f);

    dev->tx_packets++;
    dev->tx_bytes += len;
    net_rx_notify();
    return 0;
}

static void lo_poll(netdev_t* dev) {
    for (;;) {
        irqflags_t f = local_irq_save();
        if (lo.count == 0) { local_irq_restore(f); break; }
        lo_frame_t fr = lo.q[lo.head];
        lo.head = (lo.head + 1) % LO_QUEUE;
        lo.count--;
        local_irq_restore(f);

        dev->rx_packets++;
        dev->rx_bytes += fr.len;
        net_input(dev, fr.data, fr.len);
        kfree(fr.data);
    }
}

/* ---------------------------------------------------------------- init */

void net_init(void) {
    memset(&net_stats, 0, sizeof(net_stats));
    sem_init(&rx_sem, 0, 0);

    memset(&lo_dev, 0, sizeof(lo_dev));
    strcpy(lo_dev.name, "lo");
    lo_dev.ip = IPV4(127, 0, 0, 1);
    lo_dev.netmask = IPV4(255, 0, 0, 0);
    lo_dev.is_loopback = 1;
    lo_dev.up = 1;
    lo_dev.send = lo_send;
    lo_dev.poll = lo_poll;
    netdev_register(&lo_dev);

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setpriority_np(&a, PRIO_DEFAULT - 2);   /* above apps */
    pthread_create(&netd_thread, &a, netd_main, NULL);
    netd_thread->name[0] = 'n'; netd_thread->name[1] = 'e';
    netd_thread->name[2] = 't'; netd_thread->name[3] = 'd'; netd_thread->name[4] = 0;
    KLOG_I("NET", "netd started\n");
}

/* ---------------------------------------------------------------- helpers */

uint16_t inet_checksum(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint16_t inet_pseudo_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                              const void* data, size_t len) {
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF;  sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;  sum += dst & 0xFFFF;
    sum += proto;
    sum += (uint32_t)len;

    const uint8_t* p = (const uint8_t*)data;
    size_t n = len;
    while (n > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        n -= 2;
    }
    if (n) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

char* ip_to_str(uint32_t ip, char* buf) {
    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        unsigned v = (ip >> (i * 8)) & 0xFF;
        if (v >= 100) buf[pos++] = (char)('0' + v / 100);
        if (v >= 10)  buf[pos++] = (char)('0' + (v / 10) % 10);
        buf[pos++] = (char)('0' + v % 10);
        if (i) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    return buf;
}

int ip_from_str(const char* s, uint32_t* out) {
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || v > 255) return -1;
            s++;
        }
        ip = (ip << 8) | v;
        if (part < 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    if (*s != '\0') return -1;
    *out = ip;
    return 0;
}
