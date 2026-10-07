# MakhOS Phase 20-C: working directory & relative paths

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — per-process cwd, `chdir`/`getcwd`, and relative path
resolution; the canonicaliser is fuzzed; ~132 in-kernel tests
**Depends on:** Phase 18 (VFS), Phase 20-A (user processes)

---

## 1. Scope

Until now the VFS resolved **absolute paths only**. A shell — and musl — need a
current working directory and relative names. This phase adds:

- a **per-process cwd**, inherited as a canonical absolute path;
- **`chdir(path)`** and **`getcwd(buf, size)`**;
- **relative path resolution** for `open` (and anything that resolves a path
  from a user process), so `open("hello")` from `/bin` finds `/bin/hello`.

The design keeps the VFS itself absolute-only; all the cwd/relative logic lives
in one pure, fuzzable string function at the syscall boundary.

---

## 2. The canonicaliser (`fs/path.c`)

`path_canonicalize(cwd, path, out, outsz)` turns `(cwd, path)` into an
absolute, canonical path:

- an **absolute** `path` ignores `cwd`; a **relative** one is taken from it;
- `.` is dropped, `..` pops the previous component (and **never climbs above
  `/`**), and runs of `/` collapse;
- the result is absolute, has no `.`/`..`/`//`, and is always NUL-terminated
  inside `out`.

It is **pure string work with no filesystem access** — cheap, reentrant, and,
crucially, easy to fuzz: an off-by-one here is a textbook buffer overflow.
Components are tracked by an end-offset stack so `..` is an O(1) pop, and both
the component count and the output length are bounds-checked, returning
`-ERANGE` rather than writing past `out[outsz-1]`.

## 3. Per-process cwd and the syscalls (`syscall.c`)

Each process carries its cwd as a canonical absolute string in the PCB
(`cwd[256]`), initialised to `/`. Three paths change:

- **`open`** now canonicalises the user path against the process cwd before
  handing an absolute path to the VFS. A non-user (kernel) caller passes its
  path straight through, so the boot self-check and tests are unaffected.
- **`chdir(path)`** canonicalises, resolves the result, requires a directory
  (`-ENOTDIR` otherwise, `-ENOENT` if missing), and stores it as the new cwd.
- **`getcwd(buf, size)`** copies the cwd out, returning its length including the
  NUL, or `-ERANGE` if the buffer is too small.

`chdir`/`getcwd` are syscalls 80/79 (Linux x86-64). A user path is copied in
with the one-byte-at-a-time `copy_from_user` guard, so a bad or overrunning
pointer is `-EFAULT`/`-ENAMETOOLONG`, never a kernel fault.

## 4. Fuzzed (`path` target)

A KFUZZ **`path`** target builds random `cwd`/`path` strings from an alphabet
dense in `/` and `.` (so `..`, `/./` and `//` appear constantly) and runs them
into a **guarded** buffer sized to the declared limit. It asserts the guard
bytes just past the limit are never touched, and that a successful result is
absolute and NUL-terminated in bounds. The all-targets campaign includes it.

## 5. Tests (`kernel/tests/test_path.c`)

Unit tests pin the canonicaliser's behaviour:

- **`relative_joins_the_cwd`**, **`absolute_ignores_the_cwd`**;
- **`dotdot_pops_and_never_escapes_root`** — incl. `..` at `/` staying at `/`;
- **`collapses_redundant_separators_and_dots`** — `///a////b` → `/a/b`,
  `/bin/./../bin` → `/bin`;
- **`overflow_is_erange_not_a_write`** — a too-small buffer is `-ERANGE`, a NULL
  out is `-EINVAL`.

End to end, through a real ring-3 program:

- **`user_cwd_and_relative_paths`** (`/bin/cwdtest`) — `getcwd` is `/`, `chdir
  /bin` then a **relative** `open("hello")` finds `/bin/hello`, `chdir ..`
  returns to `/`, and a messy `/bin/./../bin` canonicalises to `/bin`.

**~132 in-kernel tests pass**; `make stress` is clean.

## 6. Deferred

| Item | Why it waits |
|---|---|
| cwd inherited across `fork`/`execve` | the field is copyable; needs user `fork` (20-A-2) |
| `openat`/`*at` family (dirfd-relative) | needs an fd→vnode directory handle |
| symlink resolution in `..` | no symlinks in the filesystem yet |

With anonymous memory (20-B) and relative paths (20-C) in place, the remaining
gaps before a shell are the rest of the process model (`fork`/`execve`, the
keyboard→tty read path) and the wider syscall surface (`pipe`, `dup`, `ioctl`,
`getdents`, `stat`).
