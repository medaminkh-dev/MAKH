/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* A second translation unit, so `make` has to compile two objects and link
 * them — the point of F21-c is a real multi-file build driven by make. */
#include "greet.h"

const char* greeting(void) {
    return "hello from a make-built multi-file program on MAKH";
}

int answer(void) {
    return 42;
}
