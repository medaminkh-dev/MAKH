/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - krandom.c
 * xoshiro256** stream, seeded via splitmix64 from the TSC. The state mutation
 * runs with interrupts off so a timer preemption can't interleave two draws,
 * and each draw folds in a fresh rdtsc() — a cheap moving entropy input so the
 * sequence isn't a pure function of the boot-time seed. See krandom.h.
 */
#include <krandom.h>
#include <arch/cpu.h>      /* rdtsc */
#include <irq.h>
#include <lib/string.h>    /* memcpy */

static uint64_t s[4];
static int      seeded;

/* splitmix64 spreads one seed word across the 256-bit state (its whole job). */
static uint64_t splitmix64(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

uint64_t krandom_u64(void) {
    irqflags_t f = local_irq_save();
    if (!seeded) {
        uint64_t x = rdtsc() ^ 0xA5A5A5A5DEADBEEFULL;
        for (int i = 0; i < 4; i++) s[i] = splitmix64(&x);
        seeded = 1;
    }
    s[0] ^= rdtsc();                       /* fold in fresh entropy each draw */

    const uint64_t result = rotl(s[1] * 5, 7) * 9;   /* xoshiro256** scrambler */
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    local_irq_restore(f);
    return result;
}

void krandom_bytes(void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        uint64_t r = krandom_u64();
        size_t chunk = n < sizeof(r) ? n : sizeof(r);
        memcpy(p, &r, chunk);
        p += chunk;
        n -= chunk;
    }
}
