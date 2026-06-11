#ifndef PWM_H
#define PWM_H

/*
 * This file contains the interface to the BCM2835 PWM controller.
 *
 * Documentation is available on page 138 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <stdint.h>

#include "util.h"
#include "clock.h"

/*
 *
 * PWM register memory mapping
 *
 */

// BCM2835 PWM registers are located at offset 0x20C000
#define PWM_REG_BASE (PHYS_REG_BASE + 0x20C000)

typedef struct {
    // Control registers
    volatile uint32_t ctl;  // PWM Control
    volatile uint32_t sta;  // PWM Status
    volatile uint32_t dmac; // PWM DMA Configuration

    volatile uint32_t reserved0;

    // Channel 1 registers
    volatile uint32_t rng1; // PWM Channel 1 Range
    volatile uint32_t dat1; // PWM Channel 1 Data
    volatile uint32_t fif1; // PWM FIFO Input

    volatile uint32_t reserved1;

    // Channel 2 registers
    volatile uint32_t rng2; // PWM Channel 2 Range
    volatile uint32_t dat2; // PWM Channel 2 Data
} PWM_REGS;

#define PWM_BUS_ADDR(m) (PWM_REG_BASE - PHYS_REG_BASE + BUS_REG_BASE + offsetof(PWM_REGS, m))

/*
 * pwm_get_regs
 *
 * Get a pointer to the PWM control registers mapped in to virtual memory
 */
PWM_REGS* pwm_get_regs(void);

/*
 * pwm_free_regs
 *
 * Free the virtual memory associated with the PWM control registers
 */
void pwm_free_regs(PWM_REGS* regs);

/*
 * pwm_start
 *
 * Start the PWM controller
 */
void pwm_start(PWM_REGS* regs);

/*
 * pwm_stop
 *
 * Stop the PWM controller
 */
void pwm_stop(PWM_REGS* regs);

/*
 * pwm_set_clock_freq
 *
 * Set the PWM clock to the specified frequency in Hz using the VC mailox
 */
void pwm_set_clock_freq(CLOCK_REGS* regs, uint32_t freq);

#endif // PWM_H
