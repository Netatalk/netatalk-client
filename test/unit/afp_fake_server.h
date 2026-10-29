#ifndef NETATALK_CLIENT_TEST_AFP_FAKE_SERVER_H_
#define NETATALK_CLIENT_TEST_AFP_FAKE_SERVER_H_

#include <pthread.h>
#include <stdint.h>

#include "lib/afp_internal.h"

/* The fork number of every open the server grants. */
#define AFP_FAKE_SERVER_FORK 42

/* The server end of a session for the unit tests (afp_fake_server.c). */
struct afp_fake_server {
    struct afp_server *server;
    struct afp_volume volume;
    int peer;
    pthread_t thread;
    uint8_t refuse;         /*!< command refused with kFPAccessDenied, or 0 */
    uint8_t inspect;        /*!< command whose request copies listed, or 0 */
    uint8_t read_only;      /*!< whether an open for writing is refused */
    uint8_t name_taken;     /*!< whether a soft create finds the name taken */
    const char *read_data;  /*!< a link's target, which FPReadExt reads whole */
    int inspected;          /*!< whether listed holds a copy */
    struct afp_file_info listed;
    uint8_t commands[16];   /*!< the first 16 commands received, in order */
    unsigned int ncommands;
    unsigned int forks_open;  /*!< forks granted and not closed */
};

int afp_fake_server_start(struct afp_fake_server *fake);
void afp_fake_server_stop(struct afp_fake_server *fake);
void afp_fake_server_dirty_stack(void);

#endif
