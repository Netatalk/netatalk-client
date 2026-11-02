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
    uint8_t refuse;         /*!< command refused, or 0 */
    uint8_t refuse_privs;   /*!< whether a privileges change is refused */
    int32_t refusal;        /*!< their answer, kFPAccessDenied when 0 */
    uint8_t inspect;        /*!< command whose request copies listed, or 0 */
    uint8_t read_only;      /*!< whether to answer as a read-only filesystem */
    uint8_t name_taken;     /*!< whether a soft create finds the name taken */
    uint8_t file_params;    /*!< whether FPGetFileDirParms answers an object */
    uint8_t params_dir;     /*!< whether it is a directory, not a file */
    uint32_t params_mode;   /*!< its mode, or 0 for 0644 and 0755 */
    uint32_t params_date;   /*!< its modification date, in network order */
    uint32_t data_length;   /*!< that file's data fork length */
    uint32_t resource_length; /*!< and its resource fork length */
    uint16_t set_bitmap;    /*!< the bitmap of the last FPSetFileDirParms */
    uint32_t set_uid;       /*!< the owner the last privileges set carried */
    uint32_t set_gid;       /*!< and the group */
    /*! whether a comment keeps every byte, not afpd's 199 kept and 128 read */
    uint8_t whole_comment;
    char comment[255];      /*!< the Finder comment */
    uint8_t comment_len;    /*!< its length, 0 for none */
    const char *read_data;  /*!< a link's target, which FPReadExt reads whole */
    int inspected;          /*!< whether listed holds a copy */
    struct afp_file_info listed;
    uint8_t commands[16];   /*!< the first 16 commands received, in order */
    uint32_t lengths[16];   /*!< the length of each of those requests */
    unsigned int ncommands;
    unsigned int forks_open;  /*!< forks granted and not closed */
};

int afp_fake_server_start(struct afp_fake_server *fake);
void afp_fake_server_stop(struct afp_fake_server *fake);
void afp_fake_server_dirty_stack(void);

#endif
