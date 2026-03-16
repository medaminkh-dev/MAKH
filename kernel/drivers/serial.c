/**
 * =============================================================================
 * serial.c - Serial Port Driver for MakhOS
 * =============================================================================
 * Implements serial port I/O for COM1 (0x3F8)
 * Used for capturing kernel output in QEMU with -serial flag
 * =============================================================================
 */

#include "serial.h"
#include "kernel.h"
#include "types.h"

/* Static initialization flag */
static int serial_initialized = 0;

/**
 * serial_init - Initialize the serial port (COM1)
 * Configures for 115200 baud, 8N1 (8 data, no parity, 1 stop)
 * Enables and clears FIFOs
 * 
 * @return: 0 on success
 */
int serial_init(void) {
    if (serial_initialized) {
        return 0;
    }
    
    /* Disable interrupts */
    outb(SERIAL_INT_EN_PORT, 0x00);
    
    /* Enable DLAB (Divisor Latch Access Bit) to set baud rate */
    outb(SERIAL_LINE_CTRL_PORT, SERIAL_LCR_DLAB);
    
    /* Set divisor to 1 (115200 baud) */
    outb(SERIAL_DATA_PORT, 0x01);    /* Divisor LSB */
    outb(SERIAL_INT_EN_PORT, 0x00);  /* Divisor MSB */
    
    /* Set 8N1 (8 data, no parity, 1 stop) and disable DLAB */
    outb(SERIAL_LINE_CTRL_PORT, 0x03);
    
    /* Enable FIFO, clear TX/RX FIFOs */
    outb(SERIAL_FIFO_CTRL_PORT, SERIAL_FIFO_ENABLE | SERIAL_FIFO_CLEAR_RX | SERIAL_FIFO_CLEAR_TX);
    
    /* Set IRQs enabled, UART in FIFO mode */
    outb(SERIAL_FIFO_CTRL_PORT, 0xC7);
    
    /* Set modem control to loopback mode (for testing) then normal */
    outb(SERIAL_MODEM_CTRL_PORT, 0x0B);
    
    /* Wait a bit for UART to settle */
    for (volatile int i = 0; i < 1000; i++);
    
    /* Switch back to normal operation */
    outb(SERIAL_MODEM_CTRL_PORT, 0x00);
    
    serial_initialized = 1;
    return 0;
}

/**
 * serial_is_initialized - Check if serial port has been initialized
 * 
 * @return: 1 if serial is initialized, 0 otherwise
 */
int serial_is_initialized(void) {
    return serial_initialized;
}

/**
 * serial_is_transmit_empty - Check if transmit buffer is empty
 * 
 * @return: 1 if transmit buffer is empty, 0 otherwise
 */
int serial_is_transmit_empty(void) {
    return inb(SERIAL_LINE_STS_PORT) & SERIAL_LSR_TX_EMPTY;
}

/**
 * serial_write_char - Write a single character to serial port
 * Blocks until character is sent
 * 
 * @c: Character to write
 */
void serial_write_char(char c) {
    if (!serial_initialized) {
        return;
    }
    
    /* Wait for transmit buffer to be empty */
    while (!serial_is_transmit_empty());
    
    outb(SERIAL_DATA_PORT, c);
}

/**
 * serial_write - Write a string to serial port
 * 
 * @str: String to write
 * @len: Length of string
 */
void serial_write(const char* str, size_t len) {
    if (!serial_initialized) {
        return;
    }
    
    for (size_t i = 0; i < len; i++) {
        serial_write_char(str[i]);
    }
}

/**
 * serial_writestring - Write a null-terminated string to serial port
 * 
 * @str: String to write (null-terminated)
 */
void serial_writestring(const char* str) {
    if (!serial_initialized) {
        return;
    }
    
    size_t i = 0;
    while (str[i] != '\0') {
        serial_write_char(str[i]);
        i++;
    }
}
