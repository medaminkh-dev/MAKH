/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_path.c
 * Phase 20-C: path canonicalisation + per-process cwd. The canonicaliser is a
 * pure function, so it is unit-tested exhaustively; the cwd/relative-path
 * syscalls are then checked end to end through a real ring-3 program.
 */

#include <ktest.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <errno.h>
#include <proc_internal.h>

static int canon_is(const char* cwd, const char* path, const char* want) {
    char out[128];
    if (path_canonicalize(cwd, path, out, sizeof(out)) != 0) return 0;
    return strcmp(out, want) == 0;
}

KTEST(path, relative_joins_the_cwd) {
    KEXPECT(canon_is("/", "foo/bar", "/foo/bar"));
    KEXPECT(canon_is("/a/b", "c", "/a/b/c"));
    KEXPECT(canon_is("/bin", "", "/bin"));           /* empty path -> cwd */
    KEXPECT(canon_is("/bin", ".", "/bin"));
}

KTEST(path, absolute_ignores_the_cwd) {
    KEXPECT(canon_is("/a/b", "/x/y", "/x/y"));
    KEXPECT(canon_is("/deep/dir", "/", "/"));
}

KTEST(path, dotdot_pops_and_never_escapes_root) {
    KEXPECT(canon_is("/a/b", "../c", "/a/c"));
    KEXPECT(canon_is("/", "/x/../y", "/y"));
    KEXPECT(canon_is("/", "a/b/c/../../d", "/a/d"));
    KEXPECT(canon_is("/", "..", "/"));               /* can't climb above / */
    KEXPECT(canon_is("/", "../../../x", "/x"));
    KEXPECT(canon_is("/bin", "/a/b/..", "/a"));
}

KTEST(path, collapses_redundant_separators_and_dots) {
    KEXPECT(canon_is("/", "///a////b", "/a/b"));
    KEXPECT(canon_is("/", "/bin/./../bin", "/bin"));
    KEXPECT(canon_is("/x", "./././y", "/x/y"));
}

KTEST(path, overflow_is_erange_not_a_write) {
    char out[8];
    KEXPECT_EQ(path_canonicalize("/", "aaaaaaaaaaaaaaaa", out, sizeof(out)), -ERANGE);
    KEXPECT_EQ(path_canonicalize("/", "x", (char*)0, 8), -EINVAL);
}

/* End to end: cwdtest does getcwd / chdir / relative open / ".." in ring 3. */
KTEST(path, user_cwd_and_relative_paths) {
    int pid = proc_spawn_user("/bin/cwdtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);               /* nonzero => which step failed */
}
