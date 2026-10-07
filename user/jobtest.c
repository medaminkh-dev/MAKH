/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Exercise the job-control syscalls from ring 3 and exit 0 on success, or a
 * small code identifying the first step that failed (so a test can pinpoint). */
#include "usys.h"

int umain(void) {
    int me = (int)ugetpid();

    /* A freshly spawned process leads its own group: pgid == pid. */
    if ((int)ugetpgrp() != me) return 1;
    if ((int)ugetpgid(0) != me) return 2;

    /* setpgid(0,0) names a group after our own pid — a no-op here, but it must
     * succeed and leave pgid == pid. */
    if (usetpgid(0, 0) < 0) return 3;
    if ((int)ugetpgrp() != me) return 4;

    /* Hand the terminal to our group and read it back. */
    if (utcsetpgrp(0, me) < 0) return 5;
    if (utcgetpgrp(0) != me) return 6;

    /* ioctl on a non-terminal fd must be rejected. */
    int dummy = 0;
    if (uioctl(5, TIOCGPGRP, &dummy) >= 0) return 7;   /* expected -ENOTTY */

    return 0;
}
