#ifndef DMA_SAMPLING_H
#define DMA_SAMPLING_H

#include <stdint.h>

#include "hardware/virtual_memory.h"
#include "hardware/system_timer.h"
#include "hardware/clock.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/util.h"

/*
 *
 * Sampling settings
 *
 */

/*
 * SAMPLING_CHANNEL_COUNT
 *
 * Number of channels to be sampled. Channels can then be defined in
 * SAMPLING_CHANNELS.
 */
#define SAMPLING_CHANNEL_COUNT 2

/*
 * SAMPLING_CHANNELS
 *
 * ADC channels to be sampled by the DMA sampling module. Should have length
 * SAMPLING_CHANNEL_COUNT.
 */
#define SAMPLING_CHANNELS {0, 1}

/*
 * SAMPLING_FREQUENCY
 *
 * Frequency that samples will be collected. If multiple channels are defined
 * then each channel will be sampled at this frequency.
 */
#define SAMPLING_FREQUENCY 8000

/*
 * sampling_channels
 *
 * Array for accessing enabled sampling channels populated by SAMPLING_CHANNELS.
 * For more information on usage see dma_sampling_buffers_t.
 */
extern uint32_t sampling_channels[SAMPLING_CHANNEL_COUNT];


/*
 *
 * DMA channel settings
 *
 *
 * Some DMA channels are in use by the operating system and cannot be used.
 * This is checked at initialization of the module if any of the below
 * channels are being used then a failed assertion will be raised and the
 * channel must be changed.
 */

/*
 * DMA_PACE_CHAN
 *
 * DMA channel to use for pacing sampling using the PWM module.
 */
#define DMA_PACE_CHAN 7

/*
 * DMA_TX_CHAN
 *
 * DMA channel to use for sending data to the ADC over SPI.
 */
#define DMA_TX_CHAN   9

/*
 * DMA_RX_CHAN_A
 *
 * DMA channel to use for collecting data from ADC over SPI.
 *
 * This channel makes use of 2d mode which is only available on non-lite DMA
 * channels (0-6 are normal, 7-15 are lite).
 */
#define DMA_RX_CHAN_A 5

/*
 * DMA_RX_CHAN_B
 *
 * DMA channel to use for collecting timestamps each time a sample is received.
 *
 * This channel makes use of 2d mode which is only available on non-lite DMA
 * channels (0-6 are normal, 7-15 are lite).
 */
#define DMA_RX_CHAN_B 6


/*
 *
 * Buffer settings
 *
 */

/*
 * BUFFER_PAGES
 *
 * Number of pages allocated for each sample buffer. Each page will store 512
 * 32 bit samples and their associated 32 bit timestamps. Buffers are page
 * aligned so they can be allocated in uncached memory.
 *
 * Setting this value too low will cause the buffers to overrun faster. See
 * dma_sampling_buffers_t for more information on buffer overrun.
 *
 * Maximum is 256 so BUFFER_NUM_SAMPLES fits the 16-bit DMA register.
 */
#define BUFFER_PAGES 16

/*
 * BUFFER_LENGTH
 *
 * Length of each sample buffer. Determined by BUFFER_PAGES.
 */
#define BUFFER_LENGTH (BUFFER_PAGES * PAGE_SIZE / (2 * sizeof(uint32_t)))

/*
 * BUFFER_NUM_SAMPLES
 *
 * Number of sample-timestamp pairs stored in each buffer. Determined by
 * BUFFER_PAGES.
 */
#define BUFFER_NUM_SAMPLES (BUFFER_LENGTH / 2)


/*
 *
 * PWM clock settings
 *
 */

/*
 * PWM_FREQUENCY
 *
 * Frequency to use for the PWM clock in Hz. This should only be changed if the
 * SAMPLING_FREQUENCY is increased beyond 1 Mhz (which is not possible with the
 * MCP3208 ADC being used).
 */
#define PWM_FREQUENCY 1000000

/*
 * PWM_RANGE
 *
 * Number of PWM clock cycles between each sample. This will determine when the
 * PWM module creates DMA data requests which trigger SPI transfer.
 */
#define PWM_RANGE ((2 * PWM_FREQUENCY) / (SAMPLING_FREQUENCY * SAMPLING_CHANNEL_COUNT))


/*
 *
 * SPI clock settings
 *
 */

/*
 * SPI_FREQUENCY
 *
 * Frequency to use for the SPI clock in Hz. This should be increased if it is
 * observed that the 3 byte SPI packets being sent are too close together
 * for the ADC to handle. The MCP3208 datasheet provides maximum SPI clock
 * values.
 */
#define SPI_FREQUENCY 1000000

/*
 * SPI_CLOCK_DIVIDER
 *
 * SPI clock divider associated with the selected SPI_FREQUENCY.
 */
// TODO: Why does this have a factor of 1.6?
#define SPI_CLOCK_DIVIDER ((SPI_CLOCK_HZ / SPI_FREQUENCY) * 1.6)


/*
 *
 * SPI pin mapping
 *
 * This module only works with on SPI 0 because the auxiliary SPI drivers
 * (1 and 2) do not support DMA transfers.
 */

/*
 * SPI0_CE0_PIN
 *
 * SPI 0 chip enable 0 pin.
 */
#define SPI0_CE0_PIN  8

/*
 * SPI0_MISO_PIN
 *
 * SPI 0 master in slave out pin.
 */
#define SPI0_MISO_PIN 9

/*
 * SPI0_MISO_PIN
 *
 * SPI 0 master out slave in pin.
 */
#define SPI0_MOSI_PIN 10

/*
 * SPI0_MISO_PIN
 *
 * SPI 0 clock pin.
 */
#define SPI0_SCLK_PIN 11


/*
 *
 * Public Interface
 *
 */


/*
 * dma_sampling_buffers_t
 *
 * Struct holding pointers to the buffers which will be filled with samples.
 *
 * These buffers are filled as ping-pong buffers, with buffer A being filled
 * while buffer B is being read from and vice versa.
 *
 * Buffers contain both sample values as well as timestamps from the system
 * clock free running timer module. These are stored in a striped layout as
 * follows.
 *
 * ┌────────────────────────────────────────────────────────────────────────────────┐
 * │                      Ping-pong buffer internal layout                          │
 * ├────────┬───────────────┬───────────────────────────────────────────────────────┤
 * │ Offset │     Value     │                      Description                      │
 * ├────────┼───────────────┼───────────────────────────────────────────────────────┤
 * │ 0x00   │ Timestamp 0   │ Lower 32 bits of system clock free running timer (μs) │
 * │ 0x04   │ ADC Voltage 0 │ 12 bit raw ADC value (voltage = Vref * value / 4096)  │
 * │ 0x08   │ Timestamp 1   │ Lower 32 bits of system clock free running timer (μs) │
 * │ 0x0c   │ ADC Voltage 1 │ 12 bit raw ADC value (voltage = Vref * value / 4096)  │
 * │ ...    │ ...           │ ...                                                   │
 * └────────┴───────────────┴───────────────────────────────────────────────────────┘
 *
 * In the case that multiple channels are being used (defined by
 * SAMPLING_CHANNEL_COUNT and SAMPLING_CHANNELS), the channels will be sampled
 * in the order defined in SAMPLING_CHANNELS (accessible through the
 * sampling_channels array) and will be interleaved in the sample buffer.
 * The channel for a sample at index i can be found with
 * sampling_channels[i % SAMPLING_CHANNEL_COUNT].
 *
 *
 * Buffer A will be filled first, followed by buffer B. The buffer status will
 * be set to a non-zero value when the buffer is full and ready to be read
 * from. The reading logic should be implemented as follows.
 *
 * Buffer reading loop logic:
 *   1. Poll for buffer_a_status to be non-zero
 *   2. Set buffer_a_status to zero
 *   3. Read from buffer_a
 *   4. Check for buffer overrun
 *   5. Poll for buffer_b_status to be non-zero
 *   6. Set buffer_b_status to zero
 *   7. Read from buffer_b
 *   8. Check for buffer overrun
 *
 * A buffer overrun can be detected by checking if either of the status
 * registers are set following a read. In the case of reading buffer A, if the
 * buffer A status register is non-zero following a read, then the buffer has
 * been overwritten and the data has been corrupted. The program should throw
 * away any read data. Similarly, if the buffer B status register is non-zero
 * following a read of buffer A, then the DMA has finished writing to buffer B
 * and has begun writing to buffer A. The data in buffer A should be considered
 * corrupted and the program should throw any data away.
 *
 * Following a buffer overrun, the program can either panic or recovery can be
 * attempted, which will discard any data currently in the buffers. To recover
 * the program state, the buffers status registers should be set to zero and
 * the buffer reading logic can restart at the next filled buffer.
 *
 *
 * This struct additionally contains the high 32 bits of the system clock free
 * running timer collected at the end writing of each buffer. This can be used
 * to reconstruct the full 64 bit system clock for each sample. This value
 * should be read from the respective registers following steps 2 and 6 of the
 * buffer reading logic.
 *
 * In order to reconstruct the timestamps for samples, the following logic
 * should be implemented. If the last timestamp in the buffer is less than the
 * first timestamp then the bottom 32 bits of the system clock free running
 * timer has rolled over during this buffer sample period. In this case, the
 * index at which the timer rolled over should be detected (by checking that
 * timestamp[i + 1] < timestamp[i]) and the value buffer_*_clock_high - 1
 * should be appended to all timestamps up to and including i and the value
 * buffer_*_clock_high should be appended to all following timestamps.
 *
 * If a timer roll over has not occurred then the upper 32 bits can simply be
 * appended to the 32 bit timestamp values of each sample.
 *
 * Buffers contain BUFFER_NUM_SAMPLES samples and are of length
 * BUFFER_LENGTH * sizeof(uint32_t).
 */
typedef struct {
    volatile uint32_t* buffer_a;
    volatile uint32_t* buffer_b;

    volatile uint32_t* buffer_a_status;
    volatile uint32_t* buffer_b_status;

    volatile uint32_t* buffer_a_clock_high;
    volatile uint32_t* buffer_b_clock_high;
} dma_sampling_buffers_t;


/*
 * dma_sampling_state_t
 *
 * Struct holding the state of the DMA sampling module.
 *
 * Internal contents are not stable and should not be modified. For use in calls
 * to dma_sampling_*
 */
typedef struct {
    VC_MEM_VIRT_MEM cb_mem;
    VC_MEM_VIRT_MEM src_mem;
    VC_MEM_VIRT_MEM buffer_a_mem;
    VC_MEM_VIRT_MEM buffer_b_mem;

    CLOCK_REGS* clock_regs;
    GPIO_REGS* gpio_regs;
    SPI_REGS* spi_regs;
    PWM_REGS* pwm_regs;
    DMA_CHAN_REGS* dma_regs;
} dma_sampling_state_t;

/*
 * dma_sampling_init
 *
 * Initialize the DMA sampling module, returning a struct containing pointers
 * to the ping pong buffers and their status flags.
 */
dma_sampling_buffers_t dma_sampling_init(dma_sampling_state_t* state, int mailbox_fd);

/*
 * dma_sampling_cleanup
 *
 * Cleanup the DMA sampling module, freeing all mapped memory. The buffers
 * returned from dma_sampling_init are no longer valid after this call.
 */
void dma_sampling_cleanup(dma_sampling_state_t* state, int mailbox_fd);

/*
 * dma_sampling_init_memory
 *
 * Initialize DMA sampling memory.
 */
dma_sampling_buffers_t dma_sampling_init_memory(dma_sampling_state_t* state, int mailbox_fd);

/*
 * dma_sampling_init_hardware
 *
 * Initialize DMA sampling hardware.
 */
void dma_sampling_init_hardware(dma_sampling_state_t* state, int mailbox_fd);

/*
 * dma_sampling_start
 *
 * Start DMA sampling. This will begin filling the buffers returned in
 * dma_sampling_init.
 */
void dma_sampling_start(dma_sampling_state_t* state);

/*
 * dma_sampling_stop
 *
 * Stop DMA sampling. Sampling can be started again with dma_sampling_start.
 */
void dma_sampling_stop(dma_sampling_state_t* state);

#endif // DMA_SAMPLING_H
