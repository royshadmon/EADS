#ifndef SOCKET_THREAD_H
#define SOCKET_THREAD_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/*
 * MAX_BACKOFF_RATE
 *
 * Max value of backoff_rate before client is disconnected.
 */
#define MAX_BACKOFF_RATE 16

struct client_node_s {
    int sock;

    int32_t backoff_duration;
    uint32_t backoff_rate;

    struct client_node_s* next;
    struct client_node_s* prev;
};
typedef struct client_node_s client_node_t;

typedef struct {
    client_node_t* head;
    client_node_t* tail;
    pthread_mutex_t lock;
} client_list_t;

void client_list_add(client_list_t* list, client_node_t* node);
void client_list_remove(client_list_t* list, client_node_t* node);

typedef struct {
    client_list_t* clients;
    const char* socket_name;
    volatile atomic_bool* should_run;
} socket_thread_args_t;

void* socket_thread_function(void* arg);

#endif // SOCKET_THREAD_H
