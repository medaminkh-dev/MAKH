# MakhOS Phase 18: VFS + tmpfs + devfs + initrd

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a virtual filesystem with a RAM root, device nodes, a
tar initrd, a descriptor layer and `open`/`read`/`write`/`close`/`lseek`
syscalls; 101 in-kernel tests, fuzzed, stress clean
**Depends on:** Phase 16 (syscalls, uaccess), Phase 17 (heap/refcounts)

---

## 1. Scope

This phase gives the kernel a real filesystem interface, entirely in RAM so it
is testable in one boot:

```
fd (per process) -> struct file (offset, flags) -> vnode -> vfs_ops -> tmpfs / devfs
```

Delivered and tested: **mount**, **open/read/write/close/lseek**, directories
and **path lookup**, **readdir**, **device files**, and an **initrd** unpacked
from a tar the bootloader hands us.

**Deferred to Phase 19** (the persistence half of the prompt): the
**virtio-blk** driver, a **block/buffer cache**, **ext2** read/write, and the
cross-boot **"file persists after reboot"** test. Those need a real block
device, a disk image, and a two-boot test harness — the natural next brick. The
VFS here is designed so a disk filesystem slots in as just another `vfs_ops`.

---

## 2. The VFS core (`fs/vfs.c`)

- **`vnode`** — a filesystem-agnostic handle (type, `vfs_ops`, private data,
  size, refcount, and an optional `mounted` filesystem root).
- **`vfs_ops`** — `read`, `write`, `lookup`, `create`, `readdir`, `unlink`,
  `truncate`. A filesystem fills in what it supports.
- **Path resolution** — `vfs_resolve("/a/b/c")` walks components from the root,
  crossing mount points transparently (`vnode->mounted`). `vfs_resolve_parent`
  returns the parent directory and the final name, for create/unlink.
- **Mount** — `vfs_mount(path, root)` attaches a filesystem's root vnode onto a
  directory; resolution descends into it.
- **Descriptor layer** — a lazily-allocated `file*` table hangs off each
  process (`process_t.fd_table`). `open` resolves (optionally creating), takes a
  reference and returns the lowest free fd (≥ 3; 0/1/2 are console stdio);
  `read`/`write` advance the per-fd offset; `lseek`, `close`.

## 3. tmpfs (`fs/tmpfs.c`)

The root filesystem, in RAM. Directories are singly linked lists of named
children; regular files are a heap buffer that doubles on write. Implements the
full `vfs_ops`, including `unlink` (which refuses non-empty directories and
frees a vnode at its last reference). `tmpfs_link` lets devfs and the initrd
graft vnodes straight in.

## 4. devfs (`fs/devfs.c`)

Four character devices, mounted at `/dev`:

| path | read | write |
|---|---|---|
| `/dev/null` | EOF | discarded |
| `/dev/zero` | zeros | discarded |
| `/dev/console` | — | to the terminal |
| `/dev/urandom` | pseudo-random bytes | — |

They are `VNODE_CHR` vnodes with their own `read`/`write`, linked into the
`/dev` tmpfs directory.

## 5. initrd (`fs/tar.c`)

GRUB loads `initrd.tar` as a multiboot2 **module**; at boot the kernel finds the
module tag and `tar_load_initrd()` unpacks the USTAR archive into the root
tmpfs, creating parent directories as needed. The parser is strict about
bounds — it validates the `ustar` magic and never reads past the buffer — so a
truncated or malformed archive stops the load rather than running off the end.
The build tars the `initrd/` directory (`/etc/motd`, `/hello.txt`) into the ISO.

## 6. Syscalls

`open`(2), `read`(0), `write`(1), `close`(3), `lseek`(8) — Linux x86_64 numbers.
`write`/`read` route fds 0–2 to the console and everything else to the VFS;
`open` copies the path safely from user memory (`copy_from_user`, bounded).

## 7. Fuzzed

Per this kernel's habit, a KFUZZ **`vfs`** target throws random
create/open/write/read/lseek/unlink/readdir at a scratch directory **and feeds
the tar parser random bytes** — a classic parser-hardening target. The global
oracles (heap walker, rejected-free counter, process-tree check) run after every
run, and the ring-0 sandbox catches any fault with a reproducing seed. A
30,000-iteration campaign found no crashes and no heap inconsistencies.

## 8. Tests (`kernel/tests/test_vfs.c`)

`create/write/read/close` (and re-open), directories + path lookup (including
missing paths), append + `lseek` growth, `unlink`, `readdir`, the four device
nodes, the **initrd** files (`/etc/motd`, `/hello.txt`), fd-table limits, and
the **syscall path** (`open`/`write`/`lseek`/`read`/`close`, plus reading the
initrd through the syscall layer). **101 in-kernel tests pass**; stress clean.

## 9. Deferred to Phase 19

| Item | Why it waits |
|---|---|
| virtio-blk driver | needs a QEMU disk image attached to the test VM |
| buffer/page cache for blocks | pairs with the block driver |
| ext2 read/write | the on-disk filesystem the prompt asks for |
| "persists after reboot" test | needs a persistent disk + a two-boot harness |

The VFS is the seam: ext2 will be another `vfs_ops` mounted over a block
device, with no change to path lookup, the fd layer, or the syscalls.
