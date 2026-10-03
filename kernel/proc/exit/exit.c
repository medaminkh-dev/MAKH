/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include <proc_internal.h>
#include <sched.h>
#include <kernel.h>

/**
 * =============================================================================
 * exit.c - Process termination (Phase 12)
 * =============================================================================
 * proc_exit() is the legacy entry point; the real work (reparent children,
 * detach from parent, become a zombie, wake joiners, reschedule) lives in the
 * scheduler's thread_exit() so there is a single, well-tested exit path.
 * =============================================================================
 */

void proc_exit(int code) {
    thread_exit(code);
}
