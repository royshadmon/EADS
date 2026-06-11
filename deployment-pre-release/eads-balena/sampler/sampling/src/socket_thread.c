#include "socket_thread.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

void client_list_add(client_list_t* list, client_node_t* node) {
    if(list->head == NULL) {
        assert(list->tail == NULL);
        node->next = NULL;
        node->prev = NULL;

        list->head = node;
        list->tail = node;
    } else {
        assert(list->tail != NULL);
        
        node->next = NULL;
        node->prev = list->tail;

        list->tail->next = node;
        list->tail = node;
    }
}

void client_list_remove(client_list_t* list, client_node_t* node) {
    if(node == list->head) {
        if(node == list->tail) {
            list->head = NULL;
            list->tail = NULL;
        } else {
            list->head = node->next;
            node->next->prev = NULL;
        }
    } else {
        if(node == list->tail) {
            list->tail = node->prev;
            node->prev->next = NULL;
        } else {
            node->next->prev = node->prev;
            node->prev->next = node->next;
        }
    }
}

void* socket_thread_function(void* arg) {
    socket_thread_args_t* args = (socket_thread_args_t*) arg;
    client_list_t* clients = args->clients;

    int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if(server_sock < 0) {
        perror("Error: can't open socket");
        exit(-1);
    }

    struct sockaddr_un sock_addr;
    sock_addr.sun_family = AF_UNIX;
    memcpy(sock_addr.sun_path, args->socket_name, strlen(args->socket_name));
    unlink(args->socket_name);

    if(bind(server_sock, (struct sockaddr*) &sock_addr, sizeof(sock_addr)) < 0) {
        perror("Error: can't bind to socket");
        exit(-1);
    }

    if(listen(server_sock, 10) < 0) {
        perror("Error: can't listen on socket");
        exit(-1);
    }

    unsigned int len = sizeof(struct sockaddr_un);
    while(*args->should_run) {
        struct sockaddr_un client_sockaddr;
        int client_sock = accept(server_sock, (struct sockaddr*) &client_sockaddr, &len);

        if(client_sock < 0) {
            perror("Error: failed to accept");
            exit(-1);
        }

        pthread_mutex_lock(&clients->lock);
        printf("Connection established with %i\n", client_sock);

        client_node_t* node = malloc(sizeof(client_node_t));
        node->sock = client_sock;
        node->backoff_rate = 1;
        node->backoff_duration = 1;

        client_list_add(clients, node);
        pthread_mutex_unlock(&clients->lock);
    }

    // Close all client sockets
    pthread_mutex_lock(&clients->lock);
    if(clients->head != NULL) {
        assert(clients->tail != NULL);

        client_node_t* node = clients->head;
        if(clients->head != clients->tail) {
            clients->head = clients->head->next;
        }
        close(node->sock);
        free(node);
    }
    close(server_sock);
    pthread_mutex_unlock(&clients->lock);

    return NULL;
}
