#ifndef SYSTEM_TIMER_H
#define SYSTEM_TIMER_H

/*
 * This file contains the interface to the BCM2835 system timer.
 *
 * Documentation is available on page 172 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <stdint.h>

#include "util.h"

/*
 *
 * System Timer register memory mapping
 *
 */

// BCM2835 System Timer registers are located at offset 0x3000
#define SYSTEM_TIMER_BASE (PHYS_REG_BASE + 0x3000)

typedef struct {
    volatile uint32_t cs;  // System Timer Control/Status
    volatile uint32_t clo; // System Timer Counter Lower 32 bits
    volatile uint32_t chi; // System Timer Counter Higher 32 bits
    volatile uint32_t c0;  // System Timer Compare 0
    volatile uint32_t c1;  // System Timer Compare 1
    volatile uint32_t c2;  // System Timer Compare 2
    volatile uint32_t c3;  // System Timer Compare 3
} SYSTEM_TIMER_REGS;

#define SYSTEM_TIMER_BUS_ADDR(m) (SYSTEM_TIMER_BASE - PHYS_REG_BASE + BUS_REG_BASE + offsetof(SYSTEM_TIMER_REGS, m))

/*
 * system_timer_get_regs
 *
 * Get a pointer to the System Timer control registers mapped in to virtual memory
 */
SYSTEM_TIMER_REGS* system_timer_get_regs(void);

/*
 * system_timer_free_regs
 *
 * Free the virtual memory associated with the System Timer control registers
 */
void system_timer_free_regs(SYSTEM_TIMER_REGS* regs);

#endif // SYSTEM_TIMER_H
