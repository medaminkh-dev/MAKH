/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* clear: wipe the screen and home the cursor (ANSI erase-display + home). */
#include "usys.h"

int umain(void) {
    uwrite(1, "\033[2J\033[H", 7);
    return 0;
}
