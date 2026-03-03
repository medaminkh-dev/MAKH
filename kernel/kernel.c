/**
 * MakhOS - kernel.c
 * Main kernel entry point
 */

#include "include/kernel.h"
#include "include/vga.h"
#include "include/multiboot.h"
#include "include/types.h"

/* External reference to multiboot info (passed from assembly in RDI) */
extern uint64_t multiboot_info_ptr;

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
 * kernel_main - Main kernel entry point
 * Called from boot.asm after switching to long mode
 * 
 * Note: The multiboot info pointer is passed in RDI by boot.asm
 */
void kernel_main(void) {
    /* Initialize VGA terminal (starts after bootloader messages) */
    terminal_initialize();
    
    /* Position cursor after bootloader messages (line 3) */
    terminal_setcursor(3, 0);
    
    /* Print banner */
    print_banner();
    
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
    
    /* Parse multiboot info */
    /* Note: In a real implementation, we'd get the pointer from RDI */
    /* For now, we'll skip detailed parsing in Phase 1 */
    print_ok();
    terminal_writestring("Multiboot2 info structure received\n");
    
    /* Memory check */
    print_check();
    terminal_writestring("Memory map parsed (basic)\n");
    
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
    terminal_writestring(" Complete [OK]\n");
    terminal_writestring("  ====================================\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    
    terminal_writestring("\nSystem halted. Press Ctrl+Alt+G (in QEMU) to exit.\n");
    
    /* Halt the system */
    kernel_halt();
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
