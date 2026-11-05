# UFUZZ ABI inventory (U0)

This directory holds the **U0 — ABI Inventory & Contract** deliverable for
MAKH 1.4.0 / UFUZZ: a precise description of the MAKH Ring-3 syscall ABI that
the fuzzer treats as ground truth.

## Files

| File | Role |
|------|------|
| `makh_syscalls.json` | **Single source of truth.** Machine-readable contract for every dispatched syscall: number, impl level, argument kinds/directions/size rules, resource create/consume/release rules, errno, side effects, concurrency notes. |
| `check_abi.py` | Consistency checker **and** doc generator. Verifies the JSON against the live kernel table and regenerates `docs/UFUZZ_ABI.md`. |
| `../../docs/UFUZZ_ABI.md` | **Generated** human-readable view. Do not hand-edit. |

## The contract (what the checker enforces)

`check_abi.py` parses the real kernel table — syscall numbers from
`kernel/include/syscall.h` and the dispatched `case SYS_*:` labels from
`kernel/syscall/syscall.c` — and fails (non-zero exit) if any of the U0
acceptance gates is violated:

1. every documented, supported syscall is actually dispatched (no
   "supported syscall without a contract", and no `unsupported` entry that is
   in fact dispatched);
2. every dispatched syscall has a contract in the JSON;
3. every syscall number matches the kernel header exactly;
4. every pointer argument carries a **direction** (`in`/`out`/`inout`) and a
   **size/length rule**;
5. every call states its **resource** create/consume/release rules explicitly;
6. `docs/UFUZZ_ABI.md` is in sync with the JSON.

## Usage

```sh
make abi-check                      # full check (CI gate)
python3 fuzz/abi/check_abi.py       # same, run directly
python3 fuzz/abi/check_abi.py --emit-doc   # regenerate docs/UFUZZ_ABI.md
```

The check runs in CI as the `abi-check` job (pure Python, no toolchain), so the
inventory can never silently drift from the kernel it describes.
