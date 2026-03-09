#include <syscall/syscall.h>
#include <kernel.h>
#include <vga.h>
#include <drivers/timer.h>
#include <proc/fork.h>

/**
 * =============================================================================
 * kernel/syscall/core/syscall.c - System Call Dispatch and Setup
 * =============================================================================
 * Registers syscalls and provides main dispatch handler
 * =============================================================================
 */

// =============================================================================
// SYSCALL TABLE AND HANDLERS
// =============================================================================

static syscall_fn_t syscall_table[MAX_SYSCALLS];

// MSR addresses
#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084

extern void syscall_entry(void);

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
    char buf[32];
    
    // Initialize table
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = NULL;
    }
    
    // Register syscalls
    syscall_table[SYS_EXIT] = sys_exit;
    syscall_table[SYS_WRITE] = sys_write;
    syscall_table[SYS_GETPID] = sys_getpid;
    syscall_table[SYS_SLEEP] = sys_sleep;
    syscall_table[SYS_GETTICKS] = sys_getticks;
    syscall_table[SYS_GETPPID] = sys_getppid;
    syscall_table[SYS_GETPRIORITY] = sys_getpriority;
    syscall_table[SYS_SETPRIORITY] = sys_setpriority;
    syscall_table[SYS_FORK] = sys_fork;
    
    // Enable syscall in EFER
    uint64_t efer = rdmsr(IA32_EFER);
    efer |= 0x01;  // SCE bit
    wrmsr(IA32_EFER, efer);
    
    // Set up STAR MSR
    uint64_t star_val = ((uint64_t)0x08 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(IA32_STAR, star_val);
    
    // Set LSTAR (syscall entry point)
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    
    // Set FMASK (clear IF on syscall)
    wrmsr(IA32_FMASK, 0x200);
    
    terminal_writestring("[SYSCALL] Registered 9 system calls\n");
    terminal_writestring("[SYSCALL] syscall/sysret enabled\n");
}

// =============================================================================
// SYSCALL DISPATCHER
// =============================================================================

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (num >= MAX_SYSCALLS || syscall_table[num] == NULL) {
        terminal_writestring("[SYSCALL] Unknown syscall number\n");
        return -1;
    }
    
    return syscall_table[num](arg1, arg2, arg3, arg4, arg5);
}

// =============================================================================
// SYSCALL IMPLEMENTATIONS (Process Category)
// =============================================================================

extern process_t* proc_current(void);
extern process_t* proc_find(int32_t pid);

static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    terminal_writestring("[SYSCALL] exit(");
    char buf[32];
    uint64_to_string(code, buf);
    terminal_writestring(buf);
    terminal_writestring(")\n");
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
    return -1;
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
    process_t* p = proc_find(pid);
    return p ? p->priority : (uint64_t)-1;
}

static uint64_t sys_setpriority(uint64_t pid, uint64_t priority, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (priority > 255) return (uint64_t)-1;
    process_t* p = proc_find(pid);
    if (!p) return (uint64_t)-1;
    p->priority = priority;
    return 0;
}

static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint64_t)proc_fork();
}

// =============================================================================
// SYSCALL IMPLEMENTATIONS (Misc Category)
// =============================================================================

static uint64_t sys_sleep(uint64_t ms, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    timer_sleep(ms);
    return 0;
}

static uint64_t sys_getticks(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return timer_get_ticks();
}
