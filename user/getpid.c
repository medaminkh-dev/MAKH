/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include "usys.h"
int umain(void) { return (int)(ugetpid() & 0x7f); }   /* exit code = own pid */
