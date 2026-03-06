/**
 * =============================================================================
 * serial.h - Serial Port Driver Header for MakhOS
 * =============================================================================
 * Provides definitions for serial port output (COM1)
 * Used for capturing kernel output in QEMU with -serial flag
 * =============================================================================
 */

#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

/* Serial port I/O ports (COM1) */
#define SERIAL_COM1_BASE      0x3F8
#define SERIAL_DATA_PORT      (SERIAL_COM1_BASE + 0)   /* Data register (R/W) */
#define SERIAL_INT_EN_PORT    (SERIAL_COM1_BASE + 1)   /* Interrupt enable */
#define SERIAL_FIFO_CTRL_PORT (SERIAL_COM1_BASE + 2)   /* FIFO control */
#define SERIAL_LINE_CTRL_PORT (SERIAL_COM1_BASE + 3)   /* Line control */
#define SERIAL_MODEM_CTRL_PORT (SERIAL_COM1_BASE + 4)  /* Modem control */
#define SERIAL_LINE_STS_PORT  (SERIAL_COM1_BASE + 5)   /* Line status */
#define SERIAL_MODEM_STS_PORT (SERIAL_COM1_BASE + 6)   /* Modem status */

/* Line control register bits */
#define SERIAL_LCR_DLAB       0x80    /* Divisor latch access bit */

/* Line status register bits */
#define SERIAL_LSR_DATA_READY 0x01    /* Data ready */
#define SERIAL_LSR_TX_EMPTY   0x20    /* Transmitter empty */

/* FIFO control register bits */
#define SERIAL_FIFO_ENABLE    0x01    /* Enable FIFO */
#define SERIAL_FIFO_CLEAR_RX  0x02    /* Clear receive FIFO */
#define SERIAL_FIFO_CLEAR_TX  0x04    /* Clear transmit FIFO */

/**
 * serial_init - Initialize the serial port (COM1)
 * Configures for 115200 baud, 8N1 (8 data, no parity, 1 stop)
 * Enables and clears FIFOs
 * 
 * @return: 0 on success
 */
int serial_init(void);

/**
 * serial_is_initialized - Check if serial port has been initialized
 * 
 * @return: 1 if serial is initialized, 0 otherwise
 */
int serial_is_initialized(void);

/**
 * serial_is_transmit_empty - Check if transmit buffer is empty
 * 
 * @return: 1 if transmit buffer is empty, 0 otherwise
 */
int serial_is_transmit_empty(void);

/**
 * serial_write_char - Write a single character to serial port
 * Blocks until character is sent
 * 
 * @c: Character to write
 */
void serial_write_char(char c);

/**
 * serial_write - Write a string to serial port
 * 
 * @str: String to write
 * @len: Length of string
 */
void serial_write(const char* str, size_t len);

/**
 * serial_writestring - Write a null-terminated string to serial port
 * 
 * @str: String to write (null-terminated)
 */
void serial_writestring(const char* str);

#endif /* SERIAL_H */
