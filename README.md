# MakhOS - Phase 8 Stable Branch

## The Story

We hit a wall in Phase 13.

The `main` branch became unstable — not because the code was wrong, but because we kept piling new features (syscalls, IPC, etc.) on top of a **fragile process management foundation**. Every fix created two new bugs. We were stuck in technical debt.

So we stopped.

We stepped back to Phase 8 — the last point before everything broke. Then we rebuilt Phase 9 (Process Management) from scratch, but this time:

- **Clean separation** between lists (all_next vs ready_next)
- **Proper idle/init processes** with static stacks
- **Context switch** that actually works (20+ switches and returns to kernel)
- **No hacks**, no quick fixes

Now `phase8-stable` is our new foundation. Solid. Tested. Ready.

## What's Next

We'll now implement **Phases 11, 12, 13** on this clean base:

- **Phase 11**: Process tree, priorities, sleep/wake
- **Phase 12**: Signals, advanced scheduling
- **Phase 13**: IPC, pipes, message queues

Once these are stable, we'll **merge back Phase 10 (usermode)** from `main` — because usermode needs a solid kernel underneath.

## The Lesson

Sometimes you have to go backward to move forward.
