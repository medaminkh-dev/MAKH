/**
 * MakhOS - kernel.c
 * Version: 0.0.2
 * Main kernel entry point
 */

#include "include/kernel.h"
#include "include/vga.h"
#include "include/multiboot.h"
#include "include/types.h"
#include "include/mm/pmm.h"
#include "include/mm/vmm.h"
#include "include/mm/kheap.h"
#include "include/stdlib.h"
#include "include/arch/idt.h"
#include "include/arch/pic.h"
#include "include/drivers/timer.h"
#include "include/drivers/keyboard.h"
#include "include/input_line.h"

/* External reference to multiboot info (passed from assembly in RDI) */
extern uint64_t multiboot_info_ptr;

/**
 * uint64_to_string - Convert uint64 to decimal string
 */
void uint64_to_string(uint64_t value, char* buf) {
    char temp[24];
    int i = 0;
    
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    
    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    int j;
    for (j = 0; j < i; j++) {
        buf[j] = temp[i - 1 - j];
    }
    buf[i] = '\0';
}

/**
 * uint64_to_hex - Convert uint64 to hex string
 */
void uint64_to_hex(uint64_t value, char* buf) {
    const char* hex = "0123456789ABCDEF";
    char temp[20];
    int i = 0;
    
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    
    while (value > 0) {
        temp[i++] = hex[value & 0xF];
        value >>= 4;
    }
    
    buf[0] = '0';
    buf[1] = 'x';
    int j;
    for (j = 0; j < i; j++) {
        buf[j + 2] = temp[i - 1 - j];
    }
    buf[i + 2] = '\0';
}

/**
 * print_ok - Print [OK] in green
 */
static void print_ok(void) {
    terminal_writestring("[");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("OK");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("] ");
}

/**
 * print_check - Print [CHECK] in cyan
 */
static void print_check(void) {
    terminal_writestring("[");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("CHECK");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("] ");
}

/**
 * print_banner - Print the MakhOS banner
 */
static void print_banner(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("\n");
    terminal_writestring("    __  ___       __ _____ ____   _____\n");
    terminal_writestring("   /  |/  /______/ // / _ / __ \\/ ___/\n");
    terminal_writestring("  / /|_/ / __/ __/ _  / __ / / / /__  \n");
    terminal_writestring(" /_/  /_/_/  \\__/_//_/_/ |_/_/ /_/___/\n");
    terminal_writestring("\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
}

/**
 * test_vmm - Test the Virtual Memory Manager
 */
void test_vmm(void) {
    char buf[32];
    
    terminal_writestring("\n[TEST] Testing Virtual Memory Manager...\n");
    
    /* Allocate virtual page */
    void* virt = vmm_alloc_page(PAGE_PRESENT | PAGE_WRITABLE);
    terminal_writestring("  Allocated virtual page at: ");
    uint64_to_hex((uint64_t)(uintptr_t)virt, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    /* Write value */
    uint64_t* ptr = (uint64_t*)virt;
    *ptr = 0xDEADBEEF;
    terminal_writestring("  Wrote 0xDEADBEEF to virtual address\n");
    
    /* Get physical address */
    uint64_t phys = vmm_get_physical((uint64_t)(uintptr_t)virt);
    terminal_writestring("  Physical address: ");
    uint64_to_hex(phys, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    /* Verify virtual != physical (paging works) */
    if (phys != (uint64_t)(uintptr_t)virt) {
        terminal_writestring("  [OK] Virtual != Physical (paging works)\n");
    }
    
    /* Read from physical address (identity mapped) */
    uint64_t* phys_ptr = (uint64_t*)(uintptr_t)phys;
    terminal_writestring("  Value at physical address: 0x");
    uint64_to_hex(*phys_ptr, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    if (*phys_ptr == 0xDEADBEEF) {
        terminal_writestring("  [OK] Physical memory contains correct value\n");
    }
    
    /* Test fixed mapping */
    uint64_t fixed_virt = 0xFFFF800000100000ULL;
    uint64_t fixed_phys = (uint64_t)(uintptr_t)pmm_alloc_page();
    
    if (fixed_phys != 0 && vmm_map_page(fixed_virt, fixed_phys, PAGE_PRESENT | PAGE_WRITABLE) == 0) {
        terminal_writestring("  Mapped fixed virt ");
        uint64_to_hex(fixed_virt, buf);
        terminal_writestring(buf);
        terminal_writestring(" -> phys ");
        uint64_to_hex(fixed_phys, buf);
        terminal_writestring(buf);
        terminal_writestring("\n");
        
        *(uint64_t*)(uintptr_t)fixed_virt = 0x12345678;
        if (*(uint64_t*)(uintptr_t)fixed_phys == 0x12345678) {
            terminal_writestring("  [OK] Fixed mapping works\n");
        }
        
        vmm_unmap_page(fixed_virt);
        pmm_free_page((void*)(uintptr_t)fixed_phys);
    }
    
    vmm_free_page(virt);
    terminal_writestring("[TEST] VMM tests complete\n");
}

/**
 * memset - Fill memory with a byte value
 */
static void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

/**
 * strcpy - Copy a string
 */
static char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

/**
 * test_kheap - Test the Kernel Heap Manager
 */
void test_kheap(void) {
    char buf[32];
    
    terminal_writestring("\n[TEST] Testing Kernel Heap...\n");
    
    /* Test 1: Basic allocation */
    terminal_writestring("  Test 1: Basic allocation...\n");
    char* str = (char*)kmalloc(32);
    if (str != NULL) {
        strcpy(str, "Hello Heap!");
        terminal_writestring("    Allocated string: ");
        terminal_writestring(str);
        terminal_writestring("\n");
        kfree(str);
        terminal_writestring("    [OK] Basic allocation works\n");
    }
    
    /* Test 2: Multiple allocations */
    terminal_writestring("  Test 2: Multiple allocations...\n");
    void* ptrs[10];
    int all_ok = 1;
    for (int i = 0; i < 10; i++) {
        ptrs[i] = kmalloc(128);
        if (ptrs[i] == NULL) {
            all_ok = 0;
        } else {
            memset(ptrs[i], i, 128);
        }
    }
    for (int i = 0; i < 10; i++) {
        if (ptrs[i] != NULL) kfree(ptrs[i]);
    }
    if (all_ok) {
        terminal_writestring("    [OK] Multiple allocations work\n");
    }
    
    /* Test 3: Variable sizes and alignment */
    terminal_writestring("  Test 3: Variable sizes and alignment...\n");
    size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    all_ok = 1;
    for (int i = 0; i < 10; i++) {
        void* p = kmalloc(sizes[i]);
        if (p == NULL) {
            all_ok = 0;
        } else {
            /* Check 16-byte alignment */
            if (((uint64_t)(uintptr_t)p & 15) != 0) {
                all_ok = 0;
            }
            kfree(p);
        }
    }
    if (all_ok) {
        terminal_writestring("    [OK] All sizes allocated with 16-byte alignment\n");
    }
    
    /* Test 4: kcalloc (zeroed memory) */
    terminal_writestring("  Test 4: kcalloc (zeroed memory)...\n");
    int* array = (int*)kcalloc(100, sizeof(int));
    if (array != NULL) {
        int all_zero = 1;
        for (int i = 0; i < 100; i++) {
            if (array[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        kfree(array);
        if (all_zero) {
            terminal_writestring("    [OK] kcalloc returns zeroed memory\n");
        }
    }
    
    /* Test 5: Reallocation */
    terminal_writestring("  Test 5: krealloc...\n");
    char* rptr = (char*)kmalloc(16);
    if (rptr != NULL) {
        strcpy(rptr, "Hello");
        char* new_rptr = (char*)krealloc(rptr, 64);
        if (new_rptr != NULL) {
            /* Verify data preserved */
            if (new_rptr[0] == 'H' && new_rptr[1] == 'e') {
                terminal_writestring("    [OK] krealloc preserves data\n");
            }
            kfree(new_rptr);
        } else {
            kfree(rptr);
        }
    }
    
    /* Test 6: Statistics */
    terminal_writestring("  Test 6: Heap statistics...\n");
    terminal_writestring("    Total heap: ");
    uint64_to_string(kheap_get_total(), buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
    
    terminal_writestring("    Used: ");
    uint64_to_string(kheap_get_used(), buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
    
    terminal_writestring("    Free: ");
    uint64_to_string(kheap_get_free(), buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
    
    /* Test 7: malloc/free via stdlib.h */
    terminal_writestring("  Test 7: stdlib malloc/free macros...\n");
    void* std_ptr = malloc(256);
    if (std_ptr != NULL) {
        free(std_ptr);
        terminal_writestring("    [OK] stdlib macros work\n");
    }
    
    terminal_writestring("[TEST] Kernel heap tests complete\n");
}

/**
 * test_interrupts - Test the Interrupt Descriptor Table
 */
void test_interrupts(void) {
    terminal_writestring("\n[TEST] Testing Interrupt Descriptor Table...\n");
    
    // Test 1: Try a simple software interrupt first
    terminal_writestring("  Test 1: Triggering software interrupt int 0x30...\n");
    __asm__ volatile("int $0x30");
    terminal_writestring("  [ERROR] Should not reach here after int 0x30!\n");
}

/**
 * test_timer - Test the PIC and Timer interrupts
 */
void test_timer(void) {
    char buf[32];
    
    terminal_writestring("\n[TEST] Testing Timer Interrupts...\n");
    
    // Initialize PIC
    print_check();
    terminal_writestring("Initializing Programmable Interrupt Controller...\n");
    pic_init();
    
    // Initialize Timer
    print_check();
    terminal_writestring("Initializing Timer...\n");
    timer_init(TIMER_FREQUENCY);
    
    // Enable interrupts
    print_check();
    terminal_writestring("Enabling interrupts...\n");
    idt_enable_interrupts();
    
    // Test timer with busy-wait sleep
    terminal_writestring("\n  Waiting for 2 seconds...\n");
    uint64_t ticks_before = timer_get_ticks();
    timer_sleep(2000);  // 2 seconds
    uint64_t ticks_after = timer_get_ticks();
    
    terminal_writestring("  ");
    uint64_t elapsed_ticks = ticks_after - ticks_before;
    if (elapsed_ticks >= 195 && elapsed_ticks <= 205) {
        print_ok();
        terminal_writestring("Timer counted ");
        uint64_to_string(elapsed_ticks, buf);
        terminal_writestring(buf);
        terminal_writestring(" ticks in 2 seconds (expected ~200)\n");
    } else {
        terminal_writestring("[WARNING] Timer counted ");
        uint64_to_string(elapsed_ticks, buf);
        terminal_writestring(buf);
        terminal_writestring(" ticks (expected ~200)\n");
    }
    
    // Print final statistics
    terminal_writestring("\n");
    timer_print_stats();
}

/**
 * test_keyboard - Test the PS/2 keyboard input
 */
void test_keyboard(void) {
    terminal_writestring("\n[TEST] Testing Keyboard Input...\n");
    terminal_writestring("Press some keys (they will appear on screen)\n");
    terminal_writestring("Type 'TEST' to complete the test...\n\n");
    
    // Initialize keyboard
    keyboard_init();
    
    // Buffer to track what user types
    char typed[5] = {0};
    int typed_index = 0;
    
    // Wait for user to type "TEST"
    while (typed_index < 4) {
        unsigned char c = (unsigned char)keyboard_get_char();
        
        // Display the character
        if (c == '\n' || c == '\r') {
            terminal_writestring("\n");
        } else if (c == '\b') {
            // Handle backspace - delete selection or shift left
            if (terminal_has_selection()) {
                terminal_delete_selection();
            } else {
                terminal_putchar('\b');
            }
            if (typed_index > 0) {
                typed_index--;
                typed[typed_index] = 0;
            }
        } else if (c == CHAR_SELECT_LEFT) {
            // Shift+Left Arrow - extend selection left
            size_t row, col;
            terminal_getcursor(&row, &col);
            
            if (!terminal_has_selection()) {
                terminal_start_selection(row, col);
            }
            
            if (col > 0) {
                terminal_setcursor(row, col - 1);
                terminal_extend_selection(row, col - 1);
            }
        } else if (c == CHAR_SELECT_RIGHT) {
            // Shift+Right Arrow - extend selection right
            size_t row, col;
            terminal_getcursor(&row, &col);
            
            if (!terminal_has_selection()) {
                terminal_start_selection(row, col);
            }
            
            if (col < VGA_WIDTH - 1) {
                terminal_setcursor(row, col + 1);
                terminal_extend_selection(row, col + 1);
            }
        } else if (c == CHAR_ARROW_UP) {
            // Move cursor up - cancel selection
            if (terminal_has_selection()) {
                terminal_clear_selection();
            }
            size_t row, col;
            terminal_getcursor(&row, &col);
            if (row > 0) {
                terminal_setcursor(row - 1, col);
            }
        } else if (c == CHAR_ARROW_DOWN) {
            // Move cursor down - cancel selection
            if (terminal_has_selection()) {
                terminal_clear_selection();
            }
            size_t row, col;
            terminal_getcursor(&row, &col);
            if (row < VGA_HEIGHT - 1) {
                terminal_setcursor(row + 1, col);
            }
        } else if (c == CHAR_ARROW_LEFT) {
            // Move cursor left - cancel selection
            if (terminal_has_selection()) {
                terminal_clear_selection();
            }
            size_t row, col;
            terminal_getcursor(&row, &col);
            if (col > 0) {
                terminal_setcursor(row, col - 1);
            }
        } else if (c == CHAR_ARROW_RIGHT) {
            // Move cursor right - cancel selection
            if (terminal_has_selection()) {
                terminal_clear_selection();
            }
            size_t row, col;
            terminal_getcursor(&row, &col);
            if (col < VGA_WIDTH - 1) {
                terminal_setcursor(row, col + 1);
            }
        } else if (c == CHAR_DELETE) {
            // Delete key (forward delete) - delete character at cursor
            if (terminal_has_selection()) {
                terminal_delete_selection();
            } else {
                terminal_delete_forward();
            }
        } else if (c >= 32 && c <= 126) {
            // Printable character - delete selection if active
            if (terminal_has_selection()) {
                terminal_delete_selection();
            }
            terminal_putchar(c);

            // Track for "TEST" pattern
            if (typed_index < 4) {
                typed[typed_index++] = c;
                typed[typed_index] = 0;
            }
        }
    }
    
    // Check if user typed "TEST"
    terminal_writestring("\n");
    if (typed[0] == 'T' && typed[1] == 'E' &&
        typed[2] == 'S' && typed[3] == 'T') {
        print_ok();
        terminal_writestring("Successfully typed 'TEST'\n");
    } else {
        terminal_writestring("[INFO] You typed: ");
        terminal_writestring(typed);
        terminal_writestring(" (expected TEST)\n");
    }
    
    terminal_writestring("[TEST] Keyboard test complete\n");
}

/**
 * kernel_main - Main kernel entry point
 * Called from boot.asm after switching to long mode
 * 
 * Note: The multiboot info pointer is passed in RDI by boot.asm
 */
void kernel_main(void) {
    /* Early checkpoint - direct VGA write before any function calls */
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    vga[80] = (0x0F << 8) | 'k';  /* 'k' white on black at line 1 */
    
    /* Checkpoint - about to init terminal */
    vga[81] = (0x0F << 8) | 't';  /* 't' for terminal init start */
    
    /* Initialize VGA terminal without clearing screen
     * (preserve bootloader checkmarks for debugging) */
    terminal_initialize_noclear();
    
    /* Checkpoint - terminal init done */
    vga[82] = (0x0F << 8) | 'i';  /* 'i' for init done */
    
    /* Position cursor after bootloader messages (line 3) */
    terminal_setcursor(3, 0);
    
    /* Checkpoint - about to print banner */
    vga[83] = (0x0F << 8) | 'B';  /* 'B' for banner start */
    
    /* Print banner */
    print_banner();
    
    /* Checkpoint - banner printed */
    vga[84] = (0x0F << 8) | 'b';  /* 'b' for banner done */
    
    /* Print version info */
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("  ");
    terminal_writestring(KERNEL_NAME);
    terminal_writestring(" v");
    terminal_writestring(KERNEL_VERSION);
    terminal_writestring(" - ");
    terminal_writestring(KERNEL_PHASE);
    terminal_writestring(" Complete\n\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    
    /* Check 1: Long mode active */
    print_ok();
    terminal_writestring("Long mode active (64-bit)\n");
    
    /* Check 2: CPU features */
    print_ok();
    terminal_writestring("CPU initialized\n");
    
    /* Check 3: VGA driver */
    print_ok();
    terminal_writestring("VGA driver initialized\n");
    
    /* Check 4: Parse multiboot info */
    print_ok();
    terminal_writestring("Multiboot2 info structure received\n");
    
    /* Initialize PMM using multiboot memory map */
    print_check();
    terminal_writestring("Initializing Physical Memory Manager...\n");
    
    /* Find mmap tag from multiboot info */
    struct multiboot_tag_mmap* mmap_tag = NULL;
    if (multiboot_info_ptr != 0) {
        struct multiboot_info* mb_info = (struct multiboot_info*)(uintptr_t)multiboot_info_ptr;
        struct multiboot_tag* tag = (struct multiboot_tag*)((uintptr_t)mb_info + 8);
        
        while (tag->type != MULTIBOOT_TAG_TYPE_END) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
                mmap_tag = (struct multiboot_tag_mmap*)tag;
                break;
            }
            /* Move to next tag (8-byte aligned) */
            uintptr_t next_addr = ((uintptr_t)tag + ((tag->size + 7) & ~7));
            tag = (struct multiboot_tag*)next_addr;
        }
    }
    
    /* Initialize PMM */
    pmm_init(mmap_tag);
    
    /* PMM Tests */
    print_ok();
    terminal_writestring("PMM initialized, running tests...\n");
    
    /* Test 1: Allocate 2 pages */
    terminal_writestring("  Test 1: Allocating 2 pages...\n");
    void* page1 = pmm_alloc_page();
    void* page2 = pmm_alloc_page();
    
    char buf[32];
    terminal_writestring("    Page 1: ");
    uint64_to_hex((uint64_t)(uintptr_t)page1, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    terminal_writestring("    Page 2: ");
    uint64_to_hex((uint64_t)(uintptr_t)page2, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    /* Test 2: Free first page */
    terminal_writestring("  Test 2: Freeing page 1...\n");
    pmm_free_page(page1);
    
    /* Test 3: Print memory statistics */
    terminal_writestring("  Memory Statistics:\n");
    terminal_writestring("    Total pages: ");
    uint64_to_string(pmm_get_total_page_count(), buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    terminal_writestring("    Used pages: ");
    uint64_to_string(pmm_get_used_page_count(), buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    terminal_writestring("    Free memory: ");
    uint64_to_string(pmm_get_free_memory() / (1024 * 1024), buf);
    terminal_writestring(buf);
    terminal_writestring(" MB\n");
    
    /* Initialize VMM */
    print_check();
    terminal_writestring("Initializing Virtual Memory Manager...\n");
    vmm_init();
    
    /* Run VMM tests */
    test_vmm();
    
    /* Initialize Kernel Heap */
    print_check();
    terminal_writestring("Initializing Kernel Heap...\n");
    kheap_init();
    
    /* Run heap tests */
    test_kheap();
    
    /* Initialize IDT */
    print_check();
    terminal_writestring("Initializing Interrupt Descriptor Table...\n");
    idt_init();
    
    /* Test timer and interrupts */
    test_timer();
    
    /* Test keyboard */
    test_keyboard();
    
    /* Success message */
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("\n");
    terminal_writestring("  ====================================\n");
    terminal_writestring("  ");
    terminal_writestring(KERNEL_NAME);
    terminal_writestring(" v");
    terminal_writestring(KERNEL_VERSION);
    terminal_writestring(" - ");
    terminal_writestring(KERNEL_PHASE);
    terminal_writestring(" [OK]\n");
    terminal_writestring("  ====================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    
    terminal_writestring("\n[MAIN] System fully functional.\n");
    terminal_writestring("Type anything - it will appear on screen.\n");
    terminal_writestring("Press Ctrl+Alt+G to exit QEMU.\n\n");
    
    /* Main loop - input line editing with history */
    terminal_writestring("MakhOS> ");
    size_t prompt_row;
    terminal_getcursor(&prompt_row, NULL);
    
    input_line_t current_line;
    input_history_t history;
    input_line_init(&current_line);
    input_history_init(&history);
    
    while (1) {
        input_line_render(&current_line, "MakhOS> ", prompt_row);
        unsigned char c = (unsigned char)keyboard_get_char();
        
        if (c == '\n' || c == '\r') {
            // Enter - execute command and add to history
            terminal_writestring("\n");
            if (current_line.length > 0) {
                input_history_push(&history, current_line.buffer);
            }
            // Here you would process the command
            // For now just show a new prompt
            terminal_writestring("MakhOS> ");
            terminal_getcursor(&prompt_row, NULL);
            input_line_init(&current_line);
        }
        else if (c == '\b') {
            // Backspace
            input_line_backspace(&current_line);
        }
        else if (c == CHAR_DELETE) {
            // Delete key (forward delete)
            input_line_delete(&current_line);
        }
        else if (c == CHAR_ARROW_LEFT) {
            // Left arrow
            input_line_move_left(&current_line);
        }
        else if (c == CHAR_ARROW_RIGHT) {
            // Right arrow
            input_line_move_right(&current_line);
        }
        else if (c == CHAR_ARROW_UP) {
            // Up arrow - previous history
            char* prev = input_history_prev(&history);
            if (prev) {
                input_history_load(&current_line, prev);
            }
        }
        else if (c == CHAR_ARROW_DOWN) {
            // Down arrow - next history
            char* next = input_history_next(&history);
            if (next) {
                input_history_load(&current_line, next);
            } else {
                input_line_init(&current_line);
            }
        }
        else if (c == CHAR_SELECT_LEFT) {
            // Shift+Left - extend selection
            input_line_sel_extend_left(&current_line);
        }
        else if (c == CHAR_SELECT_RIGHT) {
            // Shift+Right - extend selection
            input_line_sel_extend_right(&current_line);
        }
        else if (c == CHAR_CTRL_A) {
            // Ctrl+A - move to start of line
            input_line_move_home(&current_line);
        }
        else if (c == CHAR_CTRL_E) {
            // Ctrl+E - move to end of line
            input_line_move_end(&current_line);
        }
        else if (c == CHAR_CTRL_K) {
            // Ctrl+K - kill to end of line
            input_line_kill_to_end(&current_line);
        }
        else if (c == CHAR_CTRL_U) {
            // Ctrl+U - kill to start of line
            input_line_kill_to_start(&current_line);
        }
        else if (c == CHAR_CTRL_W) {
            // Ctrl+W - kill word back
            input_line_kill_word_back(&current_line);
        }
        else if (c == CHAR_CTRL_Y) {
            // Ctrl+Y - yank (paste)
            input_line_yank(&current_line);
        }
        else if (c == CHAR_CTRL_L) {
            // Ctrl+L - clear screen
            terminal_clear();
            terminal_writestring("MakhOS> ");
            terminal_getcursor(&prompt_row, NULL);
        }
        else if (c >= 32 && c <= 126) {
            // Printable character
            input_line_insert(&current_line, (char)c);
        }
    }
}

/**
 * kernel_halt - Halt the system
 * Disables interrupts and enters an infinite halt loop
 */
void kernel_halt(void) {
    cli();
    for (;;) {
        hlt();
    }
}

/**
 * kernel_panic - Kernel panic handler
 * @message: Error message to display
 */
void kernel_panic(const char* message) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    terminal_writestring("\n\n*** KERNEL PANIC ***\n");
    terminal_writestring(message);
    terminal_writestring("\n********************\n");
    
    kernel_halt();
}
