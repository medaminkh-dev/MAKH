/**
 * MakhOS - serial.c
 * Serial port driver implementation for COM1
 */

#include "serial.h"
#include "kernel.h"

/* Serial port initialized flag */
static int serial_initialized = 0;

/**
 * serial_init - Initialize the serial port (COM1)
 * Configures for 115200 baud, 8N1, enables FIFO
 */
int serial_init(void) {
    /* Disable interrupts */
    outb(SERIAL_INT_EN_PORT, 0x00);
    
    /* Enable DLAB (Divisor Latch Access Bit) to set baud rate */
    outb(SERIAL_LINE_CTRL_PORT, SERIAL_LCR_DLAB);
    
    /* Set divisor for 115200 baud (low byte) */
    /* 115200 / 115200 = 1 */
    outb(SERIAL_DATA_PORT, 0x01);
    
    /* Set divisor (high byte) */
    outb(SERIAL_INT_EN_PORT, 0x00);
    
    /* Configure 8N1 (8 data bits, no parity, 1 stop bit) */
    /* 0x03 = 8 data bits, no parity, 1 stop bit (8N1) */
    outb(SERIAL_LINE_CTRL_PORT, 0x03);
    
    /* Enable FIFO, clear RX and TX FIFOs */
    outb(SERIAL_FIFO_CTRL_PORT, SERIAL_FIFO_ENABLE | SERIAL_FIFO_CLEAR_RX | SERIAL_FIFO_CLEAR_TX);
    
    /* Set DTR, RTS, and OUT2 (interrupts enabled via OUT2) */
    outb(SERIAL_MODEM_CTRL_PORT, 0x03 | 0x08);
    
    serial_initialized = 1;
    return 0;
}

/**
 * serial_is_initialized - Check if serial port has been initialized
 */
int serial_is_initialized(void) {
    return serial_initialized;
}

/**
 * serial_is_transmit_empty - Check if transmit buffer is empty
 */
int serial_is_transmit_empty(void) {
    /* Read line status register and check TX empty bit */
    return (inb(SERIAL_LINE_STS_PORT) & SERIAL_LSR_TX_EMPTY) != 0;
}

/**
 * serial_write_char - Write a single character to serial port
 */
void serial_write_char(char c) {
    /* Wait until transmit buffer is empty */
    while (!serial_is_transmit_empty()) {
        /* Busy wait */
    }
    
    /* Write character to data port */
    outb(SERIAL_DATA_PORT, (uint8_t)c);
}

/**
 * serial_write - Write a buffer to serial port
 */
void serial_write(const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        serial_write_char(str[i]);
    }
}

/**
 * serial_writestring - Write a null-terminated string to serial port
 */
void serial_writestring(const char* str) {
    size_t i = 0;
    while (str[i] != '\0') {
        serial_write_char(str[i]);
        i++;
    }
}
