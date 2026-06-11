#include "spi.h"

/*
 * This file contains the interface to the BCM2835 SPI controller.
 *
 * Documentation is available on page 148 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <assert.h>

#include "virtual_memory.h"

/*
 * spi_get_regs
 *
 * Get a pointer to the SPI control registers mapped in to virtual memory
 */
SPI_REGS* spi_get_regs(void) {
    return (SPI_REGS*) map_segment((void*)SPI_REG_BASE, PAGE_SIZE);
}

/*
 * spi_free_regs
 *
 * Free the virtual memory associated with the SPI control registers
 */
void spi_free_regs(SPI_REGS* regs) {
    unmap_segment(regs, PAGE_SIZE);
}
