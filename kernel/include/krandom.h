/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - krandom.h
 * A small kernel pseudo-random source for getrandom(2) and internal use.
 * Well-distributed (xoshiro256**), lazily seeded from the TSC and re-stirred
 * with a fresh TSC read on every draw. Not crypto-grade — good enough for stack
 * canaries, ASLR jitter and malloc hardening until a real entropy pool exists.
 */
#ifndef MAKHOS_KRANDOM_H
#define MAKHOS_KRANDOM_H

#include <types.h>

uint64_t krandom_u64(void);
void     krandom_bytes(void* buf, size_t n);

#endif /* MAKHOS_KRANDOM_H */
