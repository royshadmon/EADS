#include "clock.h"

/*
 * This file contains the interface to the BCM2835 clock controller.
 *
 * This is based on the undocumented BCM2835 register CM which is
 * documented here: https://elinux.org/BCM2835_registers#CM
 *
 */

#include "virtual_memory.h"

/*
 * clock_get_regs
 *
 * Get a pointer to the clock control registers mapped in to virtual memory
 */
CLOCK_REGS* clock_get_regs(void) {
    return (CLOCK_REGS*) map_segment((void*)CLOCK_REG_BASE, PAGE_SIZE);
}

/*
 * clock_free_regs
 *
 * Free the virtual memory associated with the clock control registers
 */
void clock_free_regs(CLOCK_REGS* regs) {
    unmap_segment(regs, PAGE_SIZE);
}
