/**
 * MakhOS - linker.ld
 * Linker script for x86_64 kernel
 * Kernel loads at 0x100000 (1MB mark)
 */

ENTRY(start_32bit)

SECTIONS
{
    /* Kernel load address at 1MB mark */
    . = 0x100000;
    
    /* Multiboot header must be in first 8KB and 8-byte aligned */
    .multiboot ALIGN(8) :
    {
        *(.multiboot)
    }
    
    /* Text section (code) */
    .text ALIGN(4K) :
    {
        *(.text)
        *(.text.*)
    }
    
    /* Read-only data */
    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    }
    
    /* Initialized data */
    .data ALIGN(4K) :
    {
        *(.data)
        *(.data.*)
    }
    
    /* Uninitialized data */
    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
    
    /* Kernel end symbol for memory management */
    KERNEL_END = .;
    
    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.gnu.hash)
    }
}
