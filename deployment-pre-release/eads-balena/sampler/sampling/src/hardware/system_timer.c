#include "system_timer.h"

/*
 * This file contains the interface to the BCM2835 system timer.
 *
 * Documentation is available on page 172 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <assert.h>

#include "virtual_memory.h"

/*
 * system_timer_get_regs
 *
 * Get a pointer to the System Timer control registers mapped in to virtual memory
 */
SYSTEM_TIMER_REGS* system_timer_get_regs(void) {
    return (SYSTEM_TIMER_REGS*) map_segment((void*)SYSTEM_TIMER_BASE, PAGE_SIZE);
}

/*
 * system_timer_free_regs
 *
 * Free the virtual memory associated with the System Timer control registers
 */
void system_timer_free_regs(SYSTEM_TIMER_REGS* regs) {
    unmap_segment(regs, PAGE_SIZE);
}
