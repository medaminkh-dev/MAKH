/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/path.c
 * Path canonicalisation (Phase 20-C). See path_canonicalize() in fs/vfs.h.
 *
 * This is where relative paths and the per-process cwd become an absolute path
 * the VFS can walk. It is deliberately pure string work with no filesystem
 * access, so it is cheap, reentrant, and easy to fuzz — which matters, because
 * an off-by-one here is a classic buffer overflow. The KFUZZ "path" target
 * hammers it with random (cwd, path) pairs and asserts the output always stays
 * inside the buffer and is a well-formed absolute path.
 */

#include <fs/vfs.h>
#include <errno.h>

#define PATH_MAX_COMPS 64   /* deepest path we canonicalise */

int path_canonicalize(const char* cwd, const char* path, char* out, size_t outsz) {
    if (!path || !out || outsz < 2) return -EINVAL;

    size_t end[PATH_MAX_COMPS];   /* end[k] = out length right after component k */
    int ncomp = 0;
    out[0] = '/';
    size_t len = 1;               /* out currently holds "/" */

    /* An absolute path ignores cwd; a relative one starts from it. */
    const char* sources[2];
    int nsrc = 0;
    if (path[0] != '/') sources[nsrc++] = (cwd && cwd[0]) ? cwd : "/";
    sources[nsrc++] = path;

    for (int si = 0; si < nsrc; si++) {
        const char* p = sources[si];
        while (*p) {
            while (*p == '/') p++;                 /* skip separators */
            if (!*p) break;

            const char* st = p;                     /* one component [st, p) */
            while (*p && *p != '/') p++;
            size_t clen = (size_t)(p - st);

            if (clen == 1 && st[0] == '.') continue;            /* "." */
            if (clen == 2 && st[0] == '.' && st[1] == '.') {    /* ".." */
                if (ncomp > 0) { ncomp--; len = ncomp ? end[ncomp - 1] : 1; }
                continue;                                        /* at root: stay */
            }

            if (ncomp >= PATH_MAX_COMPS) return -ERANGE;
            /* Append "/name", but no extra '/' right after the root. */
            size_t need = (len == 1 ? 0 : 1) + clen;
            if (len + need + 1 > outsz) return -ERANGE;          /* +1 for NUL */
            if (len != 1) out[len++] = '/';
            for (size_t i = 0; i < clen; i++) out[len++] = st[i];
            end[ncomp++] = len;
        }
    }

    out[len] = '\0';
    return 0;
}
