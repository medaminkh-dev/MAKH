/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * Build identity (UFUZZ U6): the source revision this kernel was built from,
 * `git describe --always --dirty` at build time, so every crash artifact can
 * name the exact build it came from. The Makefile recompiles this file only
 * when the identity changes (build/buildid.txt).
 */
#ifndef MAKH_BUILD_ID
#define MAKH_BUILD_ID "unknown"
#endif
const char makh_build_id[] = MAKH_BUILD_ID;
