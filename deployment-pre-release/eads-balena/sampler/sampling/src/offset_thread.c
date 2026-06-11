#include "offset_thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <errno.h>

#include "hardware/system_timer.h"
#include "dma_sampling.h"

void* offset_thread_function(void* arg) {
    offset_thread_args_t* args = (offset_thread_args_t*) arg;
    client_list_t* clients = args->clients;

    SYSTEM_TIMER_REGS* system_timer_regs = system_timer_get_regs();

    // Store the previous offset to compute the drift
    uint64_t previous_time = 0;
    uint64_t previous_offset = 0;

    // TODO: This logic should be abstracted somewhere else
    while(args->should_run) {
        // Compute offset
        uint64_t system_timer_data[256];
        struct timeval clock_data[256];

        // Collect data, alternating between order
        for(size_t i = 0; i < 256; i += 2) {
            system_timer_data[i] = ((uint64_t) system_timer_regs->chi << 32) + system_timer_regs->clo;
            gettimeofday(&clock_data[i], NULL);
            usleep(100);

            gettimeofday(&clock_data[i + 1], NULL);
            system_timer_data[i + 1] = ((uint64_t) system_timer_regs->chi << 32) + system_timer_regs->clo;
            usleep(100);
        }

        // Compute the average of the offsets
        // NOTE: We are assuming that the offset is positive. This is true as
        //       long as the system was not started before Jan 1, 1970.
        uint64_t total = 0;
        for(size_t i = 0; i < 256; i++) {
            struct timeval time = clock_data[i];
            total += (uint64_t) (time.tv_sec * 1e6 + time.tv_usec) - system_timer_data[i];
        }
        uint64_t offset = total / 256;

        uint64_t end_time = (uint64_t) (clock_data[255].tv_sec * 1e6 + clock_data[255].tv_usec);
        uint64_t end_time_clock = system_timer_data[255];

        // Compute the skew since the previously computed offset in ppm
        int64_t skew = 0;
        if(previous_time != 0) {
            skew = (1e9 * ((int64_t) offset - (int64_t)previous_offset)) / ((int64_t)end_time - (int64_t)previous_time);
        }
        previous_time = end_time;
        previous_offset = offset;

        char string_buffer[256];
        int bytes = snprintf(string_buffer, 256, "%llu,%llu,%llu,%lli.%lli\n", end_time, end_time_clock, offset, skew / 1000, llabs(skew) % 1000);

        // Send offset to all clients
        pthread_mutex_lock(&clients->lock);
        if(clients->head != NULL) {
            client_node_t* node = clients->head;

            while(node != NULL) {
                // If the node is currently on backoff then decrement and move on
                if(node->backoff_duration > 0) {
                    node->backoff_duration--;
                    node = node->next;
                    continue;
                }

                int ret = send(node->sock, string_buffer, bytes, MSG_NOSIGNAL | MSG_DONTWAIT);
                if(ret < 0) {
                    if(errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Backoff by 125ms at first, doubling each time the application misbehaves
                        node->backoff_duration = node->backoff_rate;
                        node->backoff_rate *= 2;

                        printf("Client %i is not keeping up. Dropping %u offsets.\n", node->sock, node->backoff_duration);

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

        // Sleep for a while
        sleep(10);
    }

    return NULL;
}
