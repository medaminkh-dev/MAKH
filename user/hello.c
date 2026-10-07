/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include "usys.h"
int umain(void) {
    uwrite(1, "hi from user\n", 13);
    return 42;                      /* exit code */
}
