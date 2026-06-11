#ifndef DMA_H
#define DMA_H

/*
 * This file contains the interface to the BCM2835 DMA controller.
 *
 * Documentation is available on page 38 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include <stdint.h>
#include <assert.h>

#include "util.h"

/*
 *
 * DMA channel memory mapping
 *
 */

// BCM2835 DMA registers are located at offset 0x007000
#define DMA_REG_BASE (PHYS_REG_BASE + 0x007000)

typedef struct {
    volatile uint32_t cs;        // DMA Channel Control and Status
    volatile uint32_t conblk_ad; // DMA Channel Control Block Adddress
    volatile uint32_t ti;        // DMA Channel CB Word 0 (Transfer Information)
    volatile uint32_t source_ad; // DMA Channel CB Word 1 (Source Address)
    volatile uint32_t dest_ad;   // DMA Channel CB Word 2 (Destination Address)
    volatile uint32_t txfr_len;  // DMA Channel CB Word 3 (Transfer Length)
    volatile uint32_t stride;    // DMA Channel CB Word 4 (2D Stride)
    volatile uint32_t nextconbk; // DMA Channel CB Word 5 (Next CB Address)
    volatile uint32_t debug;     // DMS Channel Debug
    uint8_t padding[0x100 - 9 * sizeof(uint32_t)];
} DMA_CHAN_REG __attribute__ ((aligned(0x100)));
static_assert(sizeof(DMA_CHAN_REG) == 0x100, "DMA channel registers should be of length 0x100!");

typedef struct {
    DMA_CHAN_REG channels[15];
    volatile uint32_t int_status;
    volatile uint32_t enable;
} DMA_CHAN_REGS;

/*
 * dma_get_chan_regs
 *
 * Get a pointer to the DMA control registers mapped in to virtual memory
 */
DMA_CHAN_REGS* dma_get_chan_regs(void);

/*
 * dma_free_chan_regs
 *
 * Free the virtual memory associated with the DMA control registers
 */
void dma_free_chan_regs(DMA_CHAN_REGS* regs);

/*
 * BCM 2835 DMA TI register flags. See:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 */
typedef enum {
    DMA_PERMAP_NONE   = 0<<16, // The always-on DREQ
    DMA_PERMAP_PWM    = 5<<16, // PWM DREQ
    DMA_PERMAP_SPI_TX = 6<<16, // SPI TX DREQ
    DMA_PERMAP_SPI_RX = 7<<16, // SPI RX DREQ
    DMA_SRC_DREQ      = 1<<10, // Enable the source DREQ selected by PERMAP
    DMA_SRC_INC       = 1<<8,  // Increment the source address each write
    DMA_DEST_DREQ     = 1<<6,  // Enable the destination DREQ selected by PERMAP
    DMA_DEST_INC      = 1<<4,  // Increment the destination address each write
    DMA_WAIT_RESP     = 1<<3,  // Wait for a Write Response
    DMA_2D_MODE       = 1<<1   // 2D Mode
} DMA_TI_FLAGS;

// DMA control block (must be 32-byte aligned)
typedef struct {
    uint32_t ti,    // Transfer info
        srce_ad,    // Source address
        dest_ad,    // Destination address
        tfr_len,    // Transfer length
        stride,     // Transfer stride
        next_cb,    // Next control block
        debug,      // Debug register
        unused;
} DMA_CB __attribute__ ((aligned(32)));

/*
 * dma_enable_chan
 *
 * Enable and reset the DMA channel
 */
void dma_enable_chan(DMA_CHAN_REGS* regs, uint32_t chan);

/*
 * dma_start_chan
 *
 * Start the DMA channel with the given control block
 */
void dma_start_chan(DMA_CHAN_REGS* regs, uint32_t chan, void* cb);

/*
 * dma_stop_chan
 *
 * Stop the DMA channel
 */
void dma_stop_chan(DMA_CHAN_REGS* regs, uint32_t chan);

#endif // DMA_H
