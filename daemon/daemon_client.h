#ifndef NETATALK_CLIENT_DAEMON_DAEMON_CLIENT_H
#define NETATALK_CLIENT_DAEMON_DAEMON_CLIENT_H

#include <pthread.h>

#include "stateless_ipc.h"

#define AFP_CLIENT_INCOMING_BUF 8192

#define DAEMON_NUM_CLIENTS 32

struct daemon_client {
    char incoming_string[AFP_CLIENT_INCOMING_BUF];
    int incoming_size;
    char *a;

    char complete_packet[AFP_CLIENT_INCOMING_BUF];
    int completed_packet_size;
    pthread_mutex_t command_string_mutex;

    char outgoing_string[AFPSL_IPC_LOG_BUFFER_SIZE];
    size_t outgoing_string_len;
    int fd;
    int lock;
    char *shmem;
    int toremove;
    int pending;
    pthread_t processing_thread;
    pthread_mutex_t processing_mutex;
    int used;
};

int send_command(struct daemon_client *c, unsigned int len, const char *data);

int continue_client_connection(struct daemon_client * c);
int close_client_connection(struct daemon_client * c);
int remove_client(struct daemon_client ** toremove);
int count_active_clients(void);
void remove_all_clients(void);
int daemon_scan_extra_fds(int command_fd, fd_set *set, int *max_fd);

#endif
