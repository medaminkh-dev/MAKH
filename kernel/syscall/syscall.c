#include <syscall.h>
#include <kernel.h>
#include <vga.h>
#include <drivers/timer.h>
#include <proc.h>

// Syscall function table
static syscall_fn_t syscall_table[MAX_SYSCALLS];

// MSR addresses for syscall/sysret setup
#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084

// External assembly entry point
extern void syscall_entry(void);

// Forward declarations of syscall implementations
static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a4, uint64_t a5);
static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_sleep(uint64_t ms, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_getticks(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_getpriority(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
static uint64_t sys_setpriority(uint64_t pid, uint64_t priority, uint64_t a3, uint64_t a4, uint64_t a5);

// MSR read/write helpers
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

void syscall_init(void) {
    terminal_writestring("[SYSCALL] Initializing system call interface...\n");
    char buf[32];  // Buffer for number conversion
    
    // Initialize all entries to NULL
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = NULL;
    }
    
    // Register syscalls
    syscall_table[SYS_EXIT] = sys_exit;
    syscall_table[SYS_WRITE] = sys_write;
    syscall_table[SYS_GETPID] = sys_getpid;
    syscall_table[SYS_SLEEP] = sys_sleep;
    syscall_table[SYS_GETTICKS] = sys_getticks;
    
    // Register new process statistics syscalls
    syscall_table[SYS_GETPPID] = sys_getppid;
    syscall_table[SYS_GETPRIORITY] = sys_getpriority;
    syscall_table[SYS_SETPRIORITY] = sys_setpriority;
    
    // Enable syscall instruction in EFER MSR
    uint64_t efer = rdmsr(IA32_EFER);
    terminal_writestring("[SYSCALL] EFER before: 0x");
    uint64_to_hex(efer, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    efer |= 0x01;  // Set SCE (Syscall Enable) bit
    wrmsr(IA32_EFER, efer);
    terminal_writestring("[SYSCALL] EFER after: 0x");
    uint64_to_hex(rdmsr(IA32_EFER), buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // Set up STAR MSR:
    // For sysret to return to user mode with CS=0x1B:
    //   sysret CS = STAR[63:48] + 16 | 3
    //   So STAR[63:48] = 0x08 (so 0x08+16 = 0x18, then |3 = 0x1B)
    // For syscall to enter kernel mode with CS=0x08:
    //   STAR[47:32] = 0x08
    uint64_t star_val = ((uint64_t)0x08 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(IA32_STAR, star_val);
    terminal_writestring("[SYSCALL] STAR set to: 0x");
    uint64_to_hex(star_val, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    terminal_writestring("[SYSCALL] STAR read back: 0x");
    uint64_to_hex(rdmsr(IA32_STAR), buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // Set LSTAR MSR to syscall_entry address (RIP loaded on syscall)
    terminal_writestring("[SYSCALL] syscall_entry at: 0x");
    uint64_to_hex((uint64_t)syscall_entry, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    terminal_writestring("[SYSCALL] LSTAR read back: 0x");
    uint64_to_hex(rdmsr(IA32_LSTAR), buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // Set SFMASK MSR (RFLAGS mask) - bits to clear on syscall
    // Clear IF bit (0x200) to disable interrupts on syscall entry
    wrmsr(IA32_FMASK, 0x200);
    terminal_writestring("[SYSCALL] FMASK set to: 0x200\n");
    
    terminal_writestring("[SYSCALL] Registered 5 system calls\n");
    terminal_writestring("  [0] exit\n");
    terminal_writestring("  [1] write\n");
    terminal_writestring("  [5] getpid\n");
    terminal_writestring("  [6] sleep\n");
    terminal_writestring("  [7] getticks\n");
    terminal_writestring("[SYSCALL] syscall/sysret enabled via MSRs\n");
}

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, 
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    // Check if syscall number is valid
    if (num >= MAX_SYSCALLS || syscall_table[num] == NULL) {
        terminal_writestring("[SYSCALL] Unknown syscall number\n");
        return -1;
    }
    
    // Call the syscall implementation
    return syscall_table[num](arg1, arg2, arg3, arg4, arg5);
}

// Syscall implementations
static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;  // Suppress unused warnings
    terminal_writestring("[SYSCALL] exit(");
    char buf[32];
    uint64_to_string(code, buf);
    terminal_writestring(buf);
    terminal_writestring(") called - user program completed!\n");
    terminal_writestring("\n=== USER MODE TEST SUCCESSFUL ===\n\n");
    terminal_writestring("System halted.\n");
    for(;;) {
        __asm__ volatile("hlt");
    }
    return 0;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;  // Suppress unused warnings
    const char* str = (const char*)buf;
    
    // Only handle stdout (fd=1) for now
    if (fd == 1) {
        for (uint64_t i = 0; i < count; i++) {
            terminal_putchar(str[i]);
        }
        return count;
    }
    
    return -1;
}

static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;  // Suppress unused warnings
    // For now, kernel process gets PID 1
    return 1;
}

static uint64_t sys_sleep(uint64_t ms, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;  // Suppress unused warnings
    timer_sleep(ms);
    return 0;
}

static uint64_t sys_getticks(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;  // Suppress unused warnings
    return timer_get_ticks();
}

// Get parent PID syscall
static uint64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    process_t* current = proc_current();
    if (!current) return 0;
    return current->parent_pid;
}

// Get process priority syscall
static uint64_t sys_getpriority(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    process_t* proc = proc_find(pid);
    if (!proc) return (uint64_t)-1;
    return proc->priority;
}

// Set process priority syscall
static uint64_t sys_setpriority(uint64_t pid, uint64_t priority, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (priority > 255) return (uint64_t)-1;
    process_t* proc = proc_find(pid);
    if (!proc) return (uint64_t)-1;
    proc->priority = priority;
    return 0;
}
