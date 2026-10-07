/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* sigprocmask ordering: a blocked signal stays pending (the handler does not
 * run) until it is unblocked. We build a decimal sequence — the mainline digit
 * 1 must land before the handler's digit 2 — so exiting 12 proves the block
 * held and the unblock delivered. */
#include "usys.h"

static volatile int order = 0;
static void onsig(int s) { (void)s; order = order * 10 + 2; }

int umain(void) {
    usignal(SIGTERM, onsig);
    usigprocmask(SIG_BLOCK, sigmask(SIGTERM), 0);
    uraise(SIGTERM);                 /* pending but blocked: handler waits */
    order = order * 10 + 1;          /* mainline runs first -> 1 */
    usigprocmask(SIG_UNBLOCK, sigmask(SIGTERM), 0);  /* now it is delivered -> 2 */
    return order;                    /* 12 */
}
