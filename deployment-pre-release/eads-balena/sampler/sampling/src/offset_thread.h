#ifndef OFFSET_THREAD_H
#define OFFSET_THREAD_H

#include "socket_thread.h"

typedef struct {
    client_list_t* clients;
    volatile atomic_bool* should_run;
} offset_thread_args_t;

void* offset_thread_function(void* arg);

#endif // OFFSET_THREAD_H
