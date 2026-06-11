#ifndef WRITER_THREAD_H
#define WRITER_THREAD_H

#include <stdint.h>

#include <ck_ring.h>

#include "dma_sampling.h"
#include "socket_thread.h"

//
// Sample buffer container
//

typedef struct {
    uint32_t samples[BUFFER_LENGTH];
    uint32_t clock_high;
} sample_buffer_t;

typedef struct {
    ck_ring_t* queue;
    ck_ring_buffer_t* queue_buffer;
    ck_ring_t* free;
    ck_ring_buffer_t* free_buffer;
    client_list_t* clients;
    volatile atomic_bool* should_run;
} writer_thread_args_t;

void* writer_thread_function(void* arg);

/*
 * print_buffer
 *
 * Print out the buffer to the socket in CSV format
 */
int print_buffer(int socket, sample_buffer_t* buffer);

#endif // WRITER_THREAD_H
