#ifndef SPI_H
#define SPI_H

/*
 * This file contains the interface to the BCM2835 SPI controller.
 *
 * Documentation is available on page 148 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <stdint.h>

#include "util.h"

/*
 *
 * SPI register memory mapping
 *
 */

// BCM2835 SPI registers are located at offset 0x204000
#define SPI_REG_BASE (PHYS_REG_BASE + 0x204000)

typedef struct {
    volatile uint32_t cs;   // SPI Master Control and Status
    volatile uint32_t fifo; // SPI Master TX and RX FIFOs
    volatile uint32_t clk;  // SPI Master Clock Divider
    volatile uint32_t dlen; // SPI Master Data Length
    volatile uint32_t ltoh; // SPI LOSSI mode TOH
    volatile uint32_t dc;   // SPI DMA DREQ Controls
} SPI_REGS;

#define SPI_BUS_ADDR(m) (SPI_REG_BASE - PHYS_REG_BASE + BUS_REG_BASE + offsetof(SPI_REGS, m))

/*
 * spi_get_regs
 *
 * Get a pointer to the SPI control registers mapped in to virtual memory
 */
SPI_REGS* spi_get_regs(void);

/*
 * spi_free_regs
 *
 * Free the virtual memory associated with the SPI control registers
 */
void spi_free_regs(SPI_REGS* regs);

typedef enum {
    SPI_TXD      = (1 << 18), // SPI TX FIFO can accept data
    SPI_RXD      = (1 << 17), // SPI RX FIFO contains data
    SPI_DONE     = (1 << 16), // SPI Transfer Done
    SPI_REN      = (1 << 12), // SPI Read Enable
    SPI_ADCS     = (1 << 11), // SPI Automatically Deassert Chip Select
    SPI_DMAEN    = (1 << 8),  // SPI Enable DMA Control
    SPI_TA       = (1 << 7),  // SPI Transfer Active
    SPI_CLEAR_RX = (1 << 5),  // SPI Clear RX FIFO
    SPI_CLEAR_TX = (1 << 4)   // SPI Clear TX FIFO
} SPI_CS_FLAGS;

#endif // SPI_H
