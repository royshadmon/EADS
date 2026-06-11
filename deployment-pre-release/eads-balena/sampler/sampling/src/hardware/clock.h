#ifndef CLOCK_H
#define CLOCK_H

/*
 * This file contains the interface to the BCM2835 clock controller.
 *
 * This is based on the undocumented BCM2835 register CM which is
 * documented here: https://elinux.org/BCM2835_registers#CM
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include "util.h"

//
// Clock register memory mapping
//

// BCM2835 clock registers are located at offset 0x101000
#define CLOCK_REG_BASE (PHYS_REG_BASE + 0x101000)

// BCM2835 clock register password
#define CLOCK_REG_PASSWORD 0x5a000000

typedef struct {
    volatile uint32_t gnricctl;
    volatile uint32_t gnricdiv;
    volatile uint32_t vpuctl;
    volatile uint32_t vpudiv;
    volatile uint32_t sysctl;
    volatile uint32_t sysdiv;
    volatile uint32_t periactl;
    volatile uint32_t periadiv;
    volatile uint32_t periictl;
    volatile uint32_t periidiv;
    volatile uint32_t h264ctl;
    volatile uint32_t h264div;
    volatile uint32_t ispctl;
    volatile uint32_t ispdiv;
    volatile uint32_t v3dctl;
    volatile uint32_t v3ddiv;
    volatile uint32_t cam0ctl;
    volatile uint32_t cam0div;
    volatile uint32_t cam1ctl;
    volatile uint32_t cam1div;
    volatile uint32_t ccp2ctl;
    volatile uint32_t ccp2div;
    volatile uint32_t dsi0ectl;
    volatile uint32_t dsi0ediv;
    volatile uint32_t dsi0pctl;
    volatile uint32_t dsi0pdiv;
    volatile uint32_t dpictl;
    volatile uint32_t dpidiv;
    volatile uint32_t gp0ctl;
    volatile uint32_t gp0div;
    volatile uint32_t gp1ctl;
    volatile uint32_t gp1div;
    volatile uint32_t gp2ctl;
    volatile uint32_t gp2div;
    volatile uint32_t hsmctl;
    volatile uint32_t hsmdiv;
    volatile uint32_t otpctl;
    volatile uint32_t otpdiv;
    volatile uint32_t pcmctl;
    volatile uint32_t pcmdiv;
    volatile uint32_t pwmctl;
    volatile uint32_t pwmdiv;
} CLOCK_REGS;

/*
 * clock_get_regs
 *
 * Get a pointer to the clock control registers mapped in to virtual memory
 */
CLOCK_REGS* clock_get_regs(void);

/*
 * clock_free_regs
 *
 * Free the virtual memory associated with the clock control registers
 */
void clock_free_regs(CLOCK_REGS* regs);

/*
 * BCM 2835 DMA TI register flags. See:
 * https://elinux.org/BCM2835_registers#CM
 */
typedef enum {
    CLOCK_PWMCTL_ENAB  = 1<<4,
    CLOCK_PWMCTL_KILL  = 1<<5,
    CLOCK_PWMCTL_BUSY  = 1<<7,
    CLOCK_PWMCTL_BUSYD = 1<<8,
    CLOCK_PWMCTL_MASH  = 1<<9,
} CLOCK_PWMCTL_FLAGS;

#endif // CLOCK_H
