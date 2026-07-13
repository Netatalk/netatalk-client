#ifndef NETATALK_CLIENT_AFPSL_H
#define NETATALK_CLIENT_AFPSL_H

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <syslog.h>
#include <utime.h>

#include "types.h"
#include "url.h"

#ifdef __cplusplus
extern "C" {
#endif

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

enum afp_sl_volume_option {
    /* Disable AFP byte-range locking for the attached volume. */
    AFP_SL_VOLUME_NO_LOCKING = 0x10,
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

enum afp_sl_uam {
    AFP_SL_UAM_NO_USER_AUTH = 0x1,
    AFP_SL_UAM_CLEARTEXT = 0x2,
    AFP_SL_UAM_RANDNUM = 0x4,
    AFP_SL_UAM_TWO_WAY_RANDNUM = 0x8,
    AFP_SL_UAM_DHCAST128 = 0x10,
    AFP_SL_UAM_KERBEROS = 0x20,
    AFP_SL_UAM_DHX2 = 0x40,
    AFP_SL_UAM_RECONNECT = 0x80,
    AFP_SL_UAM_SRP = 0x100,
};

/* Local filesystem representation used by metadata transfer helpers.  AUTO
 * probes generic filesystem xattrs, then falls back to Netatalk AppleDouble
 * EA sidecars.  FinderInfo and ResourceFork use native xattrs on macOS and
 * macOS AppleDouble sidecars elsewhere unless Netatalk mode is selected. */
enum afp_metadata_mode {
    AFP_METADATA_AUTO,
    AFP_METADATA_NETATALK,
    AFP_METADATA_XATTR,
    AFP_METADATA_MACOS,
    AFP_METADATA_NONE,
};

enum afp_metadata_warning {
    AFP_METADATA_WARNING_NONE = 0,
    AFP_METADATA_WARNING_UNSUPPORTED = 1U << 0,
    AFP_METADATA_WARNING_VALUE_TOO_LARGE = 1U << 1,
    AFP_METADATA_WARNING_LIST_TOO_LARGE = 1U << 2,
};

/* loglevel uses the LOG_* values from syslog.h. The callback runs
 * synchronously on the thread making the afp_sl_* call. */
typedef void (*afp_sl_log_callback)(void *user_data, int loglevel,
                                    const char *message);

void afp_sl_set_log_callback(afp_sl_log_callback callback, void *user_data);
unsigned int afp_sl_default_uams(void);
/* Resolve a full UAM name or afpcmd-style shorthand to one supported bit. */
unsigned int afp_sl_uam_by_name(const char *name);

/* Unless documented otherwise, libafpsl operations return zero on success and
 * negative errno on failure. Read-like metadata operations return a
 * nonnegative byte count on success and negative errno on failure. */
int afp_sl_exit(void);
int afp_sl_status(const char * volumename, const char * servername,
                  char *text, unsigned int *remaining);
int afp_sl_connect(struct afpc_url * url, unsigned int uam_mask,
                   afpc_server_t *id, char *loginmesg);
int afp_sl_resume(struct afpc_url * url, unsigned int uam_mask,
                  afpc_server_t *id, char *loginmesg);
int afp_sl_disconnect(afpc_server_t *id);
int afp_sl_getvolid(afpc_server_t serverid, struct afpc_url * url,
                    afpc_volume_t *volid);
/* volume_options is zero or a bitwise combination of afp_sl_volume_option.
 * status is optional. On return it distinguishes a volume-password challenge
 * from other -EACCES failures. */
int afp_sl_attach(afpc_server_t serverid, struct afpc_url * url,
                  unsigned int volume_options, afpc_volume_t *volumeid,
                  enum afp_sl_attach_status *status);
int afp_sl_detach(afpc_volume_t * volumeid,
                  struct afpc_url * url);
int afp_sl_readdir(afpc_volume_t * volid, const char * path,
                   struct afpc_url * url,
                   int start, int count, unsigned int *numfiles,
                   struct afpc_file_info **fpb,
                   int *eod);
int afp_sl_getvols(afpc_server_t serverid, struct afpc_url * url,
                   unsigned int start,
                   unsigned int count, unsigned int *numvols,
                   struct afpc_volume_info * vols);
int afp_sl_stat(afpc_volume_t * volid, const char * path,
                struct afpc_url * url, struct stat * stat);
int afp_sl_open(afpc_volume_t * volid, const char * path,
                struct afpc_url * url, unsigned int *fileid,
                unsigned int mode);
int afp_sl_read(afpc_volume_t * volid, unsigned int fileid,
                unsigned int resource,
                unsigned long long start,
                unsigned int length, unsigned int *received,
                unsigned int *eof, char *data);
int afp_sl_write(afpc_volume_t * volid, unsigned int fileid,
                 unsigned int resource,
                 unsigned long long offset, unsigned int size,
                 unsigned int *written, const char *data);
int afp_sl_creat(afpc_volume_t * volid, const char * path,
                 struct afpc_url * url, mode_t mode);
int afp_sl_chmod(afpc_volume_t * volid, const char * path,
                 struct afpc_url * url, mode_t mode);
int afp_sl_rename(afpc_volume_t * volid, const char * path_from,
                  const char *path_to, struct afpc_url * url);
int afp_sl_unlink(afpc_volume_t * volid, const char * path,
                  struct afpc_url * url);
int afp_sl_truncate(afpc_volume_t * volid, const char * path,
                    struct afpc_url * url, unsigned long long offset);
int afp_sl_utime(afpc_volume_t * volid, const char * path,
                 struct afpc_url * url, struct utimbuf * times);
int afp_sl_mkdir(afpc_volume_t * volid, const char * path,
                 struct afpc_url * url, mode_t mode);
int afp_sl_rmdir(afpc_volume_t * volid, const char * path,
                 struct afpc_url * url);
int afp_sl_statfs(afpc_volume_t * volid, const char * path,
                  struct afpc_url * url, struct statvfs * stat);
int afp_sl_close(afpc_volume_t * volid, unsigned int fileid);
int afp_sl_serverinfo(struct afpc_url * url, struct afpc_server_info * basic);
/* status is optional. On failure it provides password-policy detail that is
 * more specific than the returned errno. */
int afp_sl_changepw(struct afpc_url * url,
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
 * Resource fork writes overwrite or extend the specified range but do not
 * shorten an existing fork. A zero-length write at offset zero clears
 * the fork. Use afp_sl_truncateresourcefork to set any final length explicitly. */
int afp_sl_getxattr(afpc_volume_t *volid, const char *path, const char *name,
                    void *value, size_t size);
int afp_sl_setxattr(afpc_volume_t *volid, const char *path, const char *name,
                    void *value, size_t size, int flags);
int afp_sl_listxattr(afpc_volume_t *volid, const char *path, char *list,
                     size_t size);
int afp_sl_removexattr(afpc_volume_t *volid, const char *path,
                       const char *name);
int afp_sl_getfinderinfo(afpc_volume_t *volid, const char *path, void *value,
                         size_t size);
int afp_sl_setfinderinfo(afpc_volume_t *volid, const char *path,
                         const void *value,
                         size_t size);
int afp_sl_removefinderinfo(afpc_volume_t *volid, const char *path);
int afp_sl_getresourcefork(afpc_volume_t *volid, const char *path, void *value,
                           size_t size, unsigned long long offset);
int afp_sl_setresourcefork(afpc_volume_t *volid, const char *path,
                           const void *value, size_t size,
                           unsigned long long offset);
int afp_sl_truncateresourcefork(afpc_volume_t *volid, const char *path,
                                unsigned long long size);
int afp_sl_removeresourcefork(afpc_volume_t *volid, const char *path);

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
int afp_sl_metadata_clear(afpc_volume_t *volume, const char *path,
                          unsigned int *warnings);
int afp_sl_metadata_copy_local_to_remote(
    const char *local_path, enum afp_metadata_mode local_mode,
    afpc_volume_t *destination_volume, const char *destination_path,
    unsigned int *warnings);
int afp_sl_metadata_copy_remote_to_local(
    afpc_volume_t *source_volume, const char *source_path,
    const char *local_path, enum afp_metadata_mode local_mode,
    unsigned int *warnings);
int afp_sl_metadata_copy_remote_to_remote(
    afpc_volume_t *source_volume, const char *source_path,
    afpc_volume_t *destination_volume, const char *destination_path,
    unsigned int *warnings);

#ifdef __cplusplus
}
#endif

#endif
