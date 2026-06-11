#include "pwm.h"

/*
 * This file contains the interface to the BCM2835 PWM controller.
 *
 * Documentation is available on page 138 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <assert.h>

#include "mailbox.h"
#include "virtual_memory.h"
#include "clock.h"

/*
 * pwm_get_regs
 *
 * Get a pointer to the PWM control registers mapped in to virtual memory
 */
PWM_REGS* pwm_get_regs(void) {
    return (PWM_REGS*) map_segment((void*)PWM_REG_BASE, PAGE_SIZE);
}

/*
 * pwm_free_regs
 *
 * Free the virtual memory associated with the PWM control registers
 */
void pwm_free_regs(PWM_REGS* regs) {
    unmap_segment(regs, PAGE_SIZE);
}

/*
 * pwm_start
 *
 * Start the PWM controller
 */
void pwm_start(PWM_REGS* regs) {
    regs->ctl |= 1;
}

/*
 * pwm_stop
 *
 * Stop the PWM controller
 */
void pwm_stop(PWM_REGS* regs) {
    regs->ctl &= ~1;
}

/*
 * pwm_set_clock_freq
 *
 * Set the PWM clock to the specified frequency in Hz using the clock registers
 */
void pwm_set_clock_freq(CLOCK_REGS* regs, uint32_t freq) {
    uint32_t clock_divider = CLOCK_HZ / freq;

    // Kill the clock
    regs->pwmctl = CLOCK_REG_PASSWORD | CLOCK_PWMCTL_KILL;
    // Wait for the clock to stop asserting busy
    while(regs->pwmctl & CLOCK_PWMCTL_BUSY);

    // Now that the clock is stopped, we can set the divider field
    regs->pwmdiv = CLOCK_REG_PASSWORD | (clock_divider << 12);

    // Start the clock again with src=6
    regs->pwmctl = CLOCK_REG_PASSWORD | CLOCK_PWMCTL_ENAB | 6;
    // Wait for the clock to start asserting busy again
    while(!(regs->pwmctl & CLOCK_PWMCTL_BUSY));
}
