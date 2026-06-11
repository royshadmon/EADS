#include "mailbox.h"

/*
 * This file contains the interface to the VideoCore mailbox.
 *
 * Documentation is available on raspberrypi firmware GitHub:
 * https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 *
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <assert.h>

/*
 * mailbox_open
 *
 * Open the VC mailbox, returning the file handle.
 */
int mailbox_open(void) {
    int fd = open("/dev/vcio", 0);

    if (fd < 0) {
        perror("Error: can't open VC mailbox");
        exit(-1);
    }

    return fd;
}

/*
 * mailbox_send
 *
 * Send message to the mailbox with the given file descriptor.
 *
 * VC response will be written in to the message tag buffer.
 */
void mailbox_send(int fd, VC_MSG_BUFFER *msgp) {
    // Set required fields on buffer
    msgp->buffer_size = sizeof(VC_MSG_BUFFER);
    msgp->code = VC_MSG_CODE_REQUEST;
    msgp->end_tag = 0;
    msgp->tag.buffer_size = sizeof(msgp->tag.buffer);
    msgp->tag.code = VC_MSG_CODE_REQUEST;

    if (ioctl(fd, _IOWR(100 /* ioctl magic number for VC */, 0, msgp), msgp) < 0) {
        perror("Error: VC ioctl failed");
        exit(-1);
    }

    if ((msgp->code & VC_MSG_CODE_SUCCESS) == 0) {
        perror("Error: VC ioctl error (0x80000000)");
        exit(-1);
    }
    if (msgp->code == VC_MSG_CODE_PARTIAL) {
        perror("Error: VC ioctl partial error (0x80000001)");
        exit(-1);
    }
}

/*
 * mailbox_mem_alloc
 *
 * Allocate memory of with given size and alignment in bytes. Flags are defined
 * in https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface.
 *
 * Returns memory handle which can be passed in to mailbox_mem_lock.
 */
VC_MEM_HANDLE mailbox_mem_alloc(int fd, uint32_t size, uint32_t alignment, VC_ALLOC_FLAGS flags) {
    VC_MSG_BUFFER msg = {.tag={.id=0x0003000c, .buffer={size, alignment, flags}}};

    mailbox_send(fd, &msg);

    // Return the handle from the VC
    return msg.tag.buffer[0];
}

/*
 * mailbox_mem_lock
 *
 * Lock memory allocated with mailbox_mem_alloc, returning the bus address of
 * the memory.
 */
void* mailbox_mem_lock(int fd, VC_MEM_HANDLE h) {
    assert(h);

    VC_MSG_BUFFER msg = {.tag={.id=0x0003000d, .buffer={h}}};

    mailbox_send(fd, &msg);

    // Return the bus address from the VC
    return (void*) msg.tag.buffer[0];
}

/*
 * mailbox_mem_unlock
 *
 * Unlock memory locked with mailbox_mem_lock, returning the VC_MEM_SUCCESS
 * on success.
 */
VC_MEM_STATUS mailbox_mem_unlock(int fd, VC_MEM_HANDLE h) {
    assert(h);

    VC_MSG_BUFFER msg = {.tag={.id=0x0003000e, .buffer={h}}};

    mailbox_send(fd, &msg);

    // Return the status from the VC
    return (VC_MEM_STATUS) msg.tag.buffer[0];
}

/*
 * mailbox_mem_free
 *
 * Free memory allocated with mailbox_mem_alloc, returning the VC_MEM_SUCCESS
 * on success.
 */
VC_MEM_STATUS mailbox_mem_free(int fd, VC_MEM_HANDLE h) {
    assert(h);

    VC_MSG_BUFFER msg = {.tag={.id=0x0003000f, .buffer={h}}};

    mailbox_send(fd, &msg);

    // Return the status from the VC
    return (VC_MEM_STATUS) msg.tag.buffer[0];
}

/*
 * mailbox_power_get_state
 *
 * Get the current power state of the device. The return value has the
 * following flags:
 *
 * Bit 0: 0=off, 1=on
 * Bit 1: 0=device exists, 1=device does not exist
 */
uint32_t mailbox_power_get_state(int fd, VC_POWER_ID id) {
    VC_MSG_BUFFER msg = {.tag={.id=0x00020001, .buffer={id}}};

    mailbox_send(fd, &msg);

    // Return the status from the VC
    return (VC_MEM_STATUS) msg.tag.buffer[1];
}

/*
 * mailbox_power_set_state
 *
 * Set the power state of the device. The state has the following flags:
 *
 * Bit 0: 0=off, 1=on
 * Bit 1: 0=do not wait, 1=wait
 * 
 *
 * The return value has the following flags:
 *
 * Bit 0: 0=off, 1=on
 * Bit 1: 0=device exists, 1=device does not exist
 */
uint32_t mailbox_power_set_state(int fd, VC_POWER_ID id, uint32_t state) {
    VC_MSG_BUFFER msg = {.tag={.id=0x00028001, .buffer={id, state}}};

    mailbox_send(fd, &msg);

    // Return the status from the VC
    return (VC_MEM_STATUS) msg.tag.buffer[1];
}
