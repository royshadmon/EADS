#include "dma.h"

/*
 * This file contains the interface to the BCM2835 DMA controller.
 *
 * Documentation is available on page 38 of the peripherals guide:
 * https://www.raspberrypi.org/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
 *
 */

#include "virtual_memory.h"

/*
 * dma_get_chan_regs
 *
 * Get a pointer to the DMA control registers mapped in to virtual memory
 */
DMA_CHAN_REGS* dma_get_chan_regs(void) {
    return (DMA_CHAN_REGS*) map_segment((void*)DMA_REG_BASE, PAGE_SIZE);
}

/*
 * dma_free_chan_regs
 *
 * Free the virtual memory associated with the DMA control registers
 */
void dma_free_chan_regs(DMA_CHAN_REGS* regs) {
    unmap_segment(regs, PAGE_SIZE);
}

/*
 * dma_enable_chan
 *
 * Enable and reset the DMA channel
 */
void dma_enable_chan(DMA_CHAN_REGS* regs, uint32_t chan) {
    // Enable the channel in power settings
    regs->enable |= (1 << chan);

    // Reset the channel
    regs->channels[chan].cs |= (1 << 31);
}

/*
 * dma_start_chan
 *
 * Start the DMA channel with the given control block
 */
void dma_start_chan(DMA_CHAN_REGS* regs, uint32_t chan, void* cb) {
    // Set the channel control block
    regs->channels[chan].conblk_ad = (uint32_t) cb;

    // Write to bit 1 on CS to clear the 'end' flag
    regs->channels[chan].cs = 0x02;

    // Clear the dma error flags by writing to bits 0,1,2 in debug
    regs->channels[chan].debug = 0x07;

    // Write to bit 0 on CS to start the dma
    regs->channels[chan].cs = 0x01;
}

/*
 * dma_stop_chan
 *
 * Stop the DMA channel
 */
void dma_stop_chan(DMA_CHAN_REGS* regs, uint32_t chan) {
    // Clear the transfer active bits
    regs->channels[chan].cs = 0x00;
}
