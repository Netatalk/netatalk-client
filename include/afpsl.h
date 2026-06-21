#ifndef __AFPSL_H_
#define __AFPSL_H_

#include <syslog.h>
#include <utime.h>

#include "afp.h"
#include "errno.h"

/* Maximum value or resource-fork payload per stateless metadata request. */
#define AFP_SL_METADATA_CHUNK 4096

/* Maximum returned xattr name list.  Lists have a separate limit because
 * namespace prefixes can make them larger than the AFP reply containing the
 * original names. */
#define AFP_SL_XATTR_LIST_MAX (224U * 1024U)

/* Portable setxattr flags.  Their values match XATTR_CREATE and
 * XATTR_REPLACE on platforms that provide those constants. */
enum afp_sl_xattr_flags {
    AFP_SL_XATTR_CREATE = 0x1,
    AFP_SL_XATTR_REPLACE = 0x2,
};

enum afp_sl_recovery_action {
    AFP_SL_RECOVERY_NONE,
    AFP_SL_RECOVERY_RECONNECT,
    AFP_SL_RECOVERY_REATTACH,
};

enum afp_sl_attach_status {
    AFP_SL_ATTACH_STATUS_NONE,
    AFP_SL_ATTACH_STATUS_PASSWORD_REQUIRED,
};

enum afp_sl_password_change_status {
    AFP_SL_PASSWORD_CHANGE_STATUS_NONE,
    AFP_SL_PASSWORD_CHANGE_STATUS_ACCESS_DENIED,
    AFP_SL_PASSWORD_CHANGE_STATUS_INCORRECT_OLD_PASSWORD,
    AFP_SL_PASSWORD_CHANGE_STATUS_UNSUPPORTED_AUTHENTICATION,
    AFP_SL_PASSWORD_CHANGE_STATUS_UNCHANGED,
    AFP_SL_PASSWORD_CHANGE_STATUS_TOO_SHORT,
    AFP_SL_PASSWORD_CHANGE_STATUS_EXPIRED,
    AFP_SL_PASSWORD_CHANGE_STATUS_POLICY_VIOLATION,
    AFP_SL_PASSWORD_CHANGE_STATUS_INVALID_PARAMETER,
};

/* Local filesystem representation used by metadata transfer helpers.  AUTO
 * probes filesystem xattrs, then falls back to Netatalk AppleDouble. */
enum afp_metadata_mode {
    AFP_METADATA_AUTO,
    AFP_METADATA_XATTR,
    AFP_METADATA_MACOS,
    AFP_METADATA_NETATALK,
    AFP_METADATA_NONE,
};

enum afp_metadata_warning {
    AFP_METADATA_WARNING_NONE = 0,
    AFP_METADATA_WARNING_UNSUPPORTED = 1U << 0,
    AFP_METADATA_WARNING_VALUE_TOO_LARGE = 1U << 1,
    AFP_METADATA_WARNING_LIST_TOO_LARGE = 1U << 2,
};

/* Basic file information structure for stateless API
 * This is the wire protocol format sent between afpsld and clients.
 * Contains essential file metadata without the full afp_file_info details.
 */
struct afp_file_info_basic {
    char name[AFP_MAX_PATH];
    unsigned int creation_date;
    unsigned int modification_date;
    struct afp_unixprivs unixprivs;
    unsigned long long size;
};

struct afp_volume_summary {
    char volume_name_printable[AFP_VOLUME_NAME_UTF8_LEN];
    char flags;
};

typedef void   *serverid_t;
typedef void   *volumeid_t;

/* loglevel uses the LOG_* values from syslog.h. The callback runs
 * synchronously on the thread making the afp_sl_* call. */
typedef void (*afp_sl_log_callback)(void *user_data, int loglevel,
                                    const char *message);

void afp_sl_set_log_callback(afp_sl_log_callback callback, void *user_data);

/* Unless documented otherwise, libafpsl operations return zero on success and
 * negative errno on failure. Read-like metadata operations return a
 * nonnegative byte count on success and negative errno on failure. */
int afp_sl_exit(void);
int afp_sl_status(const char * volumename, const char * servername,
                  char *text, unsigned int *remaining);
int afp_sl_connect(struct afp_url * url, unsigned int uam_mask,
                   serverid_t *id, char *loginmesg);
int afp_sl_disconnect(serverid_t *id);
int afp_sl_getvolid(struct afp_url * url, volumeid_t *volid);
/* status is optional. On return it distinguishes a volume-password challenge
 * from other -EACCES failures. */
int afp_sl_attach(struct afp_url * url, unsigned int volume_options,
                  volumeid_t *volumeid,
                  enum afp_sl_attach_status *status);
int afp_sl_detach(volumeid_t * volumeid,
                  struct afp_url * url);
int afp_sl_readdir(volumeid_t * volid, const char * path, struct afp_url * url,
                   int start, int count, unsigned int *numfiles,
                   struct afp_file_info_basic **fpb,
                   int *eod);
int afp_sl_getvols(struct afp_url * url, unsigned int start,
                   unsigned int count, unsigned int *numvols,
                   struct afp_volume_summary * vols);
int afp_sl_stat(volumeid_t * volid, const char * path,
                struct afp_url * url, struct stat * stat);
int afp_sl_open(volumeid_t * volid, const char * path,
                struct afp_url * url, unsigned int *fileid,
                unsigned int mode);
int afp_sl_read(volumeid_t * volid, unsigned int fileid, unsigned int resource,
                unsigned long long start,
                unsigned int length, unsigned int *received,
                unsigned int *eof, char *data);
int afp_sl_write(volumeid_t * volid, unsigned int fileid, unsigned int resource,
                 unsigned long long offset, unsigned int size,
                 unsigned int *written, const char *data);
int afp_sl_creat(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode);
int afp_sl_chmod(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode);
int afp_sl_rename(volumeid_t * volid, const char * path_from,
                  const char *path_to, struct afp_url * url);
int afp_sl_unlink(volumeid_t * volid, const char * path,
                  struct afp_url * url);
int afp_sl_truncate(volumeid_t * volid, const char * path,
                    struct afp_url * url, unsigned long long offset);
int afp_sl_utime(volumeid_t * volid, const char * path,
                 struct afp_url * url, struct utimbuf * times);
int afp_sl_mkdir(volumeid_t * volid, const char * path,
                 struct afp_url * url, mode_t mode);
int afp_sl_rmdir(volumeid_t * volid, const char * path,
                 struct afp_url * url);
int afp_sl_statfs(volumeid_t * volid, const char * path,
                  struct afp_url * url, struct statvfs * stat);
int afp_sl_close(volumeid_t * volid, unsigned int fileid);
int afp_sl_serverinfo(struct afp_url * url, struct afp_server_basic * basic);
/* status is optional. On failure it provides password-policy detail that is
 * more specific than the returned errno. */
int afp_sl_changepw(struct afp_url * url,
                    const char *old_password,
                    const char *new_password,
                    enum afp_sl_password_change_status *status);

/* Metadata read and list calls return the byte count on success. Write and
 * remove calls return zero on success. A size of zero queries the
 * required size for get/list operations. Generic xattr values are limited to
 * AFP_SL_METADATA_CHUNK bytes. Xattr name lists are limited to
 * AFP_SL_XATTR_LIST_MAX bytes. Resource forks are limited to INT_MAX bytes and
 * are transferred with repeated calls at increasing offsets.
 *
 * afp_sl_setxattr accepts zero or one of AFP_SL_XATTR_CREATE and
 * AFP_SL_XATTR_REPLACE; other flag combinations return -EINVAL.
 * Resource-fork writes overwrite or extend the specified range but do not
 * shorten an existing fork. A zero-length write at offset zero clears
 * the fork. Use afp_sl_truncateresourcefork to set any final length explicitly. */
int afp_sl_getxattr(volumeid_t *volid, const char *path, const char *name,
                    void *value, size_t size);
int afp_sl_setxattr(volumeid_t *volid, const char *path, const char *name,
                    void *value, size_t size, int flags);
int afp_sl_listxattr(volumeid_t *volid, const char *path, char *list,
                     size_t size);
int afp_sl_removexattr(volumeid_t *volid, const char *path, const char *name);
int afp_sl_getfinderinfo(volumeid_t *volid, const char *path, void *value,
                         size_t size);
int afp_sl_setfinderinfo(volumeid_t *volid, const char *path, const void *value,
                         size_t size);
int afp_sl_removefinderinfo(volumeid_t *volid, const char *path);
int afp_sl_getresourcefork(volumeid_t *volid, const char *path, void *value,
                           size_t size, unsigned long long offset);
int afp_sl_setresourcefork(volumeid_t *volid, const char *path,
                           const void *value, size_t size,
                           unsigned long long offset);
int afp_sl_truncateresourcefork(volumeid_t *volid, const char *path,
                                unsigned long long size);
int afp_sl_removeresourcefork(volumeid_t *volid, const char *path);

/* Classify errno-style failures. ESTALE requires restoring the volume
 * attachment; connection failures require rebuilding the daemon/session
 * connection. */
enum afp_sl_recovery_action afp_sl_recovery_for_error(int result);

/* Metadata transfer helpers provide replacement semantics: metadata is
 * cleared at the existing destination before the copy starts.  They do not
 * copy the data fork, POSIX mode, or timestamps, and replacement is not
 * atomic.  Unsupported or unrepresentable metadata is reported through the
 * optional warnings bitmask; other failures are returned as negative errno.
 * Missing metadata is not an error, but a missing source or destination path
 * is returned as -ENOENT. */
int afp_metadata_mode_parse(const char *name, enum afp_metadata_mode *mode);
const char *afp_metadata_mode_name(enum afp_metadata_mode mode);
int afp_metadata_clear_local(const char *path,
                             enum afp_metadata_mode mode,
                             unsigned int *warnings);
int afp_sl_metadata_clear(volumeid_t *volume, const char *path,
                          unsigned int *warnings);
int afp_sl_metadata_copy_local_to_remote(
    const char *local_path, enum afp_metadata_mode local_mode,
    volumeid_t *destination_volume, const char *destination_path,
    unsigned int *warnings);
int afp_sl_metadata_copy_remote_to_local(
    volumeid_t *source_volume, const char *source_path,
    const char *local_path, enum afp_metadata_mode local_mode,
    unsigned int *warnings);
int afp_sl_metadata_copy_remote_to_remote(
    volumeid_t *source_volume, const char *source_path,
    volumeid_t *destination_volume, const char *destination_path,
    unsigned int *warnings);
#endif
