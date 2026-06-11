#include "gpio.h"

/*
 * This file contains the interface to the BCM2835 GPIO controller.
 *
 * Documentation is available on page 89 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <unistd.h>
#include <assert.h>

#include "virtual_memory.h"

/*
 * gpio_get_regs
 *
 * Get a pointer to the GPIO control registers mapped in to virtual memory
 */
GPIO_REGS* gpio_get_regs(void) {
    return (GPIO_REGS*) map_segment((void*)GPIO_REG_BASE, PAGE_SIZE);
}

/*
 * gpio_free_regs
 *
 * Free the virtual memory associated with the GPIO control registers
 */
void gpio_free_regs(GPIO_REGS* regs) {
    unmap_segment(regs, PAGE_SIZE);
}

/*
 * gpio_set_mode
 *
 * Set the mode of the pin
 */
void gpio_set_mode(GPIO_REGS* regs, uint32_t pin, GPIO_FSEL_FLAGS mode) {
    uint32_t offset = pin / 10;
    volatile uint32_t* addr = &regs->gpfsel0 + offset;
    *addr = (*addr & ~(0x07 << (3 * (pin % 10))))
          | (mode << (3 * (pin % 10)));
}

/*
 * gpio_set_pull
 *
 * Set the state of the internal pull-up and pull-down resistor on the pin
 */
void gpio_set_pull(GPIO_REGS* regs, uint32_t pin, GPIO_PIN_PULL pull) {
    // Set the gpio pulldown value register (GPPUD)
    regs->gppud = pull;

    // Sleep for at least 150 cycles
    usleep(10);

    // Clock in the control signal
    uint32_t offset = pin / 32;
    *(&regs->gppudclk0 + offset) = 1 << (pin % 32);

    // Sleep for at least 150 cycles
    usleep(10);

    // Remove control signal from GPPUD
    regs->gppud = 0;
    *(&regs->gppudclk0 + offset) = 0;
}

/*
 * gpio_setup
 *
 * Setup the pin with the given mode and pull
 */
void gpio_setup(GPIO_REGS* regs, uint32_t pin, GPIO_FSEL_FLAGS mode, GPIO_PIN_PULL pull) {
    gpio_set_mode(regs, pin, mode);
    gpio_set_pull(regs, pin, pull);
}

/*
 * gpio_set_pin
 *
 * Set the output state of the pin
 */
void gpio_set_pin(GPIO_REGS* regs, uint32_t pin, GPIO_PIN_VALUE value) {
    uint32_t offset = pin / 32;
    if (value == HIGH) {
        *(&regs->gpset0 + offset) = 1 << (pin % 32);
    } else if (value == LOW) {
        *(&regs->gpclr0 + offset) = 1 << (pin % 32);
    } else {
        assert(0);
    }
}
