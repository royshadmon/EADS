#include "dma_sampling.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "sampling_adc.h"

#include "hardware/virtual_memory.h"
#include "hardware/system_timer.h"
#include "hardware/clock.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

/*
 * sampling_channels
 *
 * Array for accessing enabled sampling channels. For more information on usage
 * see dma_sampling_buffers_t.
 */
uint32_t sampling_channels[SAMPLING_CHANNEL_COUNT] = SAMPLING_CHANNELS;

/*
 * dma_sampling_init
 *
 * Initialize the DMA sampling module, returning a struct containing pointers
 * to the ping pong buffers and their status flags.
 */
dma_sampling_buffers_t dma_sampling_init(dma_sampling_state_t* state, int mailbox_fd) {
    // Open mapped memory to control registers
    state->clock_regs = clock_get_regs();
    state->gpio_regs = gpio_get_regs();
    state->spi_regs = spi_get_regs();
    state->pwm_regs = pwm_get_regs();
    state->dma_regs = dma_get_chan_regs();

    // Allocate one page of memory for cb, src, and dest buffers
    state->cb_mem = alloc_uncached_memory(mailbox_fd, PAGE_SIZE);
    state->src_mem = alloc_uncached_memory(mailbox_fd, PAGE_SIZE);
    state->buffer_a_mem = alloc_uncached_memory(mailbox_fd, BUFFER_PAGES * PAGE_SIZE);
    state->buffer_b_mem = alloc_uncached_memory(mailbox_fd, BUFFER_PAGES * PAGE_SIZE);

    dma_sampling_buffers_t buffers = dma_sampling_init_memory(state, mailbox_fd);
    dma_sampling_init_hardware(state, mailbox_fd);

    return buffers;
}

/*
 * dma_sampling_cleanup
 *
 * Cleanup the DMA sampling module, freeing all mapped memory. The buffers
 * returned from dma_sampling_init are no longer valid after this call.
 */
void dma_sampling_cleanup(dma_sampling_state_t* state, int mailbox_fd) {
    // Free mapped memory to control registers
    clock_free_regs(state->clock_regs);
    gpio_free_regs(state->gpio_regs);
    spi_free_regs(state->spi_regs);
    pwm_free_regs(state->pwm_regs);
    dma_free_chan_regs(state->dma_regs);

    // Free uncached memory blocks
    free_uncached_memory(mailbox_fd, state->buffer_a_mem);
    free_uncached_memory(mailbox_fd, state->buffer_b_mem);
    free_uncached_memory(mailbox_fd, state->src_mem);
    free_uncached_memory(mailbox_fd, state->cb_mem);
}

/*
 * dma_sampling_init_memory
 *
 * Initialize DMA sampling memory.
 */
dma_sampling_buffers_t dma_sampling_init_memory(dma_sampling_state_t* state, int mailbox_fd) {
    (void) mailbox_fd;

    uint32_t* src_data = (uint32_t*)state->src_mem.virt;

    // Word 0: PWM range for pacing
    uint32_t* dma_pwm_range = &src_data[0];
    *dma_pwm_range = PWM_RANGE / 2;

    // Word 1: SPI CS to clear buffers
    uint32_t* dma_spi_clear = &src_data[1];
    *dma_spi_clear = SPI_CLEAR_RX | SPI_CLEAR_TX;

    // Word 2: Data length (3 bytes)
    uint32_t* dma_spi_dlen = &src_data[2];
    *dma_spi_dlen = 3;

    // Word 3: SPI CS to start transfer
    uint32_t* dma_spi_start = &src_data[3];
    *dma_spi_start = SPI_DMAEN | SPI_ADCS | SPI_TA;

    // Word 4: Buffer A full status
    volatile uint32_t* buffer_a_status = &src_data[4];
    *buffer_a_status = 0;

    // Word 5: Buffer B clock high
    volatile uint32_t* buffer_a_clock_high = &src_data[5];
    *buffer_a_clock_high = 0;

    // Word 6: Buffer B full status
    volatile uint32_t* buffer_b_status = &src_data[6];
    *buffer_b_status = 0;

    // Word 7: Buffer B clock high
    volatile uint32_t* buffer_b_clock_high = &src_data[7];
    *buffer_b_clock_high = 0;

    // Word 8+: SPI TX data
    //
    // NOTE: Here we are storing the data shifted forward by one in the array.
    //       This is done because the initial SPI TX packet is lost when the DMA
    //       sequence starts up. This has no effect on the output data.
    uint32_t* dma_tx_data = &src_data[8];
    for(size_t i = 1; i <= SAMPLING_CHANNEL_COUNT; i++) {
        dma_tx_data[i % SAMPLING_CHANNEL_COUNT] = sampling_adc_encode_request(sampling_channels[i-1]);
    }

    // Create DMA control blocks for copying the memory
    DMA_CB *cbs = state->cb_mem.virt;

    //
    // Loop 1: Pace SPI with PWM DREQ
    //
    // NOTE: Some issues can arise if the order of these is changed.
    // See https://elinux.org/BCM2835_datasheet_errata#p158
    //

    // CB 0: Reset FIFO
    cbs[0] = (DMA_CB)
        { .ti = DMA_DEST_DREQ | DMA_PERMAP_PWM | DMA_WAIT_RESP
        , .srce_ad = (uint32_t) mem_virt_to_phys(state->src_mem, dma_spi_clear)
        , .dest_ad = SPI_BUS_ADDR(cs)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[1])
        };

    // CB 1: Set DLEN for spi
    cbs[1] = (DMA_CB)
        { .ti = DMA_DEST_DREQ | DMA_PERMAP_PWM | DMA_WAIT_RESP
        , .srce_ad = (uint32_t) mem_virt_to_phys(state->src_mem, dma_spi_dlen)
        , .dest_ad = SPI_BUS_ADDR(dlen)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[2])
        };

    // CB 2: Set CS to start transfer
    cbs[2] = (DMA_CB)
        { .ti = DMA_DEST_DREQ | DMA_PERMAP_PWM | DMA_WAIT_RESP
        , .srce_ad = (uint32_t) mem_virt_to_phys(state->src_mem, dma_spi_start)
        , .dest_ad = SPI_BUS_ADDR(cs)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[3])
        };

    // CB 3: Clear PWM DMA flag
    cbs[3] = (DMA_CB)
        { .ti = DMA_DEST_DREQ | DMA_PERMAP_PWM | DMA_WAIT_RESP
        , .srce_ad = (uint32_t) mem_virt_to_phys(state->src_mem, dma_pwm_range)
        , .dest_ad = PWM_BUS_ADDR(fif1)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[0])
        };

    //
    // Loop 2: Read data from SPI on RX DREQ
    //

    // CB 4: Read from SPI FIFO into buffer A
    cbs[4] = (DMA_CB)
        { .ti = DMA_SRC_DREQ | DMA_PERMAP_SPI_RX | DMA_WAIT_RESP | DMA_2D_MODE
        , .srce_ad = SPI_BUS_ADDR(fifo)
        , .dest_ad = (uint32_t) state->buffer_a_mem.phys + sizeof(uint32_t)
        , .tfr_len = ((BUFFER_NUM_SAMPLES - 1) << 16) | sizeof(uint32_t)
        , .stride  = ((2 * sizeof(uint32_t)) << 16)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[5])
        };
    // CB 5: Set buffer A status
    cbs[5] = (DMA_CB)
        { .ti = 0
        , .srce_ad = SPI_BUS_ADDR(cs)
        , .dest_ad = (uint32_t) mem_virt_to_phys(state->src_mem, (uint32_t*) buffer_a_status)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[6])
        };

    // CB 6: Read from SPI FIFO into buffer B
    cbs[6] = (DMA_CB)
        { .ti = DMA_SRC_DREQ | DMA_PERMAP_SPI_RX | DMA_WAIT_RESP | DMA_2D_MODE
        , .srce_ad = SPI_BUS_ADDR(fifo)
        , .dest_ad = (uint32_t) state->buffer_b_mem.phys + sizeof(uint32_t)
        , .tfr_len = ((BUFFER_NUM_SAMPLES - 1) << 16) | sizeof(uint32_t)
        , .stride  = ((2 * sizeof(uint32_t)) << 16)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[7])
        };

    // CB 7: Set buffer B status
    cbs[7] = (DMA_CB)
        { .ti = 0
        , .srce_ad = SPI_BUS_ADDR(cs)
        , .dest_ad = (uint32_t) mem_virt_to_phys(state->src_mem, (uint32_t*) buffer_b_status)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[4])
        };


    //
    // Loop 3: Read system timer on RX DREQ
    //

    // CB 8: Read from System Timer CLO into buffer A
    cbs[8] = (DMA_CB)
        { .ti = DMA_SRC_DREQ | DMA_PERMAP_SPI_RX | DMA_WAIT_RESP | DMA_2D_MODE
        , .srce_ad = SYSTEM_TIMER_BUS_ADDR(clo)
        , .dest_ad = (uint32_t) state->buffer_a_mem.phys
        , .tfr_len = ((BUFFER_NUM_SAMPLES - 1) << 16) | sizeof(uint32_t)
        , .stride  = ((2 * sizeof(uint32_t)) << 16)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[9])
        };

    // CB 9: Set buffer A clock high
    cbs[9] = (DMA_CB)
        { .ti = 0
        , .srce_ad = SYSTEM_TIMER_BUS_ADDR(chi)
        , .dest_ad = (uint32_t) mem_virt_to_phys(state->src_mem, (uint32_t*) buffer_a_clock_high)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[10])
        };

    // CB 10: Read from System Timer CLO into buffer B
    cbs[10] = (DMA_CB)
        { .ti = DMA_SRC_DREQ | DMA_PERMAP_SPI_RX | DMA_WAIT_RESP | DMA_2D_MODE
        , .srce_ad = SYSTEM_TIMER_BUS_ADDR(clo)
        , .dest_ad = (uint32_t) state->buffer_b_mem.phys
        , .tfr_len = ((BUFFER_NUM_SAMPLES - 1) << 16) | sizeof(uint32_t)
        , .stride  = ((2 * sizeof(uint32_t)) << 16)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[11])
        };

    // CB 11: Set buffer B clock high
    cbs[11] = (DMA_CB)
        { .ti = 0
        , .srce_ad = SYSTEM_TIMER_BUS_ADDR(chi)
        , .dest_ad = (uint32_t) mem_virt_to_phys(state->src_mem, (uint32_t*) buffer_b_clock_high)
        , .tfr_len = sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[8])
        };

    //
    // Loop 4: Write data to SPI on TX DREQ
    //

    // CB 10: Write to TX FIFO
    cbs[12] = (DMA_CB)
        { .ti = DMA_DEST_DREQ | DMA_PERMAP_SPI_TX | DMA_WAIT_RESP
        , .srce_ad = (uint32_t) mem_virt_to_phys(state->src_mem, dma_tx_data)
        , .dest_ad = SPI_BUS_ADDR(fifo)
        , .tfr_len = SAMPLING_CHANNEL_COUNT * sizeof(uint32_t)
        , .next_cb = (uint32_t) mem_virt_to_phys(state->cb_mem, &cbs[12])
        };

    // Return the interface to the sampling module
    return (dma_sampling_buffers_t) {
        .buffer_a = state->buffer_a_mem.virt,
        .buffer_b = state->buffer_b_mem.virt,
        .buffer_a_status = buffer_a_status,
        .buffer_b_status = buffer_b_status,
        .buffer_a_clock_high = buffer_a_clock_high,
        .buffer_b_clock_high = buffer_b_clock_high
    };
}

/*
 * dma_sampling_init_hardware
 *
 * Initialize DMA sampling hardware.
 */
void dma_sampling_init_hardware(dma_sampling_state_t* state, int mailbox_fd) {
    //
    // Set GPIO pins for SPI0
    //

    gpio_setup(state->gpio_regs, SPI0_CE0_PIN,  GPIO_FSEL_ALT0, GPIO_NOPULL);
    gpio_setup(state->gpio_regs, SPI0_MISO_PIN, GPIO_FSEL_ALT0, GPIO_PULL_UP);
    gpio_setup(state->gpio_regs, SPI0_MOSI_PIN, GPIO_FSEL_ALT0, GPIO_NOPULL);
    gpio_setup(state->gpio_regs, SPI0_SCLK_PIN, GPIO_FSEL_ALT0, GPIO_NOPULL);


    //
    // Initialize SPI
    //

    state->spi_regs->cs = SPI_CLEAR_RX | SPI_CLEAR_TX;
    state->spi_regs->clk = SPI_CLOCK_DIVIDER;
    // Set SPI DMA priorities
    // (request only on empty for tx and 3 (data length) rx, panic on 8 for tx and rx)
    state->spi_regs->dc = (8<<24) | (3<<16) | (8<<8) | 0;


    //
    // Initialize PWM
    //

    // Stop the PWM controller
    pwm_stop(state->pwm_regs);
    usleep(100);
    // Clear bus error flag
    if(state->pwm_regs->sta & 0x100) {
        printf("PWM bus error\n");
        state->pwm_regs->sta = 0x100;
    }

    usleep(100);
    pwm_set_clock_freq(state->clock_regs, PWM_FREQUENCY);
    usleep(100);

    // Set the PWM range
    state->pwm_regs->ctl = (1 << 5) | (1 << 6);
    state->pwm_regs->rng1 = PWM_RANGE;
    state->pwm_regs->fif1 = PWM_RANGE / 2;
    // Enable DMAC on PWM with threshold of 1
    state->pwm_regs->dmac = (1 << 31) | (8 << 8) | (1 << 0);


    //
    // Enable DMA channels, checking that they are not in use
    //

    // Message to get DMA channel usage
    VC_MSG_BUFFER buf = {.tag={.id=0x00060001}};
    mailbox_send(mailbox_fd, &buf);
    // Assert that the selected DMA channel is not in use
    assert(buf.tag.buffer[0] & (1 << DMA_PACE_CHAN) && "DMA_PACE_CHAN is in use by the operating system");
    assert(buf.tag.buffer[0] & (1 << DMA_TX_CHAN)   && "DMA_TX_CHAN is in use by the operating system");
    assert(buf.tag.buffer[0] & (1 << DMA_RX_CHAN_A) && "DMA_RX_CHAN_A is in use by the operating system");
    assert(buf.tag.buffer[0] & (1 << DMA_RX_CHAN_B) && "DMA_RX_CHAN_B is in use by the operating system");

    // Enable the DMA channels
    dma_enable_chan(state->dma_regs, DMA_PACE_CHAN);
    dma_enable_chan(state->dma_regs, DMA_TX_CHAN);
    dma_enable_chan(state->dma_regs, DMA_RX_CHAN_A);
    dma_enable_chan(state->dma_regs, DMA_RX_CHAN_B);
}

/*
 * dma_sampling_start
 *
 * Start DMA sampling. This will begin filling the buffers returned in
 * dma_sampling_init.
 */
void dma_sampling_start(dma_sampling_state_t* state) {
    // Start DMA channels
    dma_start_chan(state->dma_regs, DMA_PACE_CHAN, state->cb_mem.phys);
    dma_start_chan(state->dma_regs, DMA_TX_CHAN,   state->cb_mem.phys + (12 * sizeof(DMA_CB)));
    dma_start_chan(state->dma_regs, DMA_RX_CHAN_A, state->cb_mem.phys + (4 * sizeof(DMA_CB)));
    dma_start_chan(state->dma_regs, DMA_RX_CHAN_B, state->cb_mem.phys + (8 * sizeof(DMA_CB)));

    // Sleep to allow DMA to initialize
    usleep(100);

    // Start the PWM controller for pacing
    state->pwm_regs->ctl |= 1;
}

/*
 * dma_sampling_stop
 *
 * Stop DMA sampling. Sampling can be started again with dma_sampling_start.
 */
void dma_sampling_stop(dma_sampling_state_t* state) {
    // Stop PWM
    pwm_stop(state->pwm_regs);

    // Stop the DMA transfer
    dma_stop_chan(state->dma_regs, DMA_PACE_CHAN);
    dma_stop_chan(state->dma_regs, DMA_TX_CHAN);
    dma_stop_chan(state->dma_regs, DMA_RX_CHAN_A);
    dma_stop_chan(state->dma_regs, DMA_RX_CHAN_B);
}
