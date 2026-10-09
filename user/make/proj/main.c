/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* The program `make` builds on MAKH (F21-c): it #includes a real libc header
 * and a sibling translation unit, so the build is genuinely multi-file and
 * libc-linked. printf goes through musl -> writev; the exit code 42 (from the
 * other object) is what the KTEST checks. */
#include <stdio.h>
#include "greet.h"

int main(void) {
    printf("%s\n", greeting());
    return answer();
}
