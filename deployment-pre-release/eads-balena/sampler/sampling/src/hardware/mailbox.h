#ifndef MAILBOX_H
#define MAILBOX_H

/*
 * This file contains the interface to the VideoCore mailbox.
 *
 * Documentation is available on raspberrypi firmware GitHub:
 * https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 *
 */

#include <stdint.h>

/*
 *
 * Mailbox message definitions
 *
 */

#define VC_MSG_CODE_REQUEST 0x00000000
#define VC_MSG_CODE_SUCCESS 0x80000000
#define VC_MSG_CODE_PARTIAL 0x80000001

typedef struct {
    uint32_t id;          // Tag identifier
    uint32_t buffer_size; // Size of value buffer in bytes
    uint32_t code;        // Request and response codes
    volatile uint32_t buffer[4];   // Value buffer
} VC_MSG_TAG;

typedef struct {
    uint32_t buffer_size; // Overall buffer size including headers
    uint32_t code;        // Request and response codes
    VC_MSG_TAG tag;       // Tag
    uint32_t end_tag;     // Must be zero
} VC_MSG_BUFFER __attribute__ ((aligned (16)));

/*
 * mailbox_open
 *
 * Open the VC mailbox, returning the file handle.
 */
int mailbox_open(void);

/*
 * mailbox_send
 *
 * Send message to the mailbox with the given file descriptor.
 *
 * VC response will be written in to the message tag buffer.
 */
void mailbox_send(int fd, VC_MSG_BUFFER *msgp);



/*
 *
 * Mailbox memory allocation and locking routines
 *
 */

#define VC_MEM_SUCCESS 0

typedef uint32_t VC_MEM_HANDLE;
typedef uint32_t VC_MEM_STATUS;

/*
 * Videocore mailbox memory allocation flags, see:
 * https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 */
typedef enum {
    MEM_FLAG_DISCARDABLE    = 1<<0, // can be resized to 0 at any time. Use for cached data
    MEM_FLAG_NORMAL         = 0<<2, // normal allocating alias. Don't use from ARM
    MEM_FLAG_DIRECT         = 1<<2, // 0xC alias uncached
    MEM_FLAG_COHERENT       = 2<<2, // 0x8 alias. Non-allocating in L2 but coherent
    MEM_FLAG_ZERO           = 1<<4, // initialise buffer to all zeros
    MEM_FLAG_NO_INIT        = 1<<5, // don't initialise (default is initialise to all ones)
    MEM_FLAG_HINT_PERMALOCK = 1<<6, // Likely to be locked for long periods of time
    MEM_FLAG_L1_NONALLOCATING=(MEM_FLAG_DIRECT | MEM_FLAG_COHERENT) // Allocating in L2
} VC_ALLOC_FLAGS;

/*
 * mailbox_mem_alloc
 *
 * Allocate memory of with given size and alignment in bytes. Flags are defined
 * in https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface.
 *
 * Returns memory handle which can be passed in to mailbox_mem_lock.
 */
VC_MEM_HANDLE mailbox_mem_alloc(int fd, uint32_t size, uint32_t alignment, VC_ALLOC_FLAGS flags);

/*
 * mailbox_mem_lock
 *
 * Lock memory allocated with mailbox_mem_alloc, returning the bus address of
 * the memory.
 */
void* mailbox_mem_lock(int fd, VC_MEM_HANDLE h);

/*
 * mailbox_mem_unlock
 *
 * Unlock memory locked with mailbox_mem_lock, returning the VC_MEM_SUCCESS
 * on success.
 */
VC_MEM_STATUS mailbox_mem_unlock(int fd, VC_MEM_HANDLE h);

/*
 * mailbox_mem_free
 *
 * Free memory allocated with mailbox_mem_alloc, returning the VC_MEM_SUCCESS
 * on success.
 */
VC_MEM_STATUS mailbox_mem_free(int fd, VC_MEM_HANDLE h);

/*
 * Videocore mailbox power device IDs
 * See: https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 */
typedef enum {
    POWER_SD_CARD = 0,
    POWER_UART0   = 1,
    POWER_UARD1   = 2,
    POWER_USB_HCD = 3,
    POWER_I2C0    = 4,
    POWER_I2C1    = 5,
    POWER_I2C2    = 6,
    POWER_SPI     = 7,
    POWER_CCP2TX  = 8
} VC_POWER_ID;

/*
 * mailbox_power_get_state
 *
 * Get the current power state of the device. The return value has the
 * following flags:
 *
 * Bit 0: 0=off, 1=on
 * Bit 1: 0=device exists, 1=device does not exist
 */
uint32_t mailbox_power_get_state(int fd, VC_POWER_ID id);

/*
 * mailbox_power_set_state
 *
 * Set the power state of the device. The state has the following flags:
 *
 * Bit 0: 0=off, 1=on
 * Bit 1: 0=do not wait, 1=device does not exist
 * 
 *
 * The return value has the following flags:
 *
 * Bit 0: 0=off, 1=on
 * Bit 1: 0=device exists, 1=device does not exist
 */
uint32_t mailbox_power_set_state(int fd, VC_POWER_ID id, uint32_t state);

#endif // MAILBOX_H
