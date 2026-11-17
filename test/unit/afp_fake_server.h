/*!
 * @file
 * @brief A fake AFP server for the unit tests: its knobs and its calls
 */

#ifndef NETATALK_CLIENT_TEST_AFP_FAKE_SERVER_H_
#define NETATALK_CLIENT_TEST_AFP_FAKE_SERVER_H_

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>

#include "lib/afp_internal.h"

/* The fork number of every open the server grants. */
#define AFP_FAKE_SERVER_FORK 42
/* The signature of every status reply. */
#define AFP_FAKE_SERVER_SIGNATURE "0123456789abcdef"
/* The session token every login is given. */
#define AFP_FAKE_SERVER_TOKEN "TOKEN123"
/* The most connections a listening server serves. */
#define AFP_FAKE_SERVER_MAX_CONNECTIONS 32
/* Busy as afpd at its cap: the second connection refused unasked, */
#define AFP_FAKE_SERVER_BUSY_AT_ACCEPT 1
/* or the first DSIOpenSession refused in its answer, */
#define AFP_FAKE_SERVER_BUSY_AT_OPEN 2
/* or every connection's first request, the status's among them. */
#define AFP_FAKE_SERVER_BUSY_ALWAYS 3

/* The server end of a session: a socketpair or a loopback listener. */
struct afp_fake_server {
    struct afp_server *server;
    struct afp_volume volume;
    int peer;
    pthread_t thread;
    uint8_t refuse;         /*!< command refused, or 0 */
    /*! its requests answered before the only one refused; 0 refuses all */
    uint8_t refuse_skip;
    unsigned int refuse_seen; /*!< its requests so far */
    uint8_t refuse_privs;   /*!< whether a privileges change is refused */
    uint8_t refuse_unlock;  /*!< whether a byte-range unlock is refused */
    int32_t refusal;        /*!< their answer, kFPAccessDenied when 0 */
    uint8_t inspect;        /*!< command whose request copies listed, or 0 */
    uint8_t hold;           /*!< command held until released, or 0 */
    uint8_t hold_skip;      /*!< its requests answered before the one held */
    int held;               /*!< whether that request has arrived */
    int released;           /*!< whether the test let its answer go */
    pthread_mutex_t hold_mutex;
    pthread_cond_t hold_cond;
    uint8_t read_only;      /*!< whether to answer as a read-only filesystem */
    uint8_t name_taken;     /*!< whether a soft create finds the name taken */
    uint8_t file_params;    /*!< whether FPGetFileDirParms answers an object */
    uint8_t params_dir;     /*!< whether it is a directory, not a file */
    /*! the answers that are directories too, bit 0 for the first */
    uint16_t params_dirs;
    /*! the answers whose file has empty forks, bit 0 for the first */
    uint16_t params_empty;
    unsigned int params_answers; /*!< FPGetFileDirParms answered so far */
    uint32_t params_cnid;   /*!< its node ID, or 0 for none in the answer */
    uint32_t params_mode;   /*!< its mode, or 0 for 0644 and 0755 */
    uint32_t params_access; /*!< the user's rights, or 0 for ownership alone */
    uint32_t params_date;   /*!< its modification date, in network order */
    uint32_t data_length;   /*!< that file's data fork length */
    uint32_t resource_length; /*!< and its resource fork length */
    /*! the bitmap of the last FPSetFileDirParms or FPSetDirParms */
    uint16_t set_bitmap;
    uint16_t set_bitmaps;   /*!< every bit those sets carried */
    uint32_t set_uid;       /*!< the owner the last set of ids carried */
    uint32_t set_gid;       /*!< and the group */
    uint8_t echo_ids;       /*!< whether a read answers those ids once set */
    /*! whether a comment keeps every byte, not AFP's 199 and afpd's 128 read */
    uint8_t whole_comment;
    char comment[255];      /*!< the Finder comment */
    uint8_t comment_len;    /*!< its length, 0 for none */
    const char *read_data;  /*!< a link's target, which FPReadExt reads whole */
    uint8_t read_file;      /*!< whether FPReadExt reads data_length zeros */
    const char *xattr_value; /*!< FPGetExtAttr's value, or NULL */
    int inspected;          /*!< whether listed holds a copy */
    struct afp_file_info listed;
    uint8_t commands[32];   /*!< the first 32 commands received, in order */
    uint32_t lengths[32];   /*!< the length of each of those requests */
    unsigned char requests[32][64]; /*!< the first 64 bytes of each */
    unsigned int ncommands;
    unsigned int forks_open;  /*!< forks granted and not closed */
    uint16_t status_flags;  /*!< the flags of the status reply */
    const char *machine_type; /*!< the type reported, or Netatalk4.2.3 */
    const char *server_name;  /*!< the Mac Roman name, or "fake" */
    uint8_t bad_status;     /*!< whether the status points past its end */
    uint8_t afpd_offsets;   /*!< afpd's four offsets, whatever the type */
    int hold_status_ms;     /*!< ms the status waits for a 2nd connection */
    unsigned int status_sent_after; /*!< connections when the status went */
    uint8_t busy;           /*!< how a session is refused as busy, or 0 */
    unsigned int refused;   /*!< connections refused as busy */
    int listener;           /*!< the listening socket, or -1 */
    struct sockaddr_in listen_addr;
    pthread_t accept_thread;
    pthread_mutex_t lock;   /*!< guards the commands and the connections */
    int stopping;           /*!< whether the listener stops accepting */
    int connections[AFP_FAKE_SERVER_MAX_CONNECTIONS];
    pthread_t connection_threads[AFP_FAKE_SERVER_MAX_CONNECTIONS];
    unsigned int accepted;  /*!< the connections accepted */
    uint8_t replay_cache;   /*!< whether sessions announce afpd's replay cache */
    unsigned int repeated_ids; /*!< requests whose ID came again on one
                                    connection, or in the session with a
                                    replay cache */
    /*! a bit for each request ID the session has carried, replay cache on */
    unsigned char session_ids[65536 / 8];
};

int afp_fake_server_start(struct afp_fake_server *fake);
void afp_fake_server_stop(struct afp_fake_server *fake);
int afp_fake_server_wait_held(struct afp_fake_server *fake);
void afp_fake_server_release(struct afp_fake_server *fake);
unsigned int afp_fake_server_open_access(const struct afp_fake_server *fake);
int afp_fake_server_listen(struct afp_fake_server *fake);
void afp_fake_server_drop(struct afp_fake_server *fake);
void afp_fake_server_stop_listening(struct afp_fake_server *fake);
unsigned int afp_fake_server_count(struct afp_fake_server *fake,
                                   uint8_t command);
void afp_fake_server_dirty_stack(void);

#endif
