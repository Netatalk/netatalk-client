/*!
 * @file
 * @brief DSI requests and reply waits of a session
 */

#ifndef AFPCLIENT_DSI_H
#define AFPCLIENT_DSI_H

#include "afp_internal.h"

typedef int (*afpc_dsi_reply_handler)(struct afp_server *server, char *buffer,
                                      unsigned int size, void *context);

struct dsi_request {
    unsigned short requestid;
    unsigned char subcommand;
    void *other;
    int wait;
    struct timespec deadline;
    int done_waiting;
    int ignore_replies;
    /*! whether the receiver holds the request, which its sender then keeps */
    int receiving;
    pthread_cond_t waiting_cond;
    pthread_mutex_t waiting_mutex;
    struct dsi_request *next;
    int return_code;
    unsigned int connection_generation;
    afpc_dsi_reply_handler reply_handler;
    struct afp_rx_buffer *stream_buffer;
};

int afpc_dsi_receive(struct afp_server *server, void *data, int size);
int afpc_dsi_getstatus(struct afp_server *server);
int afpc_dsi_sendtickle(struct afp_server *server);
void afpc_dsi_end_sends(struct afp_server *server, int fd);
void afpc_dsi_flush_request_queue(struct afp_server *server);
void afpc_dsi_fail_request_queue(struct afp_server *server, int error);

int afpc_dsi_opensession(struct afp_server *server);

int afpc_dsi_send(struct afp_server *server, char *msg, int size, int wait,
                  unsigned char subcommand, void *other);
int afpc_dsi_transfer_wait(const struct afp_server *server, uint64_t bytes,
                           unsigned int bytes_per_second);
void afpc_dsi_set_deadline(struct afp_server *server,
                           struct dsi_request *request);
int afpc_dsi_send_with_reply(struct afp_server *server, char *msg, int size,
                             int wait, unsigned char subcommand, void *other,
                             afpc_dsi_reply_handler reply_handler,
                             struct afp_rx_buffer *stream_buffer);
struct dsi_session *afpc_dsi_create(struct afp_server *server);
int afpc_dsi_restart(struct afp_server *server);
int afpc_dsi_recv(struct afp_server *server);
void afpc_dsi_incoming_closesession(struct afp_server *server);

#define DSI_BLOCK_TIMEOUT -1
#define DSI_DONT_WAIT 0
#define DSI_DEFAULT_TIMEOUT 5
/* a spun down time capsule can take up to 20 secs to
 * wake up and reply to a mount request */
#define DSI_OPENVOLUME_TIMEOUT 20
#define DSI_LOGIN_TIMEOUT 20
#define DSI_TIMECAPSULE_DEFAULT_TIMEOUT 30
/* The most a reply's wait grows with its bytes, far inside a 32-bit time_t */
#define DSI_MAX_TRANSFER_WAIT (7 * 24 * 3600)

#define GETSTATUS_BUF_SIZE 1024

#endif
