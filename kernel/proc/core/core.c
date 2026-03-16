#include <proc_internal.h>
#include <mm/kheap.h>
#include <kernel.h>
#include <vga.h>
#include <lib/string.h>
#include <drivers/timer.h>

/**
 * =============================================================================
 * core.c - Core Process Management
 * =============================================================================
 * Contains the core process variables and initialization.
 * Phase 11: Updated to use new proc_create with name parameter.
 * =============================================================================
 */

// -----------------------------------------------------------------------------
// GLOBAL VARIABLES (defined here, declared extern in proc_internal.h)
// -----------------------------------------------------------------------------

volatile int in_interrupt_context = 0;
process_t *current_process = NULL;
process_list_t ready_queue;
process_list_t all_processes;

// -----------------------------------------------------------------------------
// STACKS (defined as arrays, declared extern in proc_internal.h)
// -----------------------------------------------------------------------------

uint8_t idle_stack[IDLE_STACK_SIZE] __attribute__((aligned(16)));
uint8_t init_stack[INIT_STACK_SIZE] __attribute__((aligned(16)));

static process_t idle_pcb;
static process_t init_pcb;

// -----------------------------------------------------------------------------
// FORWARD DECLARATIONS
// -----------------------------------------------------------------------------

static void proc_idle_fn(void);

// -----------------------------------------------------------------------------
// HELPER FUNCTIONS
// -----------------------------------------------------------------------------

void phex(uint64_t v) {
    char buf[32];
    uint64_to_hex(v, buf);
    const char *p = (buf[0] == '0' && buf[1] == 'x') ? buf + 2 : buf;
    terminal_writestring(p);
}

// -----------------------------------------------------------------------------
// PROCESS INITIALIZATION
// -----------------------------------------------------------------------------

void proc_init(void) {
    terminal_writestring("[PROC] Initializing process manager...\n");

    // Initialize subsystems
    pid_init();
    proc_table_init();
    
    // Zero lists
    memset(&ready_queue, 0, sizeof(process_list_t));
    memset(&all_processes, 0, sizeof(process_list_t));

    // Clear stacks
    memset(idle_stack, 0, IDLE_STACK_SIZE);
    memset(init_stack, 0, INIT_STACK_SIZE);

    // ----------------------------------------------------------------------
    // Create idle process (PID 0) - special case, not via proc_create
    // ----------------------------------------------------------------------
    memset(&idle_pcb, 0, sizeof(process_t));

    idle_pcb.pid                = 0;
    idle_pcb.state              = PROC_READY;
    idle_pcb.priority           = 0;  // Lowest priority
    idle_pcb.kernel_stack       = (uint64_t)idle_stack;
    idle_pcb.kernel_stack_size  = IDLE_STACK_SIZE;
    
    const char* idle_name = "idle";
    for (int i = 0; i < 4; i++) idle_pcb.name[i] = idle_name[i];
    idle_pcb.name[4] = '\0';

    // Set up stack for context switch
    uint64_t *idle_top = (uint64_t *)(idle_stack + IDLE_STACK_SIZE);
    idle_top = (uint64_t *)((uint64_t)idle_top & ~(uint64_t)0xF);
    *(--idle_top) = (uint64_t)0;
    
    idle_pcb.context.rsp    = (uint64_t)idle_top;
    idle_pcb.context.rip    = (uint64_t)proc_idle_fn;
    idle_pcb.context.rflags = 0x202;
    
    __asm__ volatile("mov %%cr3, %0" : "=r"(idle_pcb.context.cr3));
    
    idle_pcb.context.cs = 0x08;
    idle_pcb.context.ds = 0x10;
    idle_pcb.context.es = 0x10;
    idle_pcb.context.fs = 0x10;
    idle_pcb.context.gs = 0x10;
    idle_pcb.context.ss = 0x10;

    idle_pcb.creation_time = timer_get_ticks();
    idle_pcb.cpu_time_used = 0;
    idle_pcb.exit_time = 0;

    all_list_add(&idle_pcb);
    terminal_writestring("[PROC] Idle process created (PID 0), stack=0x");
    phex(idle_pcb.kernel_stack);
    terminal_writestring("\n");

    // ----------------------------------------------------------------------
    // Create init process (PID 1) - special case, not via proc_create
    // ----------------------------------------------------------------------
    memset(&init_pcb, 0, sizeof(process_t));

    init_pcb.pid                = 1;
    init_pcb.state              = PROC_READY;
    init_pcb.priority           = 128;  // Default priority
    init_pcb.kernel_stack       = (uint64_t)init_stack;
    init_pcb.kernel_stack_size  = INIT_STACK_SIZE;
    
    const char* init_name = "init";
    for (int i = 0; i < 4; i++) init_pcb.name[i] = init_name[i];
    init_pcb.name[4] = '\0';

    uint64_t *init_top = (uint64_t *)(init_stack + INIT_STACK_SIZE);
    init_top = (uint64_t *)((uint64_t)init_top & ~(uint64_t)0xF);
    *(--init_top) = (uint64_t)0;
    
    init_pcb.context.rsp    = (uint64_t)init_top;
    init_pcb.context.rip    = (uint64_t)proc_idle_fn;
    init_pcb.context.rflags = 0x202;
    
    __asm__ volatile("mov %%cr3, %0" : "=r"(init_pcb.context.cr3));
    
    init_pcb.context.cs = 0x08;
    init_pcb.context.ds = 0x10;
    init_pcb.context.es = 0x10;
    init_pcb.context.fs = 0x10;
    init_pcb.context.gs = 0x10;
    init_pcb.context.ss = 0x10;

    init_pcb.creation_time = timer_get_ticks();
    init_pcb.cpu_time_used = 0;      
    init_pcb.exit_time = 0;           
    init_pcb.parent_pid = 0;  // Init's parent is the kernel (conceptually)

    all_list_add(&init_pcb);
    terminal_writestring("[PROC] Init process created (PID 1), stack=0x");
    phex(init_pcb.kernel_stack);
    terminal_writestring("\n");

    // Add idle to ready queue
    ready_enqueue(&idle_pcb);

    current_process = NULL;

    terminal_writestring("[PROC] Ready queue: 0x");
    phex(ready_queue.count);
    terminal_writestring(" processes\n");
}

// -----------------------------------------------------------------------------
// ACCESSORS
// -----------------------------------------------------------------------------

process_t *proc_current(void) {
    return current_process;
}

uint32_t proc_get_pid(void) {
    return current_process ? current_process->pid : 0;
}

void proc_become_current(void) {
    // Makes kernel_main the current process (init / PID 1)
    // After this call:
    //   - proc_yield() will save kernel_main context to init_pcb
    //   - It will re-add init to ready_queue whenever it yields the CPU
    //   - It will return when its turn comes and proc_yield() returns normally
    init_pcb.state  = PROC_RUNNING;
    current_process = &init_pcb;
    terminal_writestring("[PROC] kernel_main registered as init (PID 1)\n");
}
// -----------------------------------------------------------------------------
// IDLE PROCESS BODY
// -----------------------------------------------------------------------------

static void proc_idle_fn(void) {
    terminal_writestring("[PROC] Idle loop started (PID 0)\n");
    for (;;) {
        __asm__ volatile("hlt");
        proc_yield();
    }
}
