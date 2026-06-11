#ifndef GPIO_H
#define GPIO_H

/*
 * This file contains the interface to the BCM2835 GPIO controller.
 *
 * Documentation is available on page 89 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <stdint.h>

#include "util.h"

/*
 *
 * GPIO register memory mapping
 *
 */

// BCM2835 GPIO registers are located at offset 0x200000 
#define GPIO_REG_BASE    (PHYS_REG_BASE + 0x200000)

typedef struct {
    volatile uint32_t gpfsel0;   // GPIO Function Select 0
    volatile uint32_t gpfsel1;   // GPIO Function Select 1
    volatile uint32_t gpfsel2;   // GPIO Function Select 2
    volatile uint32_t gpfsel3;   // GPIO Function Select 3
    volatile uint32_t gpfsel4;   // GPIO Function Select 4
    volatile uint32_t gpfsel5;   // GPIO Function Select 5
    uint32_t reserved0;
    volatile uint32_t gpset0;    // GPIO Pin Output Set 0
    volatile uint32_t gpset1;    // GPIO Pin Output Set 1
    uint32_t reserved1;
    volatile uint32_t gpclr0;    // GPIO Pin Output Set 0
    volatile uint32_t gpclr1;    // GPIO Pin Output Set 1
    uint32_t reserved2;
    volatile uint32_t gplev0;    // GPIO Pin Level 0
    volatile uint32_t gplev1;    // GPIO Pin Level 1
    uint32_t reserved3;
    volatile uint32_t gpeds0;    // GPIO Pin Event Detect Status 0
    volatile uint32_t gpeds1;    // GPIO Pin Event Detect Status 1
    uint32_t reserved4;
    volatile uint32_t gpren0;    // GPIO Pin Rising Edge Detect Enable 0
    volatile uint32_t gpren1;    // GPIO Pin Rising Edge Detect Enable 1
    uint32_t reserved5;
    volatile uint32_t gpfen0;    // GPIO Pin Falling Edge Detect Enable 0
    volatile uint32_t gpfen1;    // GPIO Pin Falling Edge Detect Enable 1
    uint32_t reserved6;
    volatile uint32_t gphen0;    // GPIO Pin High Detect Enable 0
    volatile uint32_t gphen1;    // GPIO Pin High Detect Enable 1
    uint32_t reserved7;
    volatile uint32_t gplen0;    // GPIO Pin Low Detect Enable 0
    volatile uint32_t gplen1;    // GPIO Pin Low Detect Enable 1
    uint32_t reserved8;
    volatile uint32_t gparen0;   // GPIO Pin Async Rising Detect Enable 0
    volatile uint32_t gparen1;   // GPIO Pin Async Rising Detect Enable 1
    uint32_t reserved9;
    volatile uint32_t gpafen0;   // GPIO Pin Async Falling Detect Enable 0
    volatile uint32_t gpafen1;   // GPIO Pin Async Falling Detect Enable 1
    uint32_t reserved10;
    volatile uint32_t gppud;     // GPIO Pin Pull-up/down Enable
    volatile uint32_t gppudclk0; // GPIO Pin Pull-up/down Enable Clock 0
    volatile uint32_t gppudclk1; // GPIO Pin Pull-up/down Enable Clock 1
} GPIO_REGS;

#define GPIO_BUS_ADDR(m) (GPIO_REG_BASE - PHYS_REG_BASE + BUS_REG_BASE + offsetof(GPIO_REGS, m))

/*
 * gpio_get_regs
 *
 * Get a pointer to the GPIO control registers mapped in to virtual memory
 */
GPIO_REGS* gpio_get_regs(void);

/*
 * gpio_free_regs
 *
 * Free the virtual memory associated with the GPIO control registers
 */
void gpio_free_regs(GPIO_REGS* regs);

/*
 * BCM 2835 GPIO function select register flags. See:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 */
typedef enum {
    GPIO_FSEL_INPUT  = 0, // GPIO Pin input
    GPIO_FSEL_OUTPUT = 1, // GPIO Pin output
    GPIO_FSEL_ALT0   = 4, // GPIO Pin alternate function 0
    GPIO_FSEL_ALT1   = 5, // GPIO Pin alternate function 1
    GPIO_FSEL_ALT2   = 6, // GPIO Pin alternate function 2
    GPIO_FSEL_ALT3   = 7, // GPIO Pin alternate function 3
    GPIO_FSEL_ALT4   = 3, // GPIO Pin alternate function 4
    GPIO_FSEL_ALT5   = 2, // GPIO Pin alternate function 5
} GPIO_FSEL_FLAGS;


/*
 * gpio_set_mode
 *
 * Set the mode of the pin
 */
void gpio_set_mode(GPIO_REGS* regs, uint32_t pin, GPIO_FSEL_FLAGS mode);

typedef enum {
    GPIO_NOPULL    = 0,
    GPIO_PULL_DOWN = 1,
    GPIO_PULL_UP   = 2
} GPIO_PIN_PULL;

/*
 * gpio_set_pull
 *
 * Set the state of the internal pull-up and pull-down resistor on the pin
 */
void gpio_set_pull(GPIO_REGS* regs, uint32_t pin, GPIO_PIN_PULL pull);

/*
 * gpio_setup
 *
 * Setup the pin with the given mode and pull
 */
void gpio_setup(GPIO_REGS* regs, uint32_t pin, GPIO_FSEL_FLAGS mode, GPIO_PIN_PULL pull);


typedef enum {
    HIGH,
    LOW
} GPIO_PIN_VALUE;

/*
 * gpio_set_pin
 *
 * Set the output state of the pin
 */
void gpio_set_pin(GPIO_REGS* regs, uint32_t pin, GPIO_PIN_VALUE value);

#endif // GPIO_H
