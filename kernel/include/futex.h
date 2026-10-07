/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - futex.h
 * Fast user-space mutex support (Phase 20-L). A futex is a 32-bit word in user
 * memory; threads block in the kernel only on contention. Keyed by the word's
 * physical address, so it works across any threads that share the page.
 */
#ifndef MAKHOS_FUTEX_H
#define MAKHOS_FUTEX_H

#include <types.h>

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK       (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

void futex_init(void);
/* Block while *uaddr == val. 0 woken, -EAGAIN mismatch, -ETIMEDOUT, -EINTR. */
long futex_wait(uint64_t uaddr, uint32_t val, uint64_t timeout_ticks);
/* Wake up to `n` waiters on *uaddr. Returns the number woken. */
long futex_wake(uint64_t uaddr, int n);

#endif /* MAKHOS_FUTEX_H */
