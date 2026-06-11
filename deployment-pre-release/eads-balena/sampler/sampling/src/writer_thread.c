#include "writer_thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include <ck_ring.h>

#include "socket_thread.h"
#include "dma_sampling.h"
#include "sampling_adc.h"

void* writer_thread_function(void* arg) {
    writer_thread_args_t* args = (writer_thread_args_t*) arg;
    client_list_t* clients = args->clients;

    while(args->should_run) {
        // Pull a sample buffer off the queue
        sample_buffer_t* buffer_obj;
        if(!ck_ring_dequeue_spsc(args->queue, args->queue_buffer, &buffer_obj)) {
            // If the queue is empty then just sleep for a bit
            usleep(100);
            continue;
        }

        pthread_mutex_lock(&clients->lock);
        if(clients->head != NULL) {
            client_node_t* node = clients->head;

            while(node != NULL) {
                // If the node is currently on backoff then decrement and move on
                if(node->backoff_duration > 0) {
                    node->backoff_duration -= BUFFER_NUM_SAMPLES;
                    node = node->next;
                    continue;
                }

                int ret = print_buffer(node->sock, buffer_obj);
                if(ret < 0) {
                    if((ret == -2) || (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                        // Backoff by 1 buffer at first, doubling each time the application misbehaves
                        node->backoff_duration = node->backoff_rate * BUFFER_NUM_SAMPLES;
                        node->backoff_rate *= 2;

                        printf("Client %i is not keeping up. Dropping %i samples.\n", node->sock, node->backoff_duration);

                        // If the node didn't get its act together then drop it
                        if(node->backoff_rate > MAX_BACKOFF_RATE) {
                            client_list_remove(clients, node);
                            close(node->sock);
                            printf("Client %i failed to keep up. Connection closed.\n", node->sock);
                            client_node_t* old_node = node;
                            node = node->next;
                            free(old_node);
                        }
                    } else {
                        client_list_remove(clients, node);
                        close(node->sock);
                        printf("Connection closed with %i\n", node->sock);
                        client_node_t* old_node = node;
                        node = node->next;
                        free(old_node);
                    }
                } else {
                    node = node->next;
                }
            }
        }
        pthread_mutex_unlock(&clients->lock);

        // And return the buffer to the freelist
        if(!ck_ring_enqueue_spsc(args->free, args->free_buffer, buffer_obj)) {
            fprintf(stderr, "ERROR: Free buffer failed to insert!\n");
            assert(0);
        }
    }

    return NULL;
}

/*
 * print_buffer
 *
 * Print out the buffer to the socket in CSV format
 */
int print_buffer(int sock, sample_buffer_t* buffer) {
    // NOTE: See the definition of sample_buffer_t in dma_sampling.h for
    //       information on the layout of the buffer.

    // Each sample is around 24 characters long. We will round this up to 32
    // just to be safe
    char buf[32 * BUFFER_NUM_SAMPLES];
    size_t offset = 0;

    // If the last timestamp is less than the first then we can safely assume
    // that the lower 32 bits of the timer register has rolled over. Since the
    // higher 32 bits of the clock were sampled at the end of the set of
    // ADC samples, we need to decrement it when appending to any samples
    // before the timestamp rolls over.
    //
    // Keep this rollover path separate from the normal path.
    if(buffer->samples[BUFFER_NUM_SAMPLES] < buffer->samples[0]) {
        uint64_t clock_high = buffer->clock_high - 1;

        uint32_t prev_time = 0;

        // Print out the samples to the character buffer
        for(size_t i = 0; i < BUFFER_LENGTH; i += 2) {
            uint32_t time_low = buffer->samples[i];
            uint64_t time = time_low + (clock_high << 32);
            uint32_t channel = sampling_channels[(i / 2) % SAMPLING_CHANNEL_COUNT];
            uint32_t value = sampling_adc_decode_response(buffer->samples[i + 1]);

            offset += snprintf(&buf[offset], sizeof(buf) - offset, "%llu,%u,%u\n", time, channel, value);

            // Check for lower 32 bits clock rollover
            if((i > 0) && (time_low < prev_time)) {
                clock_high++;
            }
            prev_time = time_low;
        }
    } else {
        uint64_t clock_high = buffer->clock_high;

        // Print out the samples to the character buffer
        for(size_t i = 0; i < BUFFER_LENGTH; i += 2) {
            uint64_t time = buffer->samples[i] + (clock_high << 32);
            uint32_t channel = sampling_channels[(i / 2) % SAMPLING_CHANNEL_COUNT];
            uint32_t value = sampling_adc_decode_response(buffer->samples[i + 1]);

            offset += snprintf(&buf[offset], sizeof(buf) - offset, "%llu,%u,%u\n", time, channel, value);
        }
    }

    int ret = send(sock, buf, offset, MSG_NOSIGNAL | MSG_DONTWAIT);

    if(ret == -1) {
        return -1;
    }

    return ret == offset ? 0 : -2;
}
