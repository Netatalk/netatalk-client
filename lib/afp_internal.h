/*!
 * @file
 * @brief libafpclient's internal servers, volumes, forks, and their calls
 */

#ifndef AFPCLIENT_INTERNAL_H
#define AFPCLIENT_INTERNAL_H

#include <arpa/inet.h>
#include <stdint.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/statvfs.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <time.h>

#include "netatalk-client/types.h"
#include "netatalk-client/url.h"
#include "afp_protocol.h"
#include "compat.h"
#include "client.h"
#include "xattr.h"

/* This is the maximum AFP version this library supports */
#define AFP_MAX_SUPPORTED_VERSION 34

struct afp_token {
    unsigned int length;
    char data[AFP_TOKEN_MAX_LEN];
};

#define SERVER_MAX_VERSIONS 10
#define SERVER_MAX_UAMS 10

struct afp_rx_buffer {
    unsigned int size;
    unsigned int maxsize;
    char *data;
    int errorcode;
};


#define AFP_NS_PER_SEC INT64_C(1000000000)

/*! @brief A time in seconds as the record's nanoseconds, saturated */
static inline int64_t afp_ns_from_sec(time_t sec)
{
    /* Widened, so a 32-bit time_t meets the bounds without a warning */
    int64_t s = sec;

    if (s > INT64_MAX / AFP_NS_PER_SEC) {
        return INT64_MAX;
    }

    if (s < INT64_MIN / AFP_NS_PER_SEC) {
        return INT64_MIN;
    }

    return s * AFP_NS_PER_SEC;
}

/*! @brief The record's nanoseconds as whole seconds, toward zero */
static inline time_t afp_sec_from_ns(int64_t ns)
{
    return (time_t)(ns / AFP_NS_PER_SEC);
}

/*! @brief One file's or directory's parameters, as a reply carries them */
struct afp_dirent {
    uint64_t size;
    uint64_t resourcesize;
    int64_t create_ns;      /*!< nanoseconds since 1970 */
    int64_t mod_ns;         /*!< nanoseconds since 1970 */
    uint32_t valid;         /*!< members carried, as isdir's bitmap bits */
    uint32_t cnid;
    uint32_t parent_did;
    struct afpc_unix_privileges unixprivs;
    uint32_t name_off;      /*!< the name's offset in the listing's pool */
    uint16_t attributes;
    uint16_t offspring;
    uint8_t isdir;
    uint8_t kind;           /*!< AFP_META_NONE, or a pseudo-file's kind */
    char finderinfo[32];
};

/* Ordered by size: 104 bytes on LP64 */
_Static_assert(sizeof(struct afp_dirent) <= 104, "afp_dirent packs");

/*! @brief A directory listing; a zeroed one is empty */
struct afp_listing {
    struct afp_dirent *entries;
    uint32_t count;
    uint32_t capacity;
    char *pool;
    uint32_t pool_used;
    uint32_t pool_cap;
};

/*! @brief An open fork: the file it names and the open's client state */
struct afp_file_info {
    struct afp_file_info *largelist_next;
    unsigned long long size;
    unsigned int did;
    /*! The node ID from the open or AFP 2's query, 0 without one */
    unsigned int cnid;
    unsigned int resource;
    unsigned short forkid;
    unsigned char sync;
    unsigned char writable;
    unsigned char dirty;
    /*! This record's open created the file */
    unsigned char created;
    /*! The file's name, the directory cache's key */
    char name[AFPC_MAX_NAME_BYTES];
    /*! The file's name in the volume's wire form */
    char basename[AFPC_MAX_NAME_BYTES];
};

struct dir_cache_entry;

/* The directory cache's counters: the lookups', counted with atomics, and
 * the changes', counted under the write lock */
struct dir_cache_counts {
    unsigned long hits;
    unsigned long misses;
    unsigned long negative_hits;
    unsigned long stat_hits;
    unsigned long fetches;
    unsigned long inserts;
    unsigned long evictions;
    unsigned long invalidations;
};

/* The slots of the changes the directory cache finds by node ID */
#define DIR_CACHE_CNID_SLOTS 64U

/* The directory cache (did.c): the identity, the parameters and the
 * absence of the volume's objects, keyed by their parent directory's ID
 * and their name, and indexed by node ID and by parent. */
struct dir_cache {
    pthread_rwlock_t lock;
    struct dir_cache_entry **by_name;
    struct dir_cache_entry **by_cnid;
    struct dir_cache_entry **by_parent;
    uint32_t buckets;
    uint32_t count;
    uint32_t max_entries;
    uint32_t clock_hand;
    /* The listings begun so far, under the write lock: each one's serial */
    uint64_t listings;
    /* The changes counted so far, under the write lock: each removal and
     * each invalidation takes the next number */
    uint64_t changes;
    /* The change at which a name last stopped denoting its object: a
     * request sent before it may describe an object the name no longer
     * holds */
    uint64_t last_removal;
    /* The latest change a dropped entry carried, which a new entry
     * starts from */
    uint64_t dropped_change;
    /* The latest change found by node ID, in a slot per node ID's low
     * bits, which a reply for the object under any name must postdate for
     * its parameters to be kept */
    uint64_t cnid_changes[DIR_CACHE_CNID_SLOTS];
    /* The clock of the lives, CLOCK_BOOTTIME's seconds where the system
     * has it and CLOCK_MONOTONIC's elsewhere, unless a test sets its own */
    time_t (*clock)(void);
    /* The wall clock an object's age is measured on, time(2) unless a
     * test sets its own */
    time_t (*wall)(void);
    struct dir_cache_counts stats;
};

#if 0
/* obsolete netatalk workaround flags */
#define VOLUME_EXTRA_FLAGS_VOL_CHMOD_KNOWN 0x1
#define VOLUME_EXTRA_FLAGS_VOL_CHMOD_BROKEN 0x2
#endif
#define VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE 0x4
#define VOLUME_EXTRA_FLAGS_VOL_SUPPORTS_UNIX 0x8
#define VOLUME_EXTRA_FLAGS_NO_LOCKING 0x10
#define VOLUME_EXTRA_FLAGS_IGNORE_UNIXPRIVS 0x20
#define VOLUME_EXTRA_FLAGS_READONLY 0x40
#define VOLUME_EXTRA_FLAGS_FLUSH_ON_CLOSE 0x80

#define AFP_VOLUME_UNMOUNTED 0
#define AFP_VOLUME_MOUNTED 1
#define AFP_VOLUME_UNMOUNTING 2

#define AFP_VOLUME_DETACHED    0
#define AFP_VOLUME_ATTACHED    1
#define AFP_VOLUME_DETACHING   2

struct afp_volume {
    unsigned short volid;
    char flags;  /* This is from afpGetSrvrParms */
    unsigned short attributes; /* This is from VolOpen */
    unsigned short signature;  /* This is fixed or variable */
    time_t creation_date;
    time_t modification_date;
    time_t backup_date;
    struct statvfs stat;
    unsigned char mounted;
    unsigned char attached;
    char mountpoint[AFP_MOUNTPOINT_LEN];
    struct afp_server *server;
    char volume_name[AFP_VOLUME_NAME_UTF8_LEN];
    char volume_name_printable[AFP_VOLUME_NAME_UTF8_LEN];
    /* AFP pathname format is advertised by each volume, not the server. */
    unsigned char path_encoding;
    unsigned short dtrefnum;
    unsigned char no_desktop; /*!< whether FPOpenDT answered kFPCallNotSupported */
    char volpassword[AFP_VOLPASS_LEN];
    unsigned int extra_flags; /* This is a Netatalk Client specific field */
    time_t mount_time; /* Time when the volume was mounted */

    /* The directory cache */
    struct dir_cache cache;

    /* Our journal of open forks, and how many of them are writable */
    struct afp_file_info *open_forks;
    unsigned int writable_forks;
    pthread_mutex_t open_forks_mutex;
    /* Read-locked by an open until listed; write-locked by a copy offload,
     * and by a rename or an exchange from its request to its forks' walk */
    pthread_rwlock_t open_lock;

    /* Used to trigger startup */
    pthread_cond_t  startup_condition_cond;

    void *priv;   /* This is a private structure for fuse/cmdline, etc */
    pthread_t thread; /* This is the per-volume thread */

    int mapping;

};

#define SERVER_STATE_CONNECTED 1
#define SERVER_STATE_DISCONNECTED 2
#define SERVER_STATE_CONNECTING 3
/* Taken off the server list; a request to it fails before it is sent. */
#define SERVER_STATE_DESTROYED 4

struct afp_versions {
    char        *av_name;
    int         av_number;
};

/* The server version, such as 4.7.0, is all zero when unknown. */
struct afpc_server_version {
    unsigned int major;
    unsigned int minor;
    unsigned int patch;
};

extern struct afp_versions afp_versions[];

struct afp_server {

    /* The quantum is the largest read or write one request carries. */
    unsigned int quantum;

    unsigned int tx_delay;
    int dsi_default_timeout;

    /* Connection information */
    //the linked list returned by getaddrinfo
    struct addrinfo *address;
    //the address we successfully connected to
    struct addrinfo *used_address;
    int fd;

    /* Some stats, for information only */
    struct {
        uint64_t runt_packets;
        uint64_t incoming_dsi;
        uint64_t rx_bytes;
        uint64_t tx_bytes;
        uint64_t requests_pending;
        /* It counts AFP requests sent, by command code. */
        uint64_t commands[UINT8_MAX + 1];
    } stats;

    /* General information */
    char server_name[AFP_SERVER_NAME_LEN];
    char server_name_utf8[AFP_SERVER_NAME_UTF8_LEN];
    char server_name_printable[AFP_SERVER_NAME_UTF8_LEN];
    /* Authority supplied by the client when this session was created.  This
     * is distinct from the name advertised by FPGetSrvrInfo. */
    char requested_hostname[AFP_SERVER_NAME_UTF8_LEN];
    int requested_port;

    char machine_type[17];
    /* The server software's version comes from machine_type. */
    struct afpc_server_version server_version;
    char icon[AFP_SERVER_ICON_LEN];
    char signature[16];
    unsigned short flags;
    int connect_state;
    enum afpc_server_type server_type;

    /* This is the time we connected */
    time_t connect_time;

    /* AFP 2.x timestamps use the server's local clock.  This is the
     * server-minus-client offset calculated from FPGetSrvrParms. */
    time_t time_offset;

    /* UAMs */
    unsigned int supported_uams;
    unsigned int using_uam;

    /* Authentication */
    char username[AFP_MAX_USERNAME_LEN];
    char password[AFP_MAX_PASSWORD_LEN];

    /* Session */
    struct afp_token token;
    char need_resume;
    char reconnect_in_progress;
    char suspended;

    /* Versions */
    unsigned char requested_version;
    unsigned char versions[SERVER_MAX_VERSIONS];
    struct afp_versions *using_version;

    /* Volumes */
    unsigned char num_volumes;
    struct afp_volume *volumes;

    void *dsi;
    unsigned int exit_flag;

    /* Our DSI request queue */
    pthread_mutex_t requestid_mutex;
    pthread_mutex_t request_queue_mutex;
    /* Signalled under request_queue_mutex when the receiver lets a
     * request go. */
    pthread_cond_t request_queue_cond;
    unsigned short lastrequestid;
    unsigned short expectedrequestid;
    struct dsi_request *command_requests;
    unsigned int connection_generation;  /* Incremented on each reconnect */

    /* AFP 3.3+ Replay cache support */
    unsigned int replay_cache_size;  /* Server's replay cache size */
    unsigned char supports_replay_cache;  /* 1 if server supports replay cache */


    char loginmesg[200];

    /* This is the data for the incoming buffer */
    char *incoming_buffer;
    int data_read;
    int bufsize;
    /* Payload bytes of a frame being dropped still to be read. */
    size_t discard_remaining;
    /* Payload bytes of the read reply in progress already in its buffer. */
    size_t stream_read;
    /* The connection generation whose stream the receive state follows. */
    unsigned int rx_generation;

    /* And this is for the outgoing queue */
    pthread_mutex_t send_mutex;

    /* This is for user mapping */
    struct passwd passwd;
    unsigned int server_uid, server_gid;
    int server_gid_valid;

    struct afp_server *next;

    /* Reference counting for safe multi-threaded access */
    int refcount;

    /* The attention quantum this side offers in DSIOpenSession. */
    unsigned int attention_quantum;

};

static inline int afp_server_reconnect_try_begin(struct afp_server *server)
{
    char expected = 0;
    return __atomic_compare_exchange_n(&server->reconnect_in_progress,
                                       &expected, 1, 0,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

static inline void afp_server_reconnect_end(struct afp_server *server)
{
    __atomic_store_n(&server->reconnect_in_progress, 0, __ATOMIC_RELEASE);
}

static inline int afp_server_reconnect_is_in_progress(
    const struct afp_server *server)
{
    return __atomic_load_n(&server->reconnect_in_progress,
                           __ATOMIC_ACQUIRE) != 0;
}

static inline void afp_server_set_suspended(struct afp_server *server,
                                            int suspended)
{
    __atomic_store_n(&server->suspended, suspended ? 1 : 0,
                     __ATOMIC_RELEASE);
}

static inline int afp_server_is_suspended(const struct afp_server *server)
{
    return __atomic_load_n(&server->suspended, __ATOMIC_ACQUIRE) != 0;
}

static inline unsigned int afp_server_connection_generation(
    const struct afp_server *server)
{
    return __atomic_load_n(&server->connection_generation,
                           __ATOMIC_ACQUIRE);
}

static inline unsigned int afp_server_next_connection_generation(
    struct afp_server *server)
{
    return __atomic_add_fetch(&server->connection_generation, 1,
                              __ATOMIC_ACQ_REL);
}

static inline void afp_server_connection_generation_init(
    struct afp_server *server)
{
    __atomic_store_n(&server->connection_generation, 0,
                     __ATOMIC_RELEASE);
}

struct afp_comment {
    unsigned int maxsize;
    unsigned int size;
    char *data;
};

struct afp_icon {
    unsigned int maxsize;
    unsigned int size;
    char *data;
};

/* Desktop database results intentionally carry no desktop reference number.
 * The reference is session-private state owned by struct afp_volume. */
struct afp_icon_info {
    uint32_t tag;
    uint32_t filetype;
    uint16_t size;
    uint8_t icontype;
};

/*! @brief An FPGetAPPL answer: the application's tag, parameters and name */
struct afp_appl {
    uint16_t bitmap;
    uint32_t tag;
    struct afp_dirent file;
    char name[AFPC_MAX_NAME_BYTES];
};

#define AFP_DEFAULT_ATTENTION_QUANTUM 1024

int init_uams(void);

unsigned int find_uam_by_name(const char *name);
char *uam_bitmap_to_string(unsigned int bitmap);


char *get_uam_names_list(void);

unsigned int default_uams_mask(void);

enum afpc_server_type afp_identify_machine_type(const char *machine_type);
int server_version_parse(const char *text,
                         struct afpc_server_version *version);
int server_version_at_least(const struct afpc_server_version *version,
                            unsigned int major, unsigned int minor,
                            unsigned int patch);
void afp_server_identify(struct afp_server * s);
int afp_server_is_apple(const struct afp_server *s);
int afp_server_reports_fork_length(const struct afp_server *s);
int afp_server_copies_files(const struct afp_server *s);
int afp_server_drops_long_commands(const struct afp_server *s);
int afp_server_gives_directory_ids_to_files(const struct afp_server *s);
int afp_server_keeps_unset_ids(const struct afp_server *s);
int afp_server_keeps_directory_modes(const struct afp_server *s);
void afp_server_fill_basic(const struct afp_server *server,
                           struct afpc_server_info *basic);

struct afp_volume *find_volume_by_name(struct afp_server * server,
                                       char *volname);

struct afp_connection_request {
    unsigned int uam_mask;
    struct afpc_url url;
};

int afp_list_volnames(struct afp_server * server, char *names, int max);

void afp_default_url(struct afpc_url *url);
int afp_parse_url(struct afpc_url *url, const char *text);
int afp_parse_url_quiet(struct afpc_url *url, const char *text);

/* User mapping */
int afp_detect_mapping(struct afp_volume * volume);

/* These are some functions that help with simple status text generation */

int afp_status_header(char *text, int *len);
int afp_status_server(struct afp_server * s, char *text, int *len);


struct afp_server *afp_server_full_connect(void *priv,
                                           struct afp_connection_request *req);

void *just_end_it_now(void *other);
void add_fd_and_signal(int fd);
void loop_disconnect(struct afp_server *s);
void loop_close_descriptor(struct afp_server *s, int fd);
void afp_wait_for_started_loop(void);


struct afp_versions *pick_version(unsigned char *versions,
                                  unsigned char requested);
int pick_uam(unsigned int u1, unsigned int u2);

int afp_server_login(struct afp_server *server,
                     char *mesg, unsigned int *l, unsigned int max);


int afp_dologin(struct afp_server *server,
                unsigned int uam, char *username, char *passwd);

int afp_dopasswd(struct afp_server *server,
                 unsigned int uam, char *username,
                 char *oldpasswd, char *newpasswd);

void afp_free_server(struct afp_server **server);

struct afp_server *afp_server_init(struct addrinfo * address);
struct addrinfo *afp_get_address(void *priv, const char *hostname,
                                 unsigned int port);


int afp_main_loop(int command_fd);
int afp_main_quick_startup(pthread_t *thread);

int afp_server_destroy(struct afp_server *s);
int afp_server_reconnect(struct afp_server * s, char *mesg,
                         unsigned int *l, unsigned int max);
int afp_server_connect(struct afp_server *server, int full);
/* A server identity is usable for session reuse only when FPGetSrvrInfo
 * supplies a nonzero 16-byte signature.  Some deployed servers omit
 * kSrvrSig despite supplying a configured signature. */
int afp_server_has_valid_signature(const struct afp_server *server);

/* Query a peer's FPGetSrvrInfo identity without logging in.  The caller
 * retains ownership of address. */
int afp_server_probe_signature(struct addrinfo *address,
                               char signature[AFP_SIGNATURE_LEN]);

/* Replace the address list used for future connections.  address ownership
 * transfers to server. */
void afp_server_replace_address(struct afp_server *server,
                                struct addrinfo *address);

struct afp_server *afp_server_complete_connection(
    void *priv,
    struct afp_server * server,
    unsigned char *versions,
    unsigned int uams, char *username, char *password,
    unsigned int requested_version, unsigned int uam_mask);

int afp_connect_volume(struct afp_volume * volume, struct afp_server * server,
                       char *mesg, unsigned int *l, unsigned int max);
int something_is_mounted(struct afp_server * server);
int something_is_attached(struct afp_server * server);

int add_cache_entry(struct afp_file_info * file);
struct afp_file_info *get_cache_by_name(char *name);
struct afp_server *find_server_by_address(struct addrinfo * address);
struct afp_server *find_server_by_pointer(struct afp_server * target);
struct afp_server *find_server_by_signature(char *signature);
struct afp_server *find_server_by_name(char *name);
int server_still_valid(struct afp_server * server);

/* Reference-counted server lookups for multi-threaded use.
 * These atomically find and hold a reference. Caller must
 * call afp_server_release() when done with the returned pointer. */
struct afp_server *afp_server_find_by_name_hold(char *name);
struct afp_server *afp_server_find_by_address_hold(struct addrinfo * address);
struct afp_volume *afp_volume_find_by_pointer_hold(void *id);
struct afp_volume *afp_volume_find_by_mountpoint_hold(const char *mountpoint);

void afp_server_hold(struct afp_server *s);
void afp_server_release(struct afp_server *s);

void afp_lock_server_list(void);
void afp_unlock_server_list(void);

struct afp_server *get_server_base(void);
int afp_server_remove(struct afp_server * server);

int afp_detach_volume(struct afp_volume * volume);
int afp_unmount_volume(struct afp_volume * volume);
int afp_unmount_all_volumes(struct afp_server * server);

#define volume_is_readonly(x) (((x)->attributes&kReadOnly) || \
	((x)->extra_flags & VOLUME_EXTRA_FLAGS_READONLY))

int afp_opendt(struct afp_volume *volume, unsigned short *refnum);

int afp_closedt(struct afp_server * server, unsigned short refnum);

int afp_getcomment(struct afp_volume *volume, unsigned int did,
                   const char *pathname, struct afp_comment * comment);

int afp_addcomment_sized(struct afp_volume *volume, unsigned int did,
                         const char *pathname, const void *comment,
                         size_t comment_size);

int afp_geticon(struct afp_volume * volume, unsigned int filecreator,
                unsigned int filetype, unsigned char icontype,
                unsigned short length, struct afp_icon * icon);
int afp_geticoninfo(struct afp_volume *volume, unsigned int filecreator,
                    unsigned short index, struct afp_icon_info *info);
int afp_getappl(struct afp_volume *volume, unsigned int filecreator,
                unsigned short index, unsigned short bitmap,
                struct afp_appl *appl);

/* Things you want to do to a server */

int afp_getsrvrmsg(struct afp_server *server, unsigned short messagetype,
                   unsigned char utf8, unsigned char block, char *mesg);

int afp_login(struct afp_server *server, const char *uaname,
              char *userauthinfo, unsigned int userauthinfo_len,
              struct afp_rx_buffer *rx);

int afp_loginext(struct afp_server *server, const char *uaname,
                 const char *username,
                 char *userauthinfo, unsigned int userauthinfo_len,
                 struct afp_rx_buffer *rx);

int afp_changepassword(struct afp_server *server, const char *uaname,
                       char *userauthinfo, unsigned int userauthinfo_len,
                       struct afp_rx_buffer *rx);

int afp_logincont(struct afp_server *server, unsigned short id,
                  char *userauthinfo, unsigned int userauthinfo_len,
                  struct afp_rx_buffer *rx);

int afp_getsessiontoken(struct afp_server * server, int type,
                        unsigned int timestamp, struct afp_token *outgoing_token,
                        struct afp_token * incoming_token);

int afp_getsrvrparms(struct afp_server *server);

int afp_logout(struct afp_server *server, unsigned char wait);

int afp_mapname(struct afp_server * server, unsigned char subfunction,
                char *name, unsigned int *id);

int afp_mapid(struct afp_server * server, unsigned char subfunction,
              unsigned int id, char *name);

int afp_getuserinfo(struct afp_server * server, int thisuser,
                    unsigned int userid, unsigned short bitmap,
                    unsigned int *newuid, unsigned int *newgid);

int afp_zzzzz(struct afp_server *server);

int afp_volopen(struct afp_volume * volume,
                unsigned short bitmap, char *password);

int afp_getfiledirparms(struct afp_volume *volume, unsigned int did,
                        unsigned int filebitmap, unsigned int dirbitmap, const char *pathname,
                        struct afp_dirent *e);

int afp_enumerate(struct afp_volume * volume,
                  unsigned int dirid,
                  unsigned int filebitmap, unsigned int dirbitmap,
                  unsigned short reqcount,
                  unsigned short startindex,
                  char *path,
                  struct afp_listing *l);

int afp_enumerateext(struct afp_volume * volume,
                     unsigned int dirid,
                     unsigned int filebitmap, unsigned int dirbitmap,
                     unsigned short reqcount,
                     unsigned short startindex,
                     char *path,
                     struct afp_listing *l);

int afp_enumerateext2(struct afp_volume * volume,
                      unsigned int dirid,
                      unsigned int filebitmap, unsigned int dirbitmap,
                      unsigned short reqcount,
                      unsigned long startindex,
                      char *path,
                      struct afp_listing *l);

int afp_openfork(struct afp_volume * volume,
                 unsigned char forktype,
                 unsigned int dirid,
                 unsigned short accessmode,
                 unsigned short bitmap,
                 char *filename,
                 struct afp_file_info *fp);

int afp_read(struct afp_volume * volume, unsigned short forkid,
             uint32_t offset,
             uint32_t count, struct afp_rx_buffer * rx);

int afp_readext(struct afp_volume * volume, unsigned short forkid,
                uint64_t offset,
                uint64_t count, struct afp_rx_buffer * rx);

int afp_getvolparms(struct afp_volume * volume, unsigned short bitmap);


int afp_createdir(struct afp_volume * volume, unsigned int dirid,
                  const char *pathname, unsigned int *did_p);

int afp_delete(struct afp_volume * volume,
               unsigned int dirid, char *pathname);

int afp_copyfile(struct afp_volume * volume,
                 unsigned int srcdid, char *srcname,
                 unsigned int dstdid, char *dstname, int wait);


int afp_createfile(struct afp_volume * volume, unsigned char flag,
                   unsigned int did, char *pathname);

int afp_write(struct afp_volume * volume, unsigned short forkid,
              unsigned char flag, uint32_t offset, uint32_t reqcount,
              char *data, uint32_t *written);

int afp_writeext(struct afp_volume * volume, unsigned short forkid,
                 unsigned char flag, uint64_t offset, uint64_t reqcount,
                 char *data, uint64_t *written);

int afp_flushfork(struct afp_volume * volume, unsigned short forkid,
                  int wait);

int afp_syncfork(struct afp_volume * volume, unsigned short forkid,
                 int wait);

int afp_closefork(struct afp_volume * volume, unsigned short forkid);
int afp_setfiledirparms(struct afp_volume * volume,
                        unsigned int dirid, const char *pathname, unsigned short bitmap,
                        const struct afp_dirent *e);
int afp_setdirparms(struct afp_volume * volume, unsigned int dirid,
                    const char *pathname, unsigned short bitmap,
                    const struct afp_dirent *e);

int afp_volclose(struct afp_volume * volume);


int afp_setforkparms(struct afp_volume *volume,
                     unsigned short forkid, unsigned short bitmap,
                     uint64_t len);

int afp_byterangelock(struct afp_volume * volume,
                      unsigned char flag,
                      unsigned short forkid,
                      uint32_t offset,
                      uint32_t len, uint32_t *generated_offset);

int afp_byterangelockext(struct afp_volume * volume,
                         unsigned char flag,
                         unsigned short forkid,
                         uint64_t offset,
                         uint64_t len, uint64_t *generated_offset);

int afp_exchangefiles(struct afp_volume *volume,
                      unsigned int src_did,
                      unsigned int dst_did,
                      char *src_path, char *dst_path);

int afp_moveandrename(struct afp_volume *volume,
                      unsigned int src_did,
                      unsigned int dst_did,
                      char *src_path, char *dst_path, char *new_name);

int afp_rename(struct afp_volume * volume,
               unsigned int dirid,
               char *path_from, char *path_to);

int afp_listextattr(struct afp_volume * volume,
                    unsigned int dirid, unsigned short bitmap,
                    char *pathname, struct afp_extattr_info * info);

int afp_getextattr(struct afp_volume * volume, unsigned int dirid,
                   unsigned short bitmap, unsigned int replysize,
                   const char *pathname, unsigned short namelen, const char *name,
                   struct afp_extattr_info * info);

int afp_setextattr(struct afp_volume * volume, unsigned int dirid,
                   unsigned short bitmap, uint64_t offset, const char *pathname,
                   unsigned short namelen, const char *name,
                   unsigned int attribdatalen, const char *attribdata);

int afp_removeextattr(struct afp_volume * volume, unsigned int dirid,
                      unsigned short bitmap, const char *pathname,
                      unsigned short namelen, const char *name);

/* For debugging */
char *afp_get_command_name(unsigned char code);

/* From Netatalk's adouble.h. */
#define AD_DATE_DELTA 946684800LL

/* AFP dates are signed, network-order, seconds from 2000-01-01. */
time_t afp_date_to_unix(const struct afp_server *server, uint32_t date);
uint32_t afp_date_from_unix(const struct afp_server *server, time_t date);

/* The listing's records and pool (listing.c) */
int afp_listing_reserve(struct afp_listing *l, uint32_t entries,
                        size_t bytes);
int afp_listing_append(struct afp_listing *l, const struct afp_dirent *e,
                       const char *suffix, unsigned int kind, uint64_t size);
void afp_listing_commit(struct afp_listing *l);
void afp_listing_drop_unnamed(struct afp_listing *l);
void afp_listing_free(struct afp_listing *l);
mode_t afp_dirent_mode(const struct afp_dirent *e);
int afp_dirent_to_stat(struct afp_volume *volume, const struct afp_dirent *e,
                       struct stat *stbuf);
struct timespec afp_dirent_time(const struct afp_volume *volume,
                                const struct afp_dirent *e, uint32_t bit,
                                int64_t ns);
void afp_pseudo_file_stat(struct stat *stbuf, off_t size);

/*! @brief An entry's NUL-terminated name in the UNIX form, in the pool */
static inline const char *afp_dirent_name(const struct afp_listing *l,
                                          const struct afp_dirent *e)
{
    return l->pool + e->name_off;
}

#endif
