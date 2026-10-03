/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - kfuzz_targets.c
 * The attack surfaces KFUZZ drives. Each target takes a seeded PRNG and runs a
 * bounded sequence of random-but-valid operations against one subsystem, with
 * its own local oracles on top of the global ones (heap walker, process tree).
 *
 * These functions are deliberately NOT coverage-instrumented (the Makefile
 * instruments the *subjects* they call, not the harness), so the coverage map
 * measures the kernel under test, not the fuzzer.
 */

#include <kfuzz.h>
#include <kernel.h>
#include <klog.h>
#include <sched.h>
#include <pthread.h>
#include <semaphore.h>
#include <ktime.h>
#include <shell.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <net/net.h>

/* -------------------------------------------------------------------------- */
/* 1. Heap: random malloc/free/realloc/calloc with overlap + payload checks    */
/* -------------------------------------------------------------------------- */

#define HEAP_LIVE 48

static int t_heap(kfuzz_rng_t* r, uint32_t iters) {
    struct { void* p; size_t n; uint8_t tag; } live[HEAP_LIVE];
    for (int i = 0; i < HEAP_LIVE; i++) live[i].p = NULL;

    for (uint32_t i = 0; i < iters; i++) {
        int slot = (int)kfuzz_rand_below(r, HEAP_LIVE);
        uint32_t op = kfuzz_rand_below(r, 10);

        if (live[slot].p && op < 3) {
            /* verify the payload we stamped is intact, then free */
            uint8_t* p = live[slot].p;
            for (size_t k = 0; k < live[slot].n; k++)
                if (p[k] != live[slot].tag) return -1;   /* someone wrote into us */
            kfree(live[slot].p);
            live[slot].p = NULL;
        } else if (live[slot].p && op < 5) {
            size_t nn = 1 + kfuzz_rand_below(r, 4000);
            uint8_t* p = krealloc(live[slot].p, nn);
            if (p) {
                size_t keep = nn < live[slot].n ? nn : live[slot].n;
                for (size_t k = 0; k < keep; k++)
                    if (p[k] != live[slot].tag) return -1;   /* realloc lost data */
                live[slot].p = p;
                live[slot].n = nn;
                live[slot].tag = (uint8_t)kfuzz_rand(r);
                memset(p, live[slot].tag, nn);
            }
        } else {
            if (live[slot].p) { kfree(live[slot].p); live[slot].p = NULL; }
            size_t nn = 1 + kfuzz_rand_below(r, 4000);
            uint8_t tag = (uint8_t)kfuzz_rand(r);
            uint8_t* p = (op & 1) ? kcalloc(1, nn) : kmalloc(nn);
            if (p) {
                if (op & 1) for (size_t k = 0; k < nn; k++)
                    if (p[k] != 0) return -1;               /* calloc didn't zero */
                memset(p, tag, nn);
                live[slot].p = p; live[slot].n = nn; live[slot].tag = tag;
            }
        }
    }
    for (int i = 0; i < HEAP_LIVE; i++) if (live[i].p) kfree(live[i].p);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 2. PMM: random page alloc/free, uniqueness + alignment                      */
/* -------------------------------------------------------------------------- */

#define PMM_LIVE 32

static int t_pmm(kfuzz_rng_t* r, uint32_t iters) {
    void* live[PMM_LIVE];
    for (int i = 0; i < PMM_LIVE; i++) live[i] = NULL;

    for (uint32_t i = 0; i < iters; i++) {
        int slot = (int)kfuzz_rand_below(r, PMM_LIVE);
        if (live[slot]) {
            pmm_free_page(live[slot]);
            live[slot] = NULL;
        } else {
            void* p = pmm_alloc_page();
            if (!p) continue;
            if ((uintptr_t)p & (PAGE_SIZE - 1)) return -1;   /* not page aligned */
            for (int j = 0; j < PMM_LIVE; j++)
                if (j != slot && live[j] == p) return -1;    /* handed out twice */
            live[slot] = p;
        }
    }
    for (int i = 0; i < PMM_LIVE; i++) if (live[i]) pmm_free_page(live[i]);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 3. String/mem: bounds + a reference oracle for memmove overlap              */
/* -------------------------------------------------------------------------- */

static int t_string(kfuzz_rng_t* r, uint32_t iters) {
    enum { N = 256, GUARD = 16 };
    uint8_t* buf = kmalloc(N + 2 * GUARD);
    uint8_t* ref = kmalloc(N + 2 * GUARD);
    int fail = 0;
    if (!buf || !ref) { kfree(buf); kfree(ref); return 0; }
    uint8_t* a = buf + GUARD;
    uint8_t* b = ref + GUARD;

    for (uint32_t i = 0; i < iters && !fail; i++) {
        memset(buf, 0xAA, GUARD); memset(buf + GUARD + N, 0xAA, GUARD);   /* guards */
        kfuzz_fill(r, a, N);
        memcpy(b, a, N);                                    /* reference copy */

        uint32_t off1 = kfuzz_rand_below(r, N);
        uint32_t off2 = kfuzz_rand_below(r, N);
        uint32_t len  = kfuzz_rand_below(r, N - (off1 > off2 ? off1 : off2));

        switch (kfuzz_rand_below(r, 5)) {
            case 0: {
                memmove(a + off1, a + off2, len);
                if (off1 < off2) { for (uint32_t k = 0; k < len; k++) b[off1+k] = b[off2+k]; }
                else             { for (uint32_t k = len; k-- > 0; )  b[off1+k] = b[off2+k]; }
                if (memcmp(a, b, N) != 0) fail = 1;         /* memmove botched overlap */
                break;
            }
            case 1:
                memset(a + off1, (int)kfuzz_rand(r), len);
                break;
            case 2: {
                /* NUL-terminate first: strlen on a non-terminated buffer would
                 * run off the end (that would be a bug in this harness, not in
                 * strlen). Place the terminator at a random spot in range. */
                a[off1] = 0;
                size_t l = strlen((char*)a);
                if (l != off1 && a[l] != 0) fail = 1;
                if (l > N) fail = 1;
                break;
            }
            case 3: {
                a[off1] = 0;
                char* q = strchr((char*)a, 0);
                if (q != (char*)a + strlen((char*)a)) fail = 1;
                break;
            }
            case 4:
                memcpy(a + off1, b + off2, len);
                if (memcmp(a + off1, b + off2, len) != 0) fail = 1;
                break;
        }
        for (int k = 0; k < GUARD; k++)
            if (buf[k] != 0xAA || buf[GUARD + N + k] != 0xAA) fail = 1;   /* overrun */
    }
    kfree(buf); kfree(ref);
    return fail ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
/* 4. pthread/sem: create/join/detach + mutex/sem under contention             */
/* -------------------------------------------------------------------------- */

static void* pth_noop(void* a) { return a; }

static int t_pthread(kfuzz_rng_t* r, uint32_t iters) {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    sem_t s; sem_init(&s, 0, kfuzz_rand_below(r, 3));

    for (uint32_t i = 0; i < iters; i++) {
        switch (kfuzz_rand_below(r, 4)) {
            case 0: {
                pthread_t t;
                if (pthread_create(&t, NULL, pth_noop, (void*)(long)i) == 0) {
                    if (kfuzz_rand(r) & 1) pthread_detach(t);
                    else pthread_join(t, NULL);
                }
                break;
            }
            case 1:
                if (pthread_mutex_trylock(&m) == 0) pthread_mutex_unlock(&m);
                break;
            case 2:
                pthread_mutex_lock(&m);
                pthread_mutex_unlock(&m);
                break;
            case 3:
                if (sem_trywait(&s) == 0) { /* took it */ }
                else sem_post(&s);
                break;
        }
    }
    /* drain the semaphore so it destroys cleanly */
    while (sem_trywait(&s) == 0) { }
    sem_destroy(&s);
    pthread_mutex_destroy(&m);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 5. Shell: arbitrary bytes as a command line must never crash                */
/* -------------------------------------------------------------------------- */

/* Parser-stressing vocabulary. Deliberately NO reachable/unreachable gateway
 * IPs: `ping 10.0.2.2` or `ping 1.2.3.4` would block for seconds on ARP/echo
 * timeouts (the watchdog rightly flags that as a hang). Only fast commands and
 * loopback go here; ping's real timeout paths are covered by dedicated tests. */
static const char* shell_words[] = {
    "help","mem","ps","arp","netstat","ifconfig","echo","heapcheck","selftestx",
    "127.0.0.1","ping","-1","999999999999","", "clear","uptime","0","x",
};

static int t_shell(kfuzz_rng_t* r, uint32_t iters) {
    char line[SHELL_MAX_LINE];
    for (uint32_t i = 0; i < iters; i++) {
        uint32_t len = 0;
        if (kfuzz_rand(r) & 1) {
            /* structured: real words separated by spaces */
            int words = 1 + (int)kfuzz_rand_below(r, 4);
            for (int w = 0; w < words && len < SHELL_MAX_LINE - 24; w++) {
                const char* tok = shell_words[kfuzz_rand_below(r,
                                    sizeof(shell_words)/sizeof(shell_words[0]))];
                for (const char* p = tok; *p && len < SHELL_MAX_LINE - 2; p++)
                    line[len++] = *p;
                line[len++] = ' ';
            }
        } else {
            /* raw random bytes (printable-ish so the line editor is exercised) */
            len = kfuzz_rand_below(r, SHELL_MAX_LINE - 1);
            for (uint32_t k = 0; k < len; k++)
                line[k] = (char)(0x20 + kfuzz_rand_below(r, 0x5f));
        }
        line[len] = '\0';
        shell_exec(line);          /* only requirement: it returns */
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 6. Network RX parsers: hand the stack malformed frames on loopback          */
/* -------------------------------------------------------------------------- */
/*
 * net_input() runs the whole receive path (Ethernet -> ARP/IPv4 ->
 * ICMP/UDP/TCP). Feeding it random and mutated frames must never fault or
 * corrupt state, no matter how truncated or inconsistent the headers are.
 */

static volatile int net_held;     /* 1 => this target holds net_lock */

static void t_netrx_cleanup(void) {
    if (net_held) { net_held = 0; net_unlock(); }
}

/* A plausible frame skeleton the mutator starts from: Ethernet + IPv4 + TCP. */
static void build_seed_frame(uint8_t* f, size_t* len) {
    static const uint8_t tmpl[] = {
        /* eth: dst broadcast, src, type=0800 */
        0,0,0,0,0,0, 0x52,0x54,0,0x12,0x34,0x56, 0x08,0x00,
        /* ipv4: v4 ihl5, ..., proto=6 TCP, src 127.0.0.1 dst 127.0.0.1 */
        0x45,0,0,40, 0,0,0x40,0, 0x40,0x06,0,0,
        127,0,0,1, 127,0,0,1,
        /* tcp: sport dport seq ack off flags win csum urg */
        0x30,0x39,0x1f,0x90, 0,0,0,1, 0,0,0,0, 0x50,0x02,0xff,0xff, 0,0,0,0,
    };
    memcpy(f, tmpl, sizeof(tmpl));
    *len = sizeof(tmpl);
}

static int t_netrx(kfuzz_rng_t* r, uint32_t iters) {
    netdev_t* lo = netdev_by_name("lo");
    if (!lo) return 0;

    enum { FMAX = 128 };
    uint8_t frame[FMAX];

    for (uint32_t i = 0; i < iters; i++) {
        size_t len;
        uint32_t mode = kfuzz_rand_below(r, 3);
        if (mode == 0) {
            len = kfuzz_rand_below(r, FMAX);      /* pure random */
            kfuzz_fill(r, frame, len);
        } else {
            build_seed_frame(frame, &len);        /* mutate a valid frame */
            uint32_t flips = 1 + kfuzz_rand_below(r, 8);
            for (uint32_t k = 0; k < flips && len; k++)
                frame[kfuzz_rand_below(r, (uint32_t)len)] ^= (uint8_t)kfuzz_rand(r);
            if (mode == 2 && len) len = kfuzz_rand_below(r, (uint32_t)len + 1);  /* truncate */
        }

        net_lock();
        net_held = 1;
        net_input(lo, frame, len);
        net_held = 0;
        net_unlock();
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 7. Fault injection: proves the ring-0 sandbox actually recovers             */
/* -------------------------------------------------------------------------- */
/*
 * Not part of KFUZZ_T_ALL - it always faults on purpose. A test runs it via
 * kfuzz_replay() and asserts the crash was caught (rc == 1) and the kernel is
 * still alive afterwards.
 */
static int t_fault(kfuzz_rng_t* r, uint32_t iters) {
    (void)r; (void)iters;
    /* Canonical high-half address that is not mapped -> page fault (#PF). */
    volatile uint64_t* wild = (volatile uint64_t*)0xFFFFFF0000000000ull;
    *wild = 0xBADC0FFEE;
    return 0;   /* unreachable: the store faults and the sandbox unwinds */
}

/* -------------------------------------------------------------------------- */
/* Registry                                                                   */
/* -------------------------------------------------------------------------- */

static const kfuzz_target_t g_targets[] = {
    { "heap",    t_heap,    KFUZZ_T_HEAP,    NULL },
    { "pmm",     t_pmm,     KFUZZ_T_PMM,     NULL },
    { "string",  t_string,  KFUZZ_T_STRING,  NULL },
    { "pthread", t_pthread, KFUZZ_T_PTHREAD, NULL },
    { "shell",   t_shell,   KFUZZ_T_SHELL,   NULL },
    { "netrx",   t_netrx,   KFUZZ_T_NETRX,   t_netrx_cleanup },
    { "fault",   t_fault,   KFUZZ_T_FAULT,   NULL },
};

const kfuzz_target_t* kfuzz_targets(int* count) {
    *count = (int)(sizeof(g_targets) / sizeof(g_targets[0]));
    return g_targets;
}
