#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <ck_ring.h>

#include "hardware/mailbox.h"
#include "hardware/system_timer.h"
#include "dma_sampling.h"
#include "socket_thread.h"
#include "writer_thread.h"
#include "offset_thread.h"

void signal_handler(int signal);

static atomic_bool should_run = true;

static const char* samples_fifo_name = "/var/sampling/samples.sock";
static const char* offset_fifo_name = "/var/sampling/offset.sock";

/*
 * BUFFER_QUEUE_SIZE
 *
 * Number of buffers to use in freelist. Must be a power of two greater than or
 * equal to 4.
 */
#define BUFFER_QUEUE_SIZE 16

static ck_ring_t free_buffer_queue;
static ck_ring_buffer_t free_buffer_queue_buffer[BUFFER_QUEUE_SIZE];
static ck_ring_t sample_buffer_queue;
static ck_ring_buffer_t sample_buffer_queue_buffer[BUFFER_QUEUE_SIZE];

int main() {
    signal(SIGINT, signal_handler);

    // Map memory for peripheral device access
    int fd = mailbox_open();

    //
    // Initialize the DMA sampler
    //

    dma_sampling_state_t sampling_state;
    dma_sampling_buffers_t sampling_buffers = dma_sampling_init(&sampling_state, fd);

    //
    // Set up sample buffer queues
    //

    ck_ring_init(&free_buffer_queue, BUFFER_QUEUE_SIZE);
    ck_ring_init(&sample_buffer_queue, BUFFER_QUEUE_SIZE);

    for(size_t i = 0; i < BUFFER_QUEUE_SIZE - 1; i++) {
        sample_buffer_t* container = malloc(sizeof(sample_buffer_t));

        if(!ck_ring_enqueue_spsc(&free_buffer_queue, free_buffer_queue_buffer, container)) {
            fprintf(stderr, "Error: failed to enqueue free sample\n");
            exit(-1);
        }
    }

    //
    // Create time offset socket thread
    //

    client_list_t* offset_client_list = malloc(sizeof(client_list_t));
    offset_client_list->head = NULL;
    offset_client_list->tail = NULL;

    if(pthread_mutex_init(&offset_client_list->lock, NULL) != 0) {
        perror("Error: mutex init failed");
        exit(-1);
    }

    socket_thread_args_t offset_socket_args = {
        .clients = offset_client_list,
        .socket_name = offset_fifo_name,
        .should_run = &should_run
    };

    pthread_t offset_socket_thread;
    pthread_create(&offset_socket_thread, NULL, socket_thread_function, (void*) &offset_socket_args);

    //
    // Create offset thread
    //

    offset_thread_args_t offset_args = {
        .clients = offset_client_list,
        .should_run = &should_run
    };

    pthread_t offset_thread;
    pthread_create(&offset_thread, NULL, offset_thread_function, (void*) &offset_args);

    //
    // Create time samples socket thread
    //

    client_list_t* samples_client_list = malloc(sizeof(client_list_t));
    samples_client_list->head = NULL;
    samples_client_list->tail = NULL;

    if(pthread_mutex_init(&samples_client_list->lock, NULL) != 0) {
        perror("Error: mutex init failed");
        exit(-1);
    }

    socket_thread_args_t samples_socket_args = {
        .clients = samples_client_list,
        .socket_name = samples_fifo_name,
        .should_run = &should_run
    };

    pthread_t samples_socket_thread;
    pthread_create(&samples_socket_thread, NULL, socket_thread_function, (void*) &samples_socket_args);

    //
    // Create writer thread
    //

    writer_thread_args_t writer_args = {
        .queue = &sample_buffer_queue,
        .queue_buffer = sample_buffer_queue_buffer,
        .free = &free_buffer_queue,
        .free_buffer = free_buffer_queue_buffer,
        .clients = samples_client_list,
        .should_run = &should_run
    };

    pthread_t writer_thread;
    pthread_create(&writer_thread, NULL, writer_thread_function, (void*) &writer_args);

    //
    // Start of main thread
    //

    dma_sampling_start(&sampling_state);

    //
    // Main loop
    //

    sample_buffer_t* buffer_obj;

    while(should_run) {
        //
        // Read buffer A
        //

        // Pull a sample buffer off the freelist
        if(!ck_ring_dequeue_spsc(&free_buffer_queue, free_buffer_queue_buffer, &buffer_obj)) {
            fprintf(stderr, "ERROR: Sample buffer freelist depleted!\n");
            assert(0);
        }

        // Wait for buffer A to be done
        while(!*sampling_buffers.buffer_a_status && should_run) usleep(100);
        if(!should_run) break;
        // Clear buffer status flag
        *sampling_buffers.buffer_a_status = 0;

        // Copy buffer A into free sample buffer container
        memcpy(buffer_obj->samples, (void*) sampling_buffers.buffer_a, BUFFER_LENGTH * sizeof(uint32_t));
        buffer_obj->clock_high = *sampling_buffers.buffer_a_clock_high;
        // Check that DMA finished this buffer.
        assert(!*sampling_buffers.buffer_a_status && !*sampling_buffers.buffer_b_status);
        // Push to the sample queue.
        if(!ck_ring_enqueue_spsc(&sample_buffer_queue, sample_buffer_queue_buffer, buffer_obj)) {
            fprintf(stderr, "ERROR: Sample buffer failed to insert!\n");
            assert(0);
        }

        //
        // Read buffer B
        //

        // Pull a sample buffer off the freelist
        if(!ck_ring_dequeue_spsc(&free_buffer_queue, free_buffer_queue_buffer, &buffer_obj)) {
            fprintf(stderr, "ERROR: Sample buffer freelist depleted!\n");
            assert(0);
        }

        // Wait for buffer B to be done
        while(!*sampling_buffers.buffer_b_status && should_run) usleep(100);
        if(!should_run) break;
        // Clear buffer status flag
        *sampling_buffers.buffer_b_status = 0;

        // Copy buffer B into free sample buffer container
        memcpy(buffer_obj->samples, (void*) sampling_buffers.buffer_b, BUFFER_LENGTH * sizeof(uint32_t));
        buffer_obj->clock_high = *sampling_buffers.buffer_b_clock_high;
        // Check that DMA finished this buffer.
        assert(!*sampling_buffers.buffer_a_status && !*sampling_buffers.buffer_b_status);
        // Push to the sample queue.
        if(!ck_ring_enqueue_spsc(&sample_buffer_queue, sample_buffer_queue_buffer, buffer_obj)) {
            fprintf(stderr, "ERROR: Sample buffer failed to insert!\n");
            assert(0);
        }
    }

    dma_sampling_stop(&sampling_state);

    // Join all threads
    pthread_join(offset_socket_thread, NULL);
    pthread_join(offset_thread, NULL);
    pthread_join(samples_socket_thread, NULL);
    pthread_join(writer_thread, NULL);

    //
    // Cleanup
    //

    pthread_mutex_destroy(&offset_client_list->lock);
    pthread_mutex_destroy(&samples_client_list->lock);

    void* cleanup_result;
    while(ck_ring_dequeue_spsc(&free_buffer_queue, free_buffer_queue_buffer, &cleanup_result)) {
        free((sample_buffer_t*) cleanup_result);
    }
    while(ck_ring_dequeue_spsc(&sample_buffer_queue, sample_buffer_queue_buffer, &cleanup_result)) {
        free((sample_buffer_t*) cleanup_result);
    }

    dma_sampling_cleanup(&sampling_state, fd);

    close(fd);
}

void signal_handler(int signal) {
    (void) signal;

    should_run = false;
    printf("Received SIGINT. Exiting...\n");
}
