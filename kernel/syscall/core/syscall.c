#include <syscall/syscall.h>
#include <kernel.h>
#include <vga.h>
#include <drivers/timer.h>
#include <proc/fork.h>
#include <proc/pcb.h>

/**
 * =============================================================================
 * kernel/syscall/core/syscall.c - System Call Dispatch and Setup
 * =============================================================================
 * FIXES APPLIED:
 *   BUG#7:  Added SYS_WAIT (61) and SYS_WAITPID (7 repurposed, or new number)
 *           sys_waitpid() implemented to reap zombie children.
 *           Also added sys_exit() that correctly calls proc_exit().
 *
 *   BUG#2/3: syscall_asm.asm now uses kernel_syscall_rsp global.
 *            tss_set_kernel_stack() must also call set_kernel_syscall_rsp().
 *            We add that call in syscall_init() and expose the setter.
 *
 *   STAR MSR note: bits[63:48]=0x10 for future sysret compatibility.
 *   (We still use iretq so this doesn't affect correctness now.)
 * =============================================================================
 */

static syscall_fn_t syscall_table[MAX_SYSCALLS];

#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084

extern void syscall_entry(void);
extern void set_kernel_syscall_rsp(uint64_t rsp);  // from syscall_asm.asm
extern uint64_t kernel_syscall_rsp;                 // global in syscall_asm.asm

// Forward declarations
static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a4, uint64_t a5);
static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_sleep(uint64_t ms, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_getticks(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_getpriority(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_setpriority(uint64_t pid, uint64_t priority, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_waitpid(uint64_t pid, uint64_t status_ptr, uint64_t options, uint64_t a4, uint64_t a5);  // NEW

// =============================================================================
// MSR HELPERS
// =============================================================================

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// =============================================================================
// SYSCALL INITIALIZATION
// =============================================================================

void syscall_init(void) {
    terminal_writestring("[SYSCALL] Initializing system call interface...\n");

    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = NULL;
    }

    // Register syscalls
    syscall_table[SYS_EXIT]        = sys_exit;
    syscall_table[SYS_WRITE]       = sys_write;
    syscall_table[SYS_GETPID]      = sys_getpid;
    syscall_table[SYS_SLEEP]       = sys_sleep;
    syscall_table[SYS_GETTICKS]    = sys_getticks;
    syscall_table[SYS_GETPPID]     = sys_getppid;
    syscall_table[SYS_GETPRIORITY] = sys_getpriority;
    syscall_table[SYS_SETPRIORITY] = sys_setpriority;
    syscall_table[SYS_FORK]        = sys_fork;
    syscall_table[SYS_WAITPID]     = sys_waitpid;   // FIX BUG#7: NEW!
    syscall_table[SYS_WAIT]        = sys_waitpid;   // FIX BUG#7: alias

    // Enable SCE bit in EFER
    uint64_t efer = rdmsr(IA32_EFER);
    efer |= 0x01;
    wrmsr(IA32_EFER, efer);

    // STAR MSR:
    //   bits[47:32] = kernel CS selector (0x08) - used on syscall entry
    //   bits[63:48] = sysret CS base (0x10) - for future sysretq compatibility
    //   With 0x10: sysretq would set CS=0x10+16|3=0x23? No: CS=STAR[63:48]+16|3
    //   0x10+16=0x20, 0x20|3=0x23... that's data. Hmm.
    //   For GDT: [3]=UserCode=0x18, [4]=UserData=0x20
    //   sysretq: CS = STAR[63:48]+16 | 3, SS = STAR[63:48]+8 | 3
    //   We want CS=0x1B=0x18|3 → base=0x18 → STAR[63:48]+16=0x18 → STAR[63:48]=0x08
    //   We want SS=0x23=0x20|3 → base=0x20 → STAR[63:48]+8=0x20 → STAR[63:48]=0x18
    //   These are contradictory! UserCode and UserData must be adjacent with
    //   UserData FIRST for sysret to work correctly (SS before CS in GDT).
    //   Since we use iretq (not sysret), STAR[63:48] doesn't matter at runtime.
    //   Set to 0x10 as a reasonable placeholder.
    uint64_t star_val = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(IA32_STAR, star_val);

    // LSTAR: syscall entry point
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);

    // FMASK: clear IF on syscall (disable interrupts during syscall entry)
    wrmsr(IA32_FMASK, 0x200);

    // FIX BUG#2: Initialize kernel_syscall_rsp to a safe kernel stack.
    // This will be updated by each process switch. For now, allocate
    // a static emergency kernel stack for the initial kernel context.
    // A proper implementation updates this on every context switch.
    // We'll set it to the current RSP (already in kernel) as initial value.
    uint64_t current_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(current_rsp));
    // Round up to page boundary for a cleaner stack top
    // For now just use current stack - process switches will update it
    set_kernel_syscall_rsp(current_rsp);

    terminal_writestring("[SYSCALL] Registered 11 system calls (including wait/waitpid)\n");
    terminal_writestring("[SYSCALL] syscall/iretq enabled\n");
}

// =============================================================================
// SYSCALL DISPATCHER
// =============================================================================

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (num >= MAX_SYSCALLS || syscall_table[num] == NULL) {
        terminal_writestring("[SYSCALL] Unknown syscall number\n");
        return (uint64_t)-1;
    }

    return syscall_table[num](arg1, arg2, arg3, arg4, arg5);
}

// =============================================================================
// SYSCALL IMPLEMENTATIONS
// =============================================================================

extern process_t* proc_current(void);
extern process_t* proc_find(int32_t pid);
extern int proc_reap(process_t* zombie);   // NEW: from lifecycle.c

static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    terminal_writestring("[SYSCALL] exit(");
    char buf[32];
    uint64_to_string(code, buf);
    terminal_writestring(buf);
    terminal_writestring(")\n");
    // FIX: call proc_exit() to properly handle zombie state
    proc_exit((int)code);
    // proc_exit never returns
    for(;;) __asm__ volatile("hlt");
    return 0;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    const char* str = (const char*)buf;
    if (fd == 1) {
        for (uint64_t i = 0; i < count; i++) {
            terminal_putchar(str[i]);
        }
        return count;
    }
    return (uint64_t)-1;
}

static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    process_t* p = proc_current();
    return p ? p->pid : 0;
}

static uint64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    process_t* p = proc_current();
    return p ? p->parent_pid : 0;
}

static uint64_t sys_getpriority(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    process_t* p = proc_find((int32_t)pid);
    return p ? p->priority : (uint64_t)-1;
}

static uint64_t sys_setpriority(uint64_t pid, uint64_t priority, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (priority > 255) return (uint64_t)-1;
    process_t* p = proc_find((int32_t)pid);
    if (!p) return (uint64_t)-1;
    p->priority = (uint8_t)priority;
    return 0;
}

static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint64_t)proc_fork();
}

/**
 * sys_waitpid - FIX BUG#7: Wait for a child process to exit
 *
 * @pid:        PID to wait for (-1 = any child)
 * @status_ptr: Pointer to store exit status (can be 0/NULL)
 * @options:    Wait options (0 = blocking, 1 = WNOHANG)
 * @return:     PID of reaped child, 0 if WNOHANG and no zombie, -1 on error
 */
static uint64_t sys_waitpid(uint64_t pid, uint64_t status_ptr, uint64_t options,
                             uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    process_t* curr = proc_current();
    if (!curr) return (uint64_t)-1;

    int wnohang = (options & 1);  // WNOHANG flag

    // Try to find a zombie child
    while (1) {
        process_t* zombie = NULL;

        if ((int64_t)pid == -1) {
            // Wait for any child
            // Search process tree for zombie child of current process
            process_t* child = curr->children_head;
            while (child) {
                if (child->state == PROC_ZOMBIE) {
                    zombie = child;
                    break;
                }
                child = child->sibling_next;
            }
        } else {
            // Wait for specific PID
            process_t* target = proc_find((int32_t)pid);
            if (!target || target->parent_pid != curr->pid) {
                return (uint64_t)-1;  // Not a child
            }
            if (target->state == PROC_ZOMBIE) {
                zombie = target;
            }
        }

        if (zombie) {
            // Found a zombie - get exit code and reap
            uint32_t child_pid = zombie->pid;
            int exit_code = proc_reap(zombie);

            // Store exit status if caller wants it
            if (status_ptr != 0) {
                *((int*)status_ptr) = exit_code;
            }

            char buf[32];
            terminal_writestring("[SYSCALL] waitpid reaped PID ");
            uint64_to_string(child_pid, buf);
            terminal_writestring(buf);
            terminal_writestring(" exit_code=");
            uint64_to_string((uint64_t)exit_code, buf);
            terminal_writestring(buf);
            terminal_writestring("\n");

            return child_pid;
        }

        // No zombie found
        if (wnohang) {
            return 0;  // WNOHANG: return immediately
        }

        // Blocking wait: yield and retry
        // Simple busy-wait via timer sleep
        timer_sleep(10);
    }
}

static uint64_t sys_sleep(uint64_t ms, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    timer_sleep((uint32_t)ms);
    return 0;
}

static uint64_t sys_getticks(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return timer_get_ticks();
}
