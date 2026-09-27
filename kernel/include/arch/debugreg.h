/**
 * MakhOS - debugreg.h
 * x86 hardware watchpoints (DR0-DR3 / DR7).
 *
 * Four address slots that trap (#DB, vector 1) when the CPU touches them.
 * Unlike a canary, which only says *that* memory was trampled, a watchpoint
 * stops on the exact instruction that did it - the tool of choice when the
 * self-fuzzer or a test finds a stray write into a live object.
 */

#ifndef MAKHOS_DEBUGREG_H
#define MAKHOS_DEBUGREG_H

#include <types.h>

#define HWW_WRITE      1   /* trap on data writes            */
#define HWW_READWRITE  3   /* trap on data reads or writes   */

/* Arm slot 0..3 on `addr` (len 1, 2, 4 or 8; addr must be len-aligned).
 * Returns 0, or -1 on a bad slot/len/alignment. */
int  hw_watch_set(int slot, const void* addr, int len, int kind);
void hw_watch_clear(int slot);

#endif /* MAKHOS_DEBUGREG_H */
