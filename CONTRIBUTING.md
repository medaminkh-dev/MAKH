# Contributing to MakhOS

Thanks for wanting to help. MakhOS grows **brick by brick**: one phase at a
time, each closed only when its tests are green.

## Before your first pull request

Sign the [CLA](CLA.md) by adding the agreement line to your PR description.
It keeps the project dual-licensable ([LICENSING.md](LICENSING.md)); you keep
the copyright to your work.

## Ground rules

1. **Tests first.** New behaviour comes with `KTEST`s. `make test` must pass,
   and concurrency changes should survive `make stress`.
2. **Every source file carries the header**
   ```c
   /* SPDX-License-Identifier: AGPL-3.0-only */
   /* Copyright (C) 2026 Amine Khemissi */
   ```
   `make check-license` verifies this (CI runs it too).
3. **Match the style around you**: comment banners explaining *why*,
   `KLOG_*` for logging, `-1` + `errno` for POSIX-style APIs.
4. **Commits** follow `feat(scope): ...`, `fix(scope): ...`, `docs: ...`.

## Workflow

```sh
make            # build
make test       # headless in-kernel test suite
make smoke      # boot and drive the shell through the keyboard
make stress     # whole suite many times in parallel
```
