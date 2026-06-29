/*
    Copyright (C) 1987-2002 Free Software Foundation, Inc.
    Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2024-2026 Daniel Markstedt <daniel@mindani.net>

    This is based on readline's fileman.c example, which is very useful.
    The original fileman.c carries the following notice:

    This file is part of the GNU Readline Library, a library for
    reading lines of text with interactive input and history editing.

    The GNU Readline Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2, or
    (at your option) any later version.

    The GNU Readline Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    The GNU General Public License is often shipped with GNU software, and
    is generally kept in a file called COPYING or LICENSE.  If you do not
    have a copy of the license, write to the Free Software Foundation,
    59 Temple Place, Suite 330, Boston, MA 02111 USA.
*/

#include "afp.h"
#include "afpsl.h"
#include "afp_server.h"
#include "compat.h"
#include "map_def.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#ifdef HAVE_LIBREADLINE
#include <readline/readline.h>
#elif defined(HAVE_LIBEDIT)
#include <editline/readline.h>
#endif

#include "compat.h"
#include "libafpclient.h"
#include "utils.h"
#include "cmdline_afp.h"
#include "cmdline_main.h"

static char curdir[AFP_MAX_PATH];
static struct afp_url url;
static int cmdline_log_min_rank = 2; /* Default rank: notice */
static int verbose_mode = 0;
static char connect_servername[AFP_SERVER_NAME_UTF8_LEN];

int full_url = 0;

#define DEFAULT_DIRECTORY "/"
#define METADATA_OUTPUT_MODE 0600

static volumeid_t vol_id = NULL;
static serverid_t server_id = NULL;
static int connected = 0;
static enum afp_metadata_mode transfer_metadata_mode = AFP_METADATA_AUTO;
static int metadata_warning_emitted = 0;

static int write_all_fd(int fd, const void *data, size_t size);

/* Metadata sidecars are an implementation detail of the selected local
 * storage mode.  Recursive uploads must not expose them as AFP data files;
 * their contents are transferred while processing the corresponding file. */
static int metadata_sidecar_entry(const char *name)
{
    if (transfer_metadata_mode == AFP_METADATA_AUTO) {
        return strncmp(name, "._", 2) == 0
               || strcmp(name, ".AppleDouble") == 0;
    }

    if (transfer_metadata_mode == AFP_METADATA_MACOS) {
        return strncmp(name, "._", 2) == 0;
    }

    if (transfer_metadata_mode == AFP_METADATA_NETATALK) {
        return strcmp(name, ".AppleDouble") == 0;
    }

    return 0;
}

/* Consume the command-local recursive option.  Command handlers receive the
 * text following the command name, so this deliberately only recognizes -r
 * as the first argument (for example, "get -r directory"). */
static int command_recursive_option(char **arg)
{
    char *p = *arg;

    while (isspace((unsigned char) * p)) {
        p++;
    }

    if (p[0] != '-' || p[1] != 'r'
            || (p[2] != '\0' && !isspace((unsigned char)p[2]))) {
        return 0;
    }

    p += 2;

    while (isspace((unsigned char) * p)) {
        p++;
    }

    *arg = p;
    return 1;
}

static int attach_volume_with_password_prompt(volumeid_t *vol_id_ptr,
        unsigned int volume_options);

static unsigned int get_uam_mask_for_url(void)
{
    unsigned int uam_mask;

    if (url.uamname[0] != '\0') {
        uam_mask = find_uam_by_name(url.uamname);
    } else {
        uam_mask = default_uams_mask();
    }

    return uam_mask;
}

static int is_recoverable_session_error(int ret)
{
    return afp_sl_recovery_for_error(ret) != AFP_SL_RECOVERY_NONE;
}

static int reconnect_session(int restore_volume, int restore_dir)
{
    char mesg[MAX_ERROR_LEN];
    unsigned int uam_mask;
    int ret;
    serverid_t new_server_id = NULL;
    volumeid_t new_vol_id = NULL;
    serverid_t old_server_id;
    struct afp_url reconnect_url;
    char saved_volume[AFP_VOLUME_NAME_LEN];
    char saved_dir[AFP_MAX_PATH];
    int had_volume;
    unsigned int volume_options = VOLUME_EXTRA_FLAGS_NO_LOCKING;

    if (!connected) {
        return -1;
    }

    memset(mesg, 0, sizeof(mesg));
    strlcpy(saved_volume, url.volumename, sizeof(saved_volume));
    strlcpy(saved_dir, curdir, sizeof(saved_dir));
    had_volume = (vol_id != NULL);
    old_server_id = server_id;

    /* Drop local cached handles; stale IDs are no longer trustworthy. */
    if (old_server_id && afp_sl_disconnect(&old_server_id) != 0) {
        afp_sl_exit();
    }

    uam_mask = get_uam_mask_for_url();

    if (uam_mask == 0) {
        return -1;
    }

    reconnect_url = url;

    if (connect_servername[0] != '\0') {
        strlcpy(reconnect_url.servername, connect_servername,
                sizeof(reconnect_url.servername));
    }

    if (afp_sl_connect(&reconnect_url, uam_mask, &new_server_id, mesg) != 0) {
        return -1;
    }

    if ((restore_volume || had_volume) && saved_volume[0] != '\0') {
        strlcpy(url.volumename, saved_volume, sizeof(url.volumename));
        ret = attach_volume_with_password_prompt(&new_vol_id, volume_options);

        if (ret != 0) {
            return -1;
        }
    }

    server_id = new_server_id;
    vol_id = new_vol_id;
    connected = 1;

    if (restore_dir && saved_dir[0] != '\0') {
        strlcpy(curdir, saved_dir, sizeof(curdir));
    }

    return 0;
}

static int escape_paths(char * outgoing1, char * outgoing2, char * incoming)
{
    char *writeto = outgoing1;
    int inquote = 0, inescape = 0, donewith1 = 0;
    char *p = incoming;
    size_t incoming_len;

    if (outgoing1 == NULL || incoming == NULL) {
        goto error;
    }

    incoming_len = strnlen(incoming, AFP_MAX_PATH);

    if (incoming_len >= AFP_MAX_PATH) {
        goto error;
    }

    if (incoming_len == 0) {
        goto error;
    }

    memset(outgoing1, 0, AFP_MAX_PATH);

    if (outgoing2) {
        memset(outgoing2, 0, AFP_MAX_PATH);
    }

    for (p = incoming; p < incoming + incoming_len; p++) {
        if (*p == '"') {
            if (inescape) {
                inescape = 0;
                goto add;
            } else if (inquote) {
                inquote = 0;
                continue;
            } else {
                inquote = 1;
                continue;
            }
        }

        if (*p == ' ') {
            if (inescape) {
                inescape = 0;
                goto add;
            } else if (inquote) {
                goto add;
            } else if ((donewith1 == 1) || (outgoing2 == NULL)) {
                goto out;
            }

            writeto = outgoing2;
            donewith1 = 1;
            continue;
        }

        if (*p == '\\' && inescape == 0) {
            inescape = 1;
            continue;
        } else if (inescape) {
            inescape = 0;
            goto add;
        }

add:
        *writeto = *p;
        writeto++;
    }

out:

    if ((outgoing2 != NULL) && (donewith1 == 0)) {
        goto error;
    }

    return 0;
error:
    return -1;
}

static unsigned int tvdiff(struct timeval * starttv, struct timeval * endtv)
{
    unsigned int d;
    d = (endtv->tv_sec - starttv->tv_sec) * 1000;
    d += (endtv->tv_usec - starttv->tv_usec) / 1000;
    return d;
}

static void printdiff(struct timeval * starttv, struct timeval *endtv,
                      unsigned long long *amount_written)
{
    unsigned int diff;
    unsigned long long kb_written;
    diff = tvdiff(starttv, endtv);
    float frac = ((float) diff) / 1000.0; /* Now in seconds */
    printf("    Transferred %lld bytes in ", *amount_written);
    printf("%.3f seconds. ", frac);
    /* Now calculate the transfer rate */
    kb_written = (*amount_written / 1000);
    float rate = (kb_written) / frac;
    printf("(%.0f kB/s)\n", rate);
}

static int cmdline_getpass(void)
{
    char *passwd;

    /* Prompt for password if:
     * - password is "-" (explicit prompt request), or
     * - a username was given but password is empty
     *   (without username, we fall back to guest auth as "nobody") */
    if (strcmp(url.password, "-") == 0 ||
            (url.username[0] != '\0'
             && strcmp(url.username, "nobody") != 0
             && url.password[0] == '\0')) {
        passwd = getpass("Password:");
        strlcpy(url.password, passwd, AFP_MAX_PASSWORD_LEN);
    }

    return 0;
}

static int cmdline_get_volpass(void)
{
    char *volpass;
    volpass = getpass("Volume password:");

    if (volpass == NULL) {
        return -1;  /* Ctrl+C or error */
    }

    strlcpy(url.volpassword, volpass, sizeof(url.volpassword));
    /* Clear the getpass() static buffer up to the max we could have used */
    explicit_bzero(volpass, sizeof(url.volpassword) - 1);
    return 0;
}

static int attach_volume_with_password_prompt(volumeid_t *vol_id_ptr,
        unsigned int volume_options)
{
    enum afp_sl_attach_status status;
    int ret;
    /* Clear any previous password to force a fresh prompt if needed */
    explicit_bzero(url.volpassword, sizeof(url.volpassword));
    /* First attempt */
    ret = afp_sl_attach(&url, volume_options, vol_id_ptr, &status);

    if (ret == 0) {
        return 0;
    }

    if (status == AFP_SL_ATTACH_STATUS_PASSWORD_REQUIRED) {
        if (cmdline_get_volpass() != 0) {
            printf("Password prompt cancelled.\n");
            return -EACCES;
        }

        /* Second attempt with password */
        ret = afp_sl_attach(&url, volume_options, vol_id_ptr, &status);
    }

    return ret;
}


static int get_server_path(char * filename, char * server_fullname)
{
    int result;

    if (filename[0] != '/') {
        if (strcmp(curdir, "/") == 0) {
            result = snprintf(server_fullname, AFP_MAX_PATH, "/%s", filename);
        } else {
            result = snprintf(server_fullname, AFP_MAX_PATH, "%s/%s", curdir, filename);
        }

        if (result >= AFP_MAX_PATH || result < 0) {
            fprintf(stderr,
                    "Error: Path exceeds maximum length or other error occurred.\n");
            return -1;
        }
    } else {
        result = snprintf(server_fullname, AFP_MAX_PATH, "%s", filename);
    }

    if (result >= AFP_MAX_PATH || result < 0) {
        return -1;
    }

    return 0;
}

/**
 * Appends a basename to a directory path, adding a separator if needed.
 * Uses snprintf for efficient single-operation append.
 * Returns 0 on success, -1 if the result would exceed max_len.
 */
static int append_basename_to_path(char *path, const char *base, size_t max_len)
{
    size_t path_len, base_len;
    int need_slash;
    size_t space_needed;

    if (!path || !base) {
        return -1;
    }

    /* Validate that strings are null-terminated within max_len */
    path_len = strnlen(path, max_len);

    if (path_len >= max_len) {
        return -1;
    }

    base_len = strnlen(base, max_len);

    if (base_len >= max_len) {
        return -1;
    }

    need_slash = (path_len > 0 && path[path_len - 1] != '/') ? 1 : 0;
    space_needed = path_len + need_slash + base_len + 1;

    if (space_needed > max_len) {
        return -1;
    }

    if (need_slash) {
        snprintf(path + path_len, max_len - path_len, "/%s", base);
    } else {
        snprintf(path + path_len, max_len - path_len, "%s", base);
    }

    return 0;
}

static void metadata_warn(unsigned int warnings)
{
    if (warnings != AFP_METADATA_WARNING_NONE && !metadata_warning_emitted) {
        fprintf(stderr, "Warning: some metadata could not be represented by "
                        "the selected storage mode.\n");
        metadata_warning_emitted = 1;
    }
}

static int metadata_list_next(const char *list, size_t size, size_t *pos,
                              const char **name)
{
    size_t length;

    if (*pos >= size) {
        return 0;
    }

    length = strnlen(list + *pos, size - *pos);

    if (length == size - *pos) {
        return -EIO;
    }

    *name = list + *pos;
    *pos += length + 1U;
    return 1;
}

static int remote_xattr_list(const char *path, char **list, size_t *size)
{
    int ret = afp_sl_listxattr(&vol_id, path, NULL, 0);
    *list = NULL;
    *size = 0;

    if (ret <= 0) {
        return ret;
    }

    if ((size_t)ret > AFP_SL_XATTR_LIST_MAX) {
        return -E2BIG;
    }

    *list = malloc((size_t)ret);

    if (!*list) {
        return -ENOMEM;
    }

    int got = afp_sl_listxattr(&vol_id, path, *list, (size_t)ret);

    if (got < 0 || got > ret) {
        free(*list);
        *list = NULL;
        return got < 0 ? got : -EIO;
    }

    *size = (size_t)got;
    return 0;
}

static int remote_xattr_get(const char *path, const char *name,
                            void **value, size_t *size)
{
    int ret = afp_sl_getxattr(&vol_id, path, name, NULL, 0);
    *value = NULL;
    *size = 0;

    if (ret < 0) {
        return ret;
    }

    if (ret > AFP_SL_METADATA_CHUNK) {
        return -E2BIG;
    }

    *value = malloc(ret ? (size_t)ret : 1U);

    if (!*value) {
        return -ENOMEM;
    }

    if (ret > 0) {
        int got = afp_sl_getxattr(&vol_id, path, name, *value, (size_t)ret);

        if (got < 0 || got > ret) {
            free(*value);
            *value = NULL;
            return got < 0 ? got : -EIO;
        }

        ret = got;
    }

    *size = (size_t)ret;
    return 0;
}

static int remote_readdir_all(const char *path,
                              struct afp_file_info_basic **files,
                              unsigned int *count)
{
    struct afp_file_info_basic *all = NULL;
    size_t total = 0;
    int eod = 0;

    while (!eod) {
        struct afp_file_info_basic *page = NULL;
        unsigned int page_count = 0;

        if (total > INT_MAX) {
            free(all);
            return -EOVERFLOW;
        }

        int ret = afp_sl_readdir(&vol_id, path, NULL, (int)total, 256,
                                 &page_count, &page, &eod);

        if (ret != 0) {
            free(page);
            free(all);
            return ret;
        }

        if (page_count == 0) {
            free(page);
            break;
        }

        if ((size_t)page_count > SIZE_MAX / sizeof(*all) - total) {
            free(page);
            free(all);
            return -EOVERFLOW;
        }

        size_t new_total = total + (size_t)page_count;
        struct afp_file_info_basic *grown = realloc(all,
                                            new_total * sizeof(*all));

        if (!grown) {
            free(page);
            free(all);
            return -ENOMEM;
        }

        all = grown;
        memcpy(all + total, page, (size_t)page_count * sizeof(*all));
        total = new_total;
        free(page);
    }

    if (total > UINT_MAX) {
        free(all);
        return -EOVERFLOW;
    }

    *files = all;
    *count = (unsigned int)total;
    return 0;
}

static int apply_remote_posix_metadata(const char *path, const struct stat *st)
{
    int ret = afp_sl_chmod(&vol_id, path, NULL, st->st_mode & 07777);

    if (ret != 0 && ret != -ENOTSUP && ret != -EACCES) {
        return ret;
    }

    struct utimbuf times = { .actime = st->st_mtime, .modtime = st->st_mtime };

    ret = afp_sl_utime(&vol_id, path, NULL, &times);

    return ret == 0 || ret == -ENOTSUP || ret == -EACCES ? 0 : ret;
}

static int copy_local_metadata_to_remote(const char *local_path,
        const char *remote_path, const struct stat *st)
{
    unsigned int warnings = 0;

    if (transfer_metadata_mode == AFP_METADATA_NONE) {
        return 0;
    }

    int ret = afp_sl_metadata_copy_local_to_remote(local_path,
              transfer_metadata_mode, &vol_id, remote_path, &warnings);
    metadata_warn(warnings);

    if (ret < 0) {
        return ret;
    }

    return apply_remote_posix_metadata(remote_path, st);
}

static int copy_remote_metadata_to_local(const char *remote_path,
        const char *local_path, const struct stat *st)
{
    unsigned int warnings = 0;

    if (transfer_metadata_mode == AFP_METADATA_NONE) {
        return 0;
    }

    int ret = afp_sl_metadata_copy_remote_to_local(&vol_id, remote_path,
              local_path, transfer_metadata_mode, &warnings);
    metadata_warn(warnings);

    if (ret < 0) {
        return ret;
    }

    if (chmod(local_path, st->st_mode & 07777) < 0 && errno != EPERM) {
        return -errno;
    }

    struct timespec times[2] = {
        { .tv_sec = st->st_mtime, .tv_nsec = 0 },
        { .tv_sec = st->st_mtime, .tv_nsec = 0 },
    };

    return utimensat(AT_FDCWD, local_path, times, 0) < 0 ? -errno : 0;
}

static int copy_remote_metadata(const char *source, const char *target,
                                const struct stat *st)
{
    unsigned int warnings = 0;

    if (transfer_metadata_mode == AFP_METADATA_NONE) {
        return 0;
    }

    int ret = afp_sl_metadata_copy_remote_to_remote(&vol_id, source, &vol_id,
              target, &warnings);
    metadata_warn(warnings);

    if (ret < 0) {
        return ret;
    }

    return apply_remote_posix_metadata(target, st);
}

static void print_file_details_basic(struct afp_file_info_basic * p,
                                     int size_width)
{
    struct tm * mtime;
    time_t t;
#define DATE_LEN 32
    char datestr[DATE_LEN];
    char mode_str[11];
    uint32_t mode = p->unixprivs.permissions;
    snprintf(mode_str, sizeof(mode_str), "----------");
    t = p->modification_date;
    mtime = localtime(&t);

    if (S_ISDIR(mode)) {
        mode_str[0] = 'd';
    }

    if (mode & S_IRUSR) {
        mode_str[1] = 'r';
    }

    if (mode & S_IWUSR) {
        mode_str[2] = 'w';
    }

    if (mode & S_IXUSR) {
        mode_str[3] = 'x';
    }

    if (mode & S_IRGRP) {
        mode_str[4] = 'r';
    }

    if (mode & S_IWGRP) {
        mode_str[5] = 'w';
    }

    if (mode & S_IXGRP) {
        mode_str[6] = 'x';
    }

    if (mode & S_IROTH) {
        mode_str[7] = 'r';
    }

    if (mode & S_IWOTH) {
        mode_str[8] = 'w';
    }

    if (mode & S_IXOTH) {
        mode_str[9] = 'x';
    }

    strftime(datestr, DATE_LEN, "%F %H:%M", mtime);
    printf("%s %*lld %s %s\n", mode_str, size_width, p->size, datestr, p->name);
}

static void list_volumes(void)
{
    unsigned int numvols = 0;
    struct afp_volume_summary *vols;
    unsigned int count = 100; /* Reasonable limit for CLI listing */
    int ret;
    vols = malloc(sizeof(struct afp_volume_summary) * count);

    if (!vols) {
        printf("Out of memory\n");
        return;
    }

    ret = afp_sl_getvols(&url, 0, count, &numvols, vols);

    if (ret != 0 && is_recoverable_session_error(ret)
            && reconnect_session(0, 0) == 0) {
        numvols = 0;
        ret = afp_sl_getvols(&url, 0, count, &numvols, vols);
    }

    if (ret == 0) {
        printf("Available volumes on %s:\n", url.servername);

        for (unsigned int i = 0; i < numvols; i++) {
            printf("  %s\n", vols[i].volume_name_printable);
        }
    } else {
        printf("Could not list volumes\n");
    }

    free(vols);
}

int com_pass(char *unused _U_)
{
    const char *old_password;
    const char *new_password;
    const char *new_password_confirm;
    enum afp_sl_password_change_status status;
    int ret;

    if (!connected) {
        printf("You're not connected to a server.\n");
        return -1;
    }

    old_password = getpass("Old password: ");

    if (old_password == NULL || old_password[0] == '\0') {
        printf("Password change cancelled.\n");
        return -1;
    }

    /* stash old_password since getpass uses a static buffer */
    char old_pw_buf[AFP_MAX_PASSWORD_LEN];
    strlcpy(old_pw_buf, old_password, sizeof(old_pw_buf));
    new_password = getpass("New password: ");

    if (new_password == NULL || new_password[0] == '\0') {
        printf("Password change cancelled.\n");
        explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
        return -1;
    }

    /* stash new_password since getpass uses a static buffer */
    char new_pw_buf[AFP_MAX_PASSWORD_LEN];
    strlcpy(new_pw_buf, new_password, sizeof(new_pw_buf));
    new_password_confirm = getpass("Confirm new password: ");

    if (new_password_confirm == NULL) {
        printf("Password change cancelled.\n");
        explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
        explicit_bzero(new_pw_buf, sizeof(new_pw_buf));
        return -1;
    }

    if (strcmp(new_pw_buf, new_password_confirm) != 0) {
        printf("Passwords do not match.\n");
        explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
        explicit_bzero(new_pw_buf, sizeof(new_pw_buf));
        return -1;
    }

    ret = afp_sl_changepw(&url, old_pw_buf, new_pw_buf, &status);
    explicit_bzero(old_pw_buf, sizeof(old_pw_buf));
    explicit_bzero(new_pw_buf, sizeof(new_pw_buf));

    if (ret != 0) {
        switch (status) {
        case AFP_SL_PASSWORD_CHANGE_STATUS_ACCESS_DENIED:
            printf("Password change failed: access denied.\n");
            break;

        case AFP_SL_PASSWORD_CHANGE_STATUS_INCORRECT_OLD_PASSWORD:
            printf("Password change failed: incorrect old password.\n");
            break;

        case AFP_SL_PASSWORD_CHANGE_STATUS_UNSUPPORTED_AUTHENTICATION:
            printf("Password change failed: not supported by the current "
                   "authentication method.\n");
            break;

        case AFP_SL_PASSWORD_CHANGE_STATUS_UNCHANGED:
            printf("Password change failed: new password must be different "
                   "from old password.\n");
            break;

        case AFP_SL_PASSWORD_CHANGE_STATUS_TOO_SHORT:
            printf("Password change failed: new password is too short.\n");
            break;

        case AFP_SL_PASSWORD_CHANGE_STATUS_EXPIRED:
            printf("Password change failed: password has expired.\n");
            break;

        case AFP_SL_PASSWORD_CHANGE_STATUS_POLICY_VIOLATION:
            printf("Password change failed: new password does not meet "
                   "the server's password policy.\n");
            break;

        case AFP_SL_PASSWORD_CHANGE_STATUS_INVALID_PARAMETER:
            printf("Password change failed: invalid parameter.\n");
            break;

        default:
            printf("Password change failed with unknown error.\n");
            break;
        }

        return -1;
    }

    printf("Password changed successfully.\n");
    return 0;
}

int com_dir(char * arg)
{
    if (!arg) {
        arg = "";
    }

    struct afp_file_info_basic *filebase = NULL;

    unsigned int numfiles = 0;
    int eod = 0;
    int ret = -1;
    char path[AFP_MAX_PATH];
    char dir_path[AFP_MAX_PATH];

    if (!vol_id) {
        if (connected) {
            list_volumes();
            return 0;
        }

        printf("You're not connected to a server\n");
        goto out;
    }

    /* If an argument is provided, use it; otherwise use current directory */
    if (arg[0] != '\0') {
        if (escape_paths(path, NULL, arg)) {
            printf("Invalid path\n");
            goto out;
        }

        /* Handle "." as the current directory */
        if (strcmp(path, ".") == 0) {
            strlcpy(dir_path, curdir, AFP_MAX_PATH);
        } else {
            get_server_path(path, dir_path);
        }
    } else {
        strlcpy(dir_path, curdir, AFP_MAX_PATH);
    }

    ret = afp_sl_readdir(&vol_id, dir_path, NULL, 0, 100, &numfiles, &filebase,
                         &eod);

    if (ret != 0 && is_recoverable_session_error(ret)
            && reconnect_session(1, 1) == 0) {
        if (filebase) {
            free(filebase);
            filebase = NULL;
        }

        numfiles = 0;
        eod = 0;
        ret = afp_sl_readdir(&vol_id, dir_path, NULL, 0, 100, &numfiles,
                             &filebase, &eod);
    }

    if (ret != 0) {
        printf("Could not read directory\n");
        goto out;
    }

    if (numfiles == 0) {
        ret = 0;
        goto out;
    }

    /* Calculate max width for file size column */
    int max_width = 0;

    for (unsigned int i = 0; i < numfiles; i++) {
        char size_str[32];
        int width = snprintf(size_str, sizeof(size_str), "%lld", filebase[i].size);

        if (width > max_width) {
            max_width = width;
        }
    }

    for (unsigned int i = 0; i < numfiles; i++) {
        print_file_details_basic(&filebase[i], max_width);
    }

    ret = 0;
out:

    if (filebase) {
        free(filebase);
    }

    return ret;
}

int com_touch(char * arg)
{
    char filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    struct utimbuf times;
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(filename, NULL, arg)) {
        printf("expecting format: touch <filename>\n");
        return -1;
    }

    if (get_server_path(filename, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    ret = afp_sl_creat(&vol_id, server_fullname, NULL, 0644);

    if (ret == 0) {
        return 0;
    } else if (ret == -EEXIST) {
        time_t now = time(NULL);
        times.actime = now;
        times.modtime = now;
        ret = afp_sl_utime(&vol_id, server_fullname, NULL, &times);

        if (ret != 0) {
            printf("Could not update timestamp for %s (result=%d)\n", filename, ret);
            return -1;
        }

        return 0;
    }

    printf("Could not create file %s (result=%d)\n", filename, ret);
    return -1;
}

static int chmod_remote_tree(const char *server_path, mode_t mode)
{
    struct afp_file_info_basic *entries = NULL;
    unsigned int count = 0;
    int ret = 0;

    if (remote_readdir_all(server_path, &entries, &count) != 0) {
        printf("Could not read directory %s\n", server_path);
        return -1;
    }

    for (unsigned int i = 0; i < count; i++) {
        struct afp_file_info_basic *entry = &entries[i];
        char child[AFP_MAX_PATH];

        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
            continue;
        }

        int length = snprintf(child, sizeof(child), "%s/%s",
                              server_path, entry->name);

        if (length < 0 || (size_t)length >= sizeof(child)) {
            printf("Path too long: %s/%s\n", server_path, entry->name);
            ret = -1;
            continue;
        }

        if (S_ISLNK(entry->unixprivs.permissions)) {
            printf("Symlinks are not supported: %s\n", child);
            ret = -1;
        } else if (S_ISDIR(entry->unixprivs.permissions)
                   && chmod_remote_tree(child, mode) < 0) {
            ret = -1;
        } else if (!S_ISDIR(entry->unixprivs.permissions)
                   && afp_sl_chmod(&vol_id, child, NULL, mode)
                   != 0) {
            printf("Could not chmod %s\n", child);
            ret = -1;
        }
    }

    free(entries);

    if (afp_sl_chmod(&vol_id, server_path, NULL, mode)
            != 0) {
        printf("Could not chmod %s\n", server_path);
        ret = -1;
    }

    return ret;
}

int com_chmod(char * arg)
{
    char mode_str[AFP_MAX_PATH];
    char filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    mode_t mode;
    char *endptr;
    int ret;
    int recursive = command_recursive_option(&arg);

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(mode_str, filename, arg)) {
        printf("expecting format: chmod [-r] <mode> <filename>\n");
        return -1;
    }

    mode = (mode_t)strtol(mode_str, &endptr, 8);

    if (*endptr != '\0') {
        printf("Invalid mode: %s\n", mode_str);
        return -1;
    }

    if (get_server_path(filename, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    if (recursive) {
        struct stat st;

        if (afp_sl_stat(&vol_id, server_fullname, NULL, &st)
                != 0) {
            printf("File not found: %s\n", filename);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            return chmod_remote_tree(server_fullname, mode);
        }
    }

    ret = afp_sl_chmod(&vol_id, server_fullname, NULL, mode);

    if (ret != 0) {
        if (ret == -EACCES) {
            printf("Permission denied changing mode for %s\n", filename);
        } else if (ret == -ENOENT) {
            printf("File not found: %s\n", filename);
        } else {
            printf("Could not chmod %s (result=%d)\n", filename, ret);
        }

        return -1;
    }

    return 0;
}

static int upload_file(char *local_filename, char *server_fullname,
                       unsigned long long *bytes_transferred)
{
    int localfd = -1;
    unsigned int fileid = 0;
    struct stat localstat;
    unsigned long long offset = 0;
    unsigned long long total_written = 0;
#define PUT_BUFSIZE 102400
    char buf[PUT_BUFSIZE];
    ssize_t amount_read;
    unsigned int written;
    int ret = -1;
    struct timeval starttv, endtv;
    int file_opened = 0;
    localfd = open(local_filename, O_RDONLY);

    if (localfd < 0) {
        printf("Could not open local file \"%s\"\n", local_filename);
        perror("open");
        goto out;
    }

    if (fstat(localfd, &localstat) != 0) {
        printf("Could not get attributes for local file \"%s\"\n", local_filename);
        perror("fstat");
        goto out;
    }

    if (!S_ISREG(localstat.st_mode)) {
        printf("Not a regular file: %s\n", local_filename);
        goto out;
    }

    if (verbose_mode) {
        printf("    Uploading file %s to %s\n", local_filename, server_fullname);
    }

    gettimeofday(&starttv, NULL);
    /* Create remote file first (with permission bits only, not file type)
     * We mask with 0777 to ensure we only pass permission bits, not file type bits (like S_IFREG)
     * which could confuse the server or midlevel API */
    ret = afp_sl_creat(&vol_id, server_fullname, NULL, localstat.st_mode & 0777);

    if (ret != 0) {
        if (ret == -EEXIST) {
            if (verbose_mode) {
                printf("    File exists, truncating before overwriting...\n");
            }

            ret = afp_sl_truncate(&vol_id, server_fullname, NULL, 0);

            if (ret != 0) {
                printf("Could not truncate existing file \"%s\" (result=%d)\n", server_fullname,
                       ret);
                goto out;
            }
        } else if (ret == -EACCES) {
            /* Sometimes ACCESS is returned if file exists but is read-only.
               Try to truncate/overwrite anyway. */
            ret = afp_sl_truncate(&vol_id, server_fullname, NULL, 0);

            if (ret != 0) {
                printf("Permission denied creating file \"%s\"\n", server_fullname);
                goto out;
            }
        } else {
            printf("Could not create remote file \"%s\" (result=%d)\n", server_fullname,
                   ret);
            goto out;
        }
    }

    ret = afp_sl_open(&vol_id, server_fullname, NULL, &fileid, O_RDWR);

    if (ret) {
        if (ret == -EACCES) {
            printf("Permission denied opening file \"%s\"\n", server_fullname);
        } else if (ret == -ENOENT) {
            printf("Permission denied creating file \"%s\"\n", server_fullname);
        } else {
            printf("Could not open remote file for writing (result=%d)\n", ret);
        }

        /* If we created the file but failed to open it, try to remove it to avoid leaving
           a partial/inaccessible file and potentially confusing the server state */
        afp_sl_unlink(&vol_id, server_fullname, NULL);
        goto out;
    }

    file_opened = 1;

    /* Upload loop: read from local, write to remote */
    while ((amount_read = read(localfd, buf, PUT_BUFSIZE)) > 0) {
        int api_ret = afp_sl_write(&vol_id, fileid, 0, offset, amount_read, &written,
                                   buf);

        if (api_ret || written != (unsigned int)amount_read) {
            printf("Write error at offset %llu (wrote %u of %zd bytes)\n",
                   offset, written, amount_read);
            ret = -1;
            goto out;
        }

        offset += written;
        total_written += written;
    }

    if (amount_read < 0) {
        printf("Error reading local file\n");
        perror("read");
        goto out;
    }

    ret = afp_sl_close(&vol_id, fileid);
    file_opened = 0;
    fileid = 0;

    if (ret != 0) {
        printf("Could not close remote file \"%s\" (result=%d)\n",
               server_fullname, ret);
        goto out;
    }

    ret = copy_local_metadata_to_remote(local_filename, server_fullname,
                                        &localstat);

    if (ret < 0) {
        printf("Could not preserve metadata for \"%s\" (error=%d)\n",
               local_filename, ret);
        goto out;
    }

    gettimeofday(&endtv, NULL);

    if (verbose_mode) {
        unsigned long long elapsed_usec =
            (endtv.tv_sec - starttv.tv_sec) * 1000000ULL +
            (endtv.tv_usec - starttv.tv_usec);
        double elapsed_sec = elapsed_usec / 1000000.0;
        double rate_mbps = 0.0;

        if (elapsed_sec > 0.0) {
            rate_mbps = (total_written * 8.0) / (elapsed_sec * 1000000.0);
        }

        printf("    Transferred %llu bytes in %.2f seconds (%.2f Mbps)\n",
               total_written, elapsed_sec, rate_mbps);
    }

    if (bytes_transferred) {
        *bytes_transferred = total_written;
    }

    ret = 0;
out:

    if (file_opened && fileid) {
        afp_sl_close(&vol_id, fileid);
    }

    if (localfd >= 0) {
        close(localfd);
    }

    /* Note: We don't delete the partially created remote file on error */
    return ret;
}

static int upload_directory(char *local_dirname, char *server_parent_path,
                            unsigned long long *total_bytes)
{
    DIR *dir;
    struct dirent *entry;
    char local_path[PATH_MAX];
    char server_path[AFP_MAX_PATH];
    struct stat st;
    struct stat dir_stat;
    int ret = 0;
    unsigned long long bytes = 0;

    if (lstat(local_dirname, &dir_stat) != 0) {
        perror("lstat");
        return -1;
    }

    if (S_ISLNK(dir_stat.st_mode)) {
        printf("Symlinks are not supported: %s\n", local_dirname);
        return -1;
    }

    ret = afp_sl_mkdir(&vol_id, server_parent_path, NULL,
                       dir_stat.st_mode & 0777);

    if (ret != 0 && ret != -EEXIST) {
        printf("Failed to create remote directory %s (error: %d)\n", server_parent_path,
               ret);

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    /* An existing destination directory is a successful merge target. */
    ret = 0;
    dir = opendir(local_dirname);

    if (!dir) {
        perror("opendir");

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (metadata_sidecar_entry(entry->d_name)) {
            continue;
        }

        snprintf(local_path, sizeof(local_path), "%s/%s", local_dirname, entry->d_name);
        snprintf(server_path, sizeof(server_path), "%s/%s", server_parent_path,
                 entry->d_name);

        if (lstat(local_path, &st) != 0) {
            perror("lstat");
            ret = -1;
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            printf("Symlinks are not supported: %s\n", local_path);
            ret = -1;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            unsigned long long subdir_bytes = 0;

            if (upload_directory(local_path, server_path, &subdir_bytes) < 0) {
                ret = -1;
            } else {
                bytes += subdir_bytes;
            }
        } else if (S_ISREG(st.st_mode)) {
            unsigned long long file_bytes = 0;

            if (upload_file(local_path, server_path, &file_bytes) < 0) {
                ret = -1;
            } else {
                bytes += file_bytes;
            }
        }
    }

    closedir(dir);

    if (copy_local_metadata_to_remote(local_dirname, server_parent_path,
                                      &dir_stat) < 0) {
        printf("Could not preserve directory metadata for %s\n", local_dirname);
        ret = -1;
    }

    if (total_bytes) {
        *total_bytes = bytes;
    }

    return ret;
}

int com_put(char *arg)
{
    char local_filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    char *basename_ptr;
    struct stat st;
    int recursive = command_recursive_option(&arg);
    unsigned long long bytes_transferred = 0;
    int ret = -1;
    metadata_warning_emitted = 0;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if (escape_paths(local_filename, NULL, arg)) {
        printf("expecting format: put [-r] <filename>\n");
        goto error;
    }

    if (lstat(local_filename, &st) != 0) {
        perror("lstat");
        goto error;
    }

    if (S_ISLNK(st.st_mode)) {
        printf("Symlinks are not supported: %s\n", local_filename);
        goto error;
    }

    basename_ptr = basename(local_filename);
    get_server_path(basename_ptr, server_fullname);

    if (S_ISDIR(st.st_mode)) {
        if (recursive) {
            ret = upload_directory(local_filename, server_fullname, &bytes_transferred);
            goto out;
        } else {
            printf("%s is a directory (use put -r to upload recursively)\n",
                   local_filename);
            goto error;
        }
    }

    ret = upload_file(local_filename, server_fullname, &bytes_transferred);
out:

    if (bytes_transferred > 0) {
        printf("Transfer complete. %llu bytes sent.\n", bytes_transferred);
    }

error:
    return ret;
}

static int retrieve_file(char * arg, int fd, struct stat *stat,
                         unsigned long long *amount_written)
{
    unsigned int fileid = 0;
    int file_opened = 0;
    char path[PATH_MAX];
    unsigned long long offset = 0;
#define BUF_SIZE 102400
    unsigned int size = BUF_SIZE;
    char buf[BUF_SIZE];
    unsigned int received, eof = 0;
    unsigned long long total = 0;
    struct timeval starttv, endtv;
    int ret = -1;
    *amount_written = 0;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto out;
    }

    get_server_path(arg, path);
    gettimeofday(&starttv, NULL);

    if (afp_sl_stat(&vol_id, path, NULL, stat)) {
        printf("Could not get file attributes for file %s\n", path);
        goto out;
    }

    if (afp_sl_open(&vol_id, path, NULL, &fileid, O_RDONLY)) {
        printf("Could not open %s on server\n", arg);
        goto out;
    }

    file_opened = 1;

    /* Read file in chunks */
    while (!eof) {
        memset(buf, 0, BUF_SIZE);
        int api_ret = afp_sl_read(&vol_id, fileid, 0, offset, size, &received, &eof,
                                  buf);

        if (api_ret) {
            printf("Error reading file\n");
            ret = -1;
            goto out;
        }

        if (received == 0) {
            break;
        }

        if (write_all_fd(fd, buf, received) < 0) {
            printf("Error writing local file\n");
            ret = -1;
            goto out;
        }

        total += received;
        offset += received;
    }

    if (verbose_mode) {
        gettimeofday(&endtv, NULL);
        printdiff(&starttv, &endtv, &total);
    }

    *amount_written = total;
    ret = 0;
out:

    /* Do not close fd here, caller owns it */
    if (file_opened && fileid) {
        afp_sl_close(&vol_id, fileid);
    }

    return ret;
}

static int download_directory(char *server_path, char *local_path,
                              unsigned long long *total_bytes)
{
    struct afp_file_info_basic *filebase = NULL;
    unsigned int numfiles = 0;
    int ret = 0;
    char new_server_path[AFP_MAX_PATH];
    char new_local_path[PATH_MAX];
    struct stat st;
    struct stat dir_stat;
    unsigned long long bytes = 0;

    if (afp_sl_stat(&vol_id, server_path, NULL, &dir_stat) != 0) {
        printf("Could not stat directory %s\n", server_path);
        return -1;
    }

    if (mkdir(local_path, dir_stat.st_mode & 0777) < 0 && errno != EEXIST) {
        perror("mkdir");

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    if (remote_readdir_all(server_path, &filebase, &numfiles) != 0) {
        printf("Could not read directory %s\n", server_path);

        if (total_bytes) {
            *total_bytes = 0;
        }

        return -1;
    }

    for (unsigned int i = 0; i < numfiles; i++) {
        struct afp_file_info_basic *p = &filebase[i];

        if (strcmp(p->name, ".") == 0 || strcmp(p->name, "..") == 0) {
            continue;
        }

        int path_len = snprintf(new_server_path, sizeof(new_server_path), "%s/%s",
                                server_path,
                                p->name);

        if (path_len < 0 || (size_t)path_len >= sizeof(new_server_path)) {
            printf("Path too long: %s/%s\n", server_path, p->name);
            continue;
        }

        path_len = snprintf(new_local_path, sizeof(new_local_path), "%s/%s", local_path,
                            p->name);

        if (path_len < 0 || (size_t)path_len >= sizeof(new_local_path)) {
            printf("Local path too long: %s/%s\n", local_path, p->name);
            continue;
        }

        if (S_ISDIR(p->unixprivs.permissions)) {
            unsigned long long subdir_bytes = 0;

            if (download_directory(new_server_path, new_local_path, &subdir_bytes) < 0) {
                ret = -1;
            } else {
                bytes += subdir_bytes;
            }
        } else if (S_ISLNK(p->unixprivs.permissions)) {
            printf("Symlinks are not supported: %s\n", new_server_path);
            ret = -1;
        } else {
            int fd = open(new_local_path, O_CREAT | O_TRUNC | O_RDWR, 0644);

            if (fd < 0) {
                perror("open");
                ret = -1;
                continue;
            }

            /* Construct a stat struct for retrieve_file */
            memset(&st, 0, sizeof(st));
            st.st_mode = p->unixprivs.permissions;
            st.st_size = p->size;
            st.st_uid = p->unixprivs.uid;
            st.st_gid = p->unixprivs.gid;
            st.st_mtime = p->modification_date;
            unsigned long long amount = 0;

            if (verbose_mode) {
                printf("    Downloading file %s\n", p->name);
            }

            if (retrieve_file(new_server_path, fd, &st, &amount) < 0) {
                ret = -1;
            } else {
                bytes += amount;
            }

            close(fd);

            if (copy_remote_metadata_to_local(new_server_path, new_local_path,
                                              &st) < 0) {
                printf("Could not preserve metadata for %s\n", new_server_path);
                ret = -1;
            }
        }
    }

    if (filebase) {
        free(filebase);
    }

    if (copy_remote_metadata_to_local(server_path, local_path, &dir_stat) < 0) {
        printf("Could not preserve directory metadata for %s\n", server_path);
        ret = -1;
    }

    if (total_bytes) {
        *total_bytes = bytes;
    }

    return ret;
}

static int com_get_file(char * arg, unsigned long long *total)
{
    int fd;
    struct stat stat;
    char *localfilename;
    char filename[AFP_MAX_PATH];
    char getattr_path[AFP_MAX_PATH];

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if ((escape_paths(filename, NULL, arg))) {
        printf("expecting format: get <filename>\n");
        goto error;
    }

    localfilename = basename(filename);

    if (verbose_mode) {
        printf("    Downloading file %s\n", filename);
    }

    get_server_path(filename, getattr_path);

    if (afp_sl_stat(&vol_id, getattr_path, NULL, &stat)) {
        printf("Could not get attributes for file \"%s\"\n", filename);
        goto error;
    }

    fd = open(localfilename, O_CREAT | O_TRUNC | O_RDWR, stat.st_mode);

    if (fd < 0) {
        printf("Failed to open \"%s\" for writing\n", localfilename);
        perror("Opening local file");
        goto error;
    }

    if (fchmod(fd, stat.st_mode) < 0) {
        perror("Setting file mode");
        /* Non-fatal error, continue */
    }

    if (retrieve_file(filename, fd, &stat, total) < 0) {
        close(fd);
        goto error;
    }

    close(fd);

    if (copy_remote_metadata_to_local(getattr_path, localfilename, &stat) < 0) {
        printf("Could not preserve metadata for %s\n", filename);
        goto error;
    }

    return 0;
error:
    return -1;
}

int com_get(char *arg)
{
    unsigned long long amount_written = 0;
    char filename[AFP_MAX_PATH];
    char server_path[AFP_MAX_PATH];
    struct stat st;
    int recursive = command_recursive_option(&arg);
    int ret = -1;
    metadata_warning_emitted = 0;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if (escape_paths(filename, NULL, arg)) {
        printf("expecting format: get [-r] <filename>\n");
        goto error;
    }

    get_server_path(filename, server_path);

    if (afp_sl_stat(&vol_id, server_path, NULL, &st) != 0) {
        printf("File not found: %s\n", filename);
        goto error;
    }

    if (S_ISDIR(st.st_mode)) {
        if (recursive) {
            char *local_name = basename(filename);
            ret = download_directory(server_path, local_name, &amount_written);
            goto out;
        } else {
            printf("%s is a directory (use get -r to download recursively)\n",
                   filename);
            goto error;
        }
    }

    if (S_ISLNK(st.st_mode)) {
        printf("Symlinks are not supported: %s\n", filename);
        goto error;
    }

    ret = com_get_file(arg, &amount_written);
out:

    if (amount_written > 0) {
        printf("Transfer complete. %llu bytes received.\n", amount_written);
    }

error:
    return ret;
}


int com_view(char * arg)
{
    unsigned long long amount_written;
    char filename[AFP_MAX_PATH];
    struct stat stat;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto error;
    }

    if ((escape_paths(filename, NULL, arg))) {
        printf("expecting format: cat <filename>\n");
        goto error;
    }

    retrieve_file(filename, fileno(stdout), &stat, &amount_written);
    printf("\n");
    return 0;
error:
    return -1;
}

int com_rename(char * arg)
{
    char oldpath[AFP_MAX_PATH];
    char newpath[AFP_MAX_PATH];
    char server_oldpath[AFP_MAX_PATH];
    char server_newpath[AFP_MAX_PATH];
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(oldpath, newpath, arg)) {
        printf("expecting format: mv <oldname> <newname>\n");
        return -1;
    }

    if (get_server_path(oldpath, server_oldpath) < 0) {
        printf("Invalid old path\n");
        return -1;
    }

    if (get_server_path(newpath, server_newpath) < 0) {
        printf("Invalid new path\n");
        return -1;
    }

    ret = afp_sl_rename(&vol_id, server_oldpath, server_newpath, NULL);

    if (ret != 0) {
        printf("Failed to move %s to %s (error: %d)\n",
               server_oldpath, server_newpath, ret);
        return -1;
    }

    /* Check if target is a directory for display purposes */
    struct stat st;

    if (afp_sl_stat(&vol_id, server_newpath, NULL, &st) == 0
            && S_ISDIR(st.st_mode)) {
        char oldpath_copy[AFP_MAX_PATH];
        strlcpy(oldpath_copy, oldpath, AFP_MAX_PATH);
        const char *base = basename(oldpath_copy);
        append_basename_to_path(newpath, base, AFP_MAX_PATH);
        /* Silent failure is OK here for display purposes */
    }

    return 0;
}

static int parse_command_words(const char *input,
                               char words[][AFP_MAX_PATH], int max_words)
{
    int count = 0;
    const char *p = input;

    while (*p) {
        int quoted = 0;
        size_t length = 0;

        while (isspace((unsigned char) * p)) {
            p++;
        }

        if (!*p) {
            break;
        }

        if (count == max_words) {
            return -E2BIG;
        }

        while (*p && (quoted || !isspace((unsigned char) * p))) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }

            if (*p == '\\' && p[1] != '\0') {
                p++;
            }

            if (length + 1 >= AFP_MAX_PATH) {
                return -ENAMETOOLONG;
            }

            words[count][length++] = *p++;
        }

        if (quoted) {
            return -EINVAL;
        }

        words[count][length] = '\0';
        count++;
    }

    return count;
}

static int write_all_fd(int fd, const void *data, size_t size)
{
    const unsigned char *p = data;
    size_t written = 0;

    while (written < size) {
        ssize_t amount = write(fd, p + written, size - written);

        if (amount < 0) {
            return -errno;
        }

        if (amount == 0) {
            return -EIO;
        }

        written += (size_t)amount;
    }

    return 0;
}

static int read_binary_file(const char *path, void **data, size_t *size,
                            size_t maximum)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    int ret = 0;

    if (fd < 0) {
        return -errno;
    }

    if (fstat(fd, &st) < 0) {
        ret = -errno;
        goto out;
    }

    if (!S_ISREG(st.st_mode) || st.st_size < 0
            || (uintmax_t)st.st_size > maximum) {
        ret = -EFBIG;
        goto out;
    }

    *size = (size_t)st.st_size;
    *data = malloc(*size ? *size : 1);

    if (!*data) {
        ret = -ENOMEM;
        goto out;
    }

    size_t offset = 0;

    while (offset < *size) {
        ssize_t amount = read(fd, (unsigned char *)*data + offset, *size - offset);

        if (amount < 0) {
            ret = -errno;
            free(*data);
            *data = NULL;
            goto out;
        }

        if (amount == 0) {
            ret = -EIO;
            free(*data);
            *data = NULL;
            goto out;
        }

        offset += (size_t)amount;
    }

out:
    close(fd);
    return ret;
}

static int write_binary_file(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, METADATA_OUTPUT_MODE);
    int ret;

    if (fd < 0) {
        return -errno;
    }

    ret = write_all_fd(fd, data, size);

    if (close(fd) < 0 && ret == 0) {
        ret = -errno;
    }

    return ret;
}

static void print_bounded_hex(const void *data, size_t shown, size_t total)
{
    const unsigned char *bytes = data;

    for (size_t i = 0; i < shown; i++) {
        printf("%02x%s", bytes[i], (i + 1) % 16 == 0 ? "\n" : " ");
    }

    if (shown % 16 != 0) {
        printf("\n");
    }

    if (shown < total) {
        printf("... (%zu of %zu bytes shown)\n", shown, total);
    } else {
        printf("%zu bytes\n", total);
    }
}

static int metadata_command_error(const char *operation, int ret)
{
    if (ret < 0) {
        printf("%s failed: %s (%d)\n", operation, strerror(-ret), ret);
    } else {
        printf("%s failed (result=%d)\n", operation, ret);
    }

    return -1;
}

int com_xattr(char *arg)
{
    char words[4][AFP_MAX_PATH] = {{0}};
    char path[AFP_MAX_PATH];
    int argc = parse_command_words(arg, words, 4);
    int ret;

    if (!vol_id) {
        return metadata_command_error("xattr", -ENODEV);
    }

    if (argc < 2 || get_server_path(words[1], path) < 0) {
        printf("usage: xattr list PATH | get PATH NAME [OUTPUT] | "
               "set PATH NAME INPUT | remove PATH NAME\n");
        return -1;
    }

    if (strcmp(words[0], "list") == 0 && argc == 2) {
        char *list = NULL;
        size_t size = 0;
        size_t pos = 0;
        const char *name;
        int next;
        ret = remote_xattr_list(path, &list, &size);

        if (ret < 0) {
            return metadata_command_error("xattr list", ret);
        }

        while ((next = metadata_list_next(list, size, &pos, &name)) > 0) {
            printf("%s\n", name);
        }

        free(list);
        return next < 0 ? metadata_command_error("xattr list", next) : 0;
    }

    if (strcmp(words[0], "get") == 0 && (argc == 3 || argc == 4)) {
        void *value = NULL;
        size_t size = 0;
        ret = remote_xattr_get(path, words[2], &value, &size);

        if (ret < 0) {
            return metadata_command_error("xattr get", ret);
        }

        if (argc == 4) {
            ret = write_binary_file(words[3], value, size);
        } else {
            print_bounded_hex(value, size > 256 ? 256 : size, size);
        }

        free(value);
        return ret < 0 ? metadata_command_error("write output", ret) : 0;
    }

    if (strcmp(words[0], "set") == 0 && argc == 4) {
        void *value = NULL;
        size_t size = 0;
        ret = read_binary_file(words[3], &value, &size, AFP_SL_METADATA_CHUNK);

        if (ret == 0) {
            ret = afp_sl_setxattr(&vol_id, path, words[2], value, size, 0);
        }

        free(value);
        return ret < 0 ? metadata_command_error("xattr set", ret) : 0;
    }

    if (strcmp(words[0], "remove") == 0 && argc == 3) {
        ret = afp_sl_removexattr(&vol_id, path, words[2]);
        return ret < 0 ? metadata_command_error("xattr remove", ret) : 0;
    }

    printf("usage: xattr list PATH | get PATH NAME [OUTPUT] | "
           "set PATH NAME INPUT | remove PATH NAME\n");
    return -1;
}

int com_finderinfo(char *arg)
{
    char words[3][AFP_MAX_PATH] = {{0}};
    char path[AFP_MAX_PATH];
    unsigned char value[32];
    int argc = parse_command_words(arg, words, 3);
    int ret;

    if (!vol_id) {
        return metadata_command_error("finderinfo", -ENODEV);
    }

    if (argc < 2 || get_server_path(words[1], path) < 0) {
        goto usage;
    }

    if (strcmp(words[0], "get") == 0 && (argc == 2 || argc == 3)) {
        ret = afp_sl_getfinderinfo(&vol_id, path, value, sizeof(value));

        if (ret < 0) {
            return metadata_command_error("finderinfo get", ret);
        }

        if (argc == 3) {
            ret = write_binary_file(words[2], value, sizeof(value));
        } else {
            print_bounded_hex(value, sizeof(value), sizeof(value));
        }

        return ret < 0 ? metadata_command_error("write output", ret) : 0;
    }

    if (strcmp(words[0], "set") == 0 && argc == 3) {
        void *input = NULL;
        size_t size = 0;
        ret = read_binary_file(words[2], &input, &size, sizeof(value));

        if (ret == 0 && size != sizeof(value)) {
            ret = -EINVAL;
        }

        if (ret == 0) {
            ret = afp_sl_setfinderinfo(&vol_id, path, input, size);
        }

        free(input);
        return ret < 0 ? metadata_command_error("finderinfo set", ret) : 0;
    }

    if (strcmp(words[0], "remove") == 0 && argc == 2) {
        ret = afp_sl_removefinderinfo(&vol_id, path);
        return ret < 0 ? metadata_command_error("finderinfo remove", ret) : 0;
    }

usage:
    printf("usage: finderinfo get PATH [OUTPUT] | set PATH INPUT | remove PATH\n");
    return -1;
}

int com_resourcefork(char *arg)
{
    char words[3][AFP_MAX_PATH] = {{0}};
    char path[AFP_MAX_PATH];
    unsigned char buffer[AFP_SL_METADATA_CHUNK];
    int argc = parse_command_words(arg, words, 3);
    int ret;

    if (!vol_id) {
        return metadata_command_error("resourcefork", -ENODEV);
    }

    if (argc < 2 || get_server_path(words[1], path) < 0) {
        goto usage;
    }

    if (strcmp(words[0], "get") == 0 && (argc == 2 || argc == 3)) {
        ret = afp_sl_getresourcefork(&vol_id, path, NULL, 0, 0);

        if (ret < 0) {
            return metadata_command_error("resourcefork get", ret);
        }

        size_t total = (size_t)ret;
        size_t offset = 0;
        int fd = -1;

        if (argc == 3) {
            fd = open(words[2], O_WRONLY | O_CREAT | O_TRUNC,
                      METADATA_OUTPUT_MODE);

            if (fd < 0) {
                return metadata_command_error("open output", -errno);
            }
        }

        size_t limit;

        if (fd >= 0) {
            limit = total;
        } else if (total > 256) {
            limit = 256;
        } else {
            limit = total;
        }

        if (limit == 0 && fd < 0) {
            print_bounded_hex(buffer, 0, 0);
        }

        while (offset < limit) {
            size_t chunk = limit - offset;

            if (chunk > sizeof(buffer)) {
                chunk = sizeof(buffer);
            }

            ret = afp_sl_getresourcefork(&vol_id, path, buffer, chunk, offset);

            if (ret <= 0) {
                if (fd >= 0) {
                    close(fd);
                }

                return metadata_command_error("resourcefork get", ret < 0 ? ret : -EIO);
            }

            size_t amount = (size_t)ret;

            if (fd >= 0) {
                ret = write_all_fd(fd, buffer, amount);

                if (ret < 0) {
                    close(fd);
                    return metadata_command_error("write output", ret);
                }
            } else {
                print_bounded_hex(buffer, amount, total);
            }

            offset += amount;
        }

        if (fd >= 0 && close(fd) < 0) {
            return metadata_command_error("close output", -errno);
        }

        return 0;
    }

    if (strcmp(words[0], "set") == 0 && argc == 3) {
        int fd = open(words[2], O_RDONLY);
        unsigned long long offset = 0;

        if (fd < 0) {
            return metadata_command_error("open input", -errno);
        }

        while (1) {
            ssize_t amount = read(fd, buffer, sizeof(buffer));

            if (amount < 0) {
                ret = -errno;
                break;
            }

            if (amount == 0) {
                ret = 0;
                break;
            }

            ret = afp_sl_setresourcefork(&vol_id, path, buffer,
                                         (size_t)amount, offset);

            if (ret < 0) {
                break;
            }

            offset += (unsigned long long)amount;
        }

        if (ret >= 0) {
            ret = afp_sl_truncateresourcefork(&vol_id, path, offset);
        }

        close(fd);
        return ret < 0 ? metadata_command_error("resourcefork set", ret) : 0;
    }

    if (strcmp(words[0], "remove") == 0 && argc == 2) {
        ret = afp_sl_removeresourcefork(&vol_id, path);
        return ret < 0 ? metadata_command_error("resourcefork remove", ret) : 0;
    }

usage:
    printf("usage: resourcefork get PATH [OUTPUT] | set PATH INPUT | remove PATH\n");
    return -1;
}

static int quote_copy_argument(char *output, size_t output_size,
                               const char *path)
{
    size_t used = 0;

    if (output_size < 3) {
        return -1;
    }

    output[used++] = '"';

    for (const char *p = path; *p; p++) {
        if ((*p == '"' || *p == '\\') && used + 1 >= output_size) {
            return -1;
        }

        if (*p == '"' || *p == '\\') {
            output[used++] = '\\';
        }

        if (used + 1 >= output_size) {
            return -1;
        }

        output[used++] = *p;
    }

    if (used + 2 > output_size) {
        return -1;
    }

    output[used++] = '"';
    output[used] = '\0';
    return 0;
}

static int copy_remote_tree(const char *source, const char *target,
                            const struct stat *source_stat)
{
    struct afp_file_info_basic *entries = NULL;
    unsigned int count = 0;
    int ret = 0;
    int mkdir_ret = afp_sl_mkdir(&vol_id, target, NULL,
                                 source_stat->st_mode & 0777);

    if (mkdir_ret != 0 && mkdir_ret != -EEXIST) {
        printf("Could not create directory %s (error: %d)\n", target, mkdir_ret);
        return -1;
    }

    if (mkdir_ret == -EEXIST) {
        struct stat target_stat;
        int stat_ret = afp_sl_stat(&vol_id, target, NULL, &target_stat);

        if (stat_ret != 0) {
            printf("Could not stat existing copy target %s (error: %d)\n",
                   target, stat_ret);
            return -1;
        }

        if (!S_ISDIR(target_stat.st_mode)) {
            printf("Copy target exists and is not a directory: %s\n", target);
            return -1;
        }
    }

    if (remote_readdir_all(source, &entries, &count) != 0) {
        printf("Could not read directory %s\n", source);
        return -1;
    }

    for (unsigned int i = 0; i < count; i++) {
        struct afp_file_info_basic *entry = &entries[i];
        char child_source[AFP_MAX_PATH];
        char child_target[AFP_MAX_PATH];

        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
            continue;
        }

        int source_len = snprintf(child_source, sizeof(child_source), "%s/%s",
                                  source, entry->name);
        int target_len = snprintf(child_target, sizeof(child_target), "%s/%s",
                                  target, entry->name);

        if (source_len < 0 || (size_t)source_len >= sizeof(child_source)
                || target_len < 0 || (size_t)target_len >= sizeof(child_target)) {
            printf("Path too long while copying %s\n", entry->name);
            ret = -1;
            continue;
        }

        if (S_ISLNK(entry->unixprivs.permissions)) {
            printf("Symlinks are not supported: %s\n", child_source);
            ret = -1;
        } else if (S_ISDIR(entry->unixprivs.permissions)) {
            struct stat child_stat;

            if (afp_sl_stat(&vol_id, child_source, NULL, &child_stat) != 0
                    || copy_remote_tree(child_source, child_target,
                                        &child_stat) < 0) {
                ret = -1;
            }
        } else {
            char quoted_source[2 * AFP_MAX_PATH + 3];
            char quoted_target[2 * AFP_MAX_PATH + 3];
            char arguments[4 * AFP_MAX_PATH + 7];

            if (quote_copy_argument(quoted_source, sizeof(quoted_source),
                                    child_source) < 0
                    || quote_copy_argument(quoted_target, sizeof(quoted_target),
                                           child_target) < 0) {
                ret = -1;
                continue;
            }

            int argument_len = snprintf(arguments, sizeof(arguments), "%s %s",
                                        quoted_source, quoted_target);

            if (argument_len < 0
                    || (size_t)argument_len >= sizeof(arguments)) {
                printf("Could not format copy arguments for %s\n", child_source);
                ret = -1;
                continue;
            }

            if (com_copy(arguments) < 0) {
                ret = -1;
            }
        }
    }

    free(entries);

    if (copy_remote_metadata(source, target, source_stat) < 0) {
        printf("Could not preserve copied metadata for %s\n", source);
        ret = -1;
    }

    return ret;
}

static int path_is_same_or_descendant(const char *parent, const char *candidate)
{
    size_t parent_len = strnlen(parent, AFP_MAX_PATH);

    if (parent_len == AFP_MAX_PATH) {
        return 0;
    }

    /* Ignore trailing separators when finding the component boundary. */
    while (parent_len > 1 && parent[parent_len - 1] == '/') {
        parent_len--;
    }

    if (parent_len == 1 && parent[0] == '/') {
        return candidate[0] == '/';
    }

    return strncmp(parent, candidate, parent_len) == 0
           && (candidate[parent_len] == '\0' || candidate[parent_len] == '/');
}

int com_copy(char * arg)
{
    char source_path[AFP_MAX_PATH];
    char target_path[AFP_MAX_PATH];
    char server_source[AFP_MAX_PATH];
    char server_target[AFP_MAX_PATH];
    struct stat source_stat;
    struct stat target_stat;
    unsigned int source_fid = 0, target_fid = 0;
    int ret = -1;
    unsigned long long offset = 0;
    unsigned int received, written;
    unsigned int eof = 0;
    int recursive = command_recursive_option(&arg);
#define COPY_BUFSIZE 102400
    char buf[COPY_BUFSIZE];
    metadata_warning_emitted = 0;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        goto out;
    }

    if (escape_paths(source_path, target_path, arg)) {
        printf("expecting format: cp [-r] <source> <target>\n");
        goto out;
    }

    if (get_server_path(source_path, server_source) < 0) {
        printf("Invalid source path\n");
        goto out;
    }

    if (get_server_path(target_path, server_target) < 0) {
        printf("Invalid target path\n");
        goto out;
    }

    if (afp_sl_stat(&vol_id, server_source, NULL, &source_stat)) {
        printf("Could not stat source file: %s\n", source_path);
        goto out;
    }

    if (S_ISDIR(source_stat.st_mode)) {
        if (!recursive) {
            printf("Source is a directory (use cp -r to copy recursively)\n");
            goto out;
        }

        int target_stat_ret = afp_sl_stat(&vol_id, server_target, NULL,
                                          &target_stat);

        if (target_stat_ret == 0) {
            if (!S_ISDIR(target_stat.st_mode)) {
                printf("Target exists and is not a directory: %s\n", target_path);
                goto out;
            }

            const char *base = basename(source_path);

            if (append_basename_to_path(server_target, base, AFP_MAX_PATH) < 0) {
                printf("Target path too long\n");
                goto out;
            }
        }

        if (path_is_same_or_descendant(server_source, server_target)) {
            printf("Cannot copy a directory into itself\n");
            goto out;
        }

        ret = copy_remote_tree(server_source, server_target, &source_stat);
        goto out;
    }

    if (S_ISLNK(source_stat.st_mode)) {
        printf("Symlinks are not supported: %s\n", source_path);
        goto out;
    }

    if (afp_sl_stat(&vol_id, server_target, NULL,
                    &target_stat) == 0 && S_ISDIR(target_stat.st_mode)) {
        const char *base = basename(source_path);

        if (append_basename_to_path(server_target, base, AFP_MAX_PATH) < 0) {
            printf("Target path too long\n");
            goto out;
        }
    }

    if (afp_sl_open(&vol_id, server_source, NULL, &source_fid, O_RDONLY)) {
        printf("Could not open source file: %s\n", source_path);
        goto out;
    }

    ret = afp_sl_creat(&vol_id, server_target, NULL, source_stat.st_mode & 0777);

    if (ret != 0) {
        if (ret == -EEXIST) {
            if (afp_sl_truncate(&vol_id, server_target, NULL, 0)) {
                printf("Could not truncate target file\n");
                goto out;
            }
        } else if (ret == -EACCES) {
            printf("Permission denied creating target file\n");
            goto out;
        } else {
            printf("Could not create target file: %d\n", ret);
            goto out;
        }
    }

    if (afp_sl_open(&vol_id, server_target, NULL, &target_fid, O_RDWR)) {
        printf("Could not open target file for writing\n");
        goto out;
    }

    while (!eof) {
        int api_ret = afp_sl_read(&vol_id, source_fid, 0, offset, COPY_BUFSIZE,
                                  &received, &eof, buf);

        if (api_ret != 0) {
            printf("Error reading from source\n");
            ret = -1;
            goto out;
        }

        if (received > 0) {
            api_ret = afp_sl_write(&vol_id, target_fid, 0, offset, received, &written, buf);

            if (api_ret != 0 || written != received) {
                printf("Error writing to target\n");
                ret = -1;
                goto out;
            }

            offset += received;
        }
    }

    int source_close = afp_sl_close(&vol_id, source_fid);
    int target_close = afp_sl_close(&vol_id, target_fid);

    if (source_close != 0 || target_close != 0) {
        source_fid = 0;
        target_fid = 0;
        printf("Could not close files after copy\n");
        goto out;
    }

    source_fid = 0;
    target_fid = 0;
    ret = copy_remote_metadata(server_source, server_target, &source_stat);

    if (ret < 0) {
        printf("Could not preserve copied metadata (error=%d)\n", ret);
        goto out;
    }

    ret = 0;
    printf("Copied %llu bytes\n", offset);
out:

    if (source_fid) {
        afp_sl_close(&vol_id, source_fid);
    }

    if (target_fid) {
        afp_sl_close(&vol_id, target_fid);
    }

    return ret;
}

static int delete_directory(char *server_path)
{
    struct afp_file_info_basic *filebase = NULL;
    unsigned int numfiles = 0;
    int eod = 0;
    int ret = 0;
    char new_server_path[AFP_MAX_PATH];

    if (afp_sl_readdir(&vol_id, server_path, NULL, 0, 1000, &numfiles, &filebase,
                       &eod)) {
        printf("Could not read directory %s\n", server_path);
        return -1;
    }

    for (unsigned int i = 0; i < numfiles; i++) {
        struct afp_file_info_basic *p = &filebase[i];

        if (strcmp(p->name, ".") == 0 || strcmp(p->name, "..") == 0) {
            continue;
        }

        int path_len = snprintf(new_server_path, sizeof(new_server_path), "%s/%s",
                                server_path,
                                p->name);

        if (path_len < 0 || (size_t)path_len >= sizeof(new_server_path)) {
            printf("Path too long: %s/%s\n", server_path, p->name);
            continue;
        }

        if (S_ISDIR(p->unixprivs.permissions)) {
            if (delete_directory(new_server_path) < 0) {
                ret = -1;
            }
        } else {
            int del_ret = afp_sl_unlink(&vol_id, new_server_path, NULL);

            if (del_ret != 0) {
                printf("Failed to delete file %s (error: %d)\n", new_server_path, del_ret);
                ret = -1;
            }
        }
    }

    if (filebase) {
        free(filebase);
    }

    if (ret == 0) {
        int rmdir_ret = afp_sl_rmdir(&vol_id, server_path, NULL);

        if (rmdir_ret != 0) {
            printf("Failed to remove directory %s (error: %d)\n", server_path, rmdir_ret);
            return -1;
        }
    }

    return ret;
}

int com_delete(char *arg)
{
    char filename[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    struct stat st;
    int ret;
    int recursive = command_recursive_option(&arg);

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(filename, NULL, arg)) {
        printf("expecting format: rm [-r] <filename>\n");
        return -1;
    }

    if (get_server_path(filename, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    if (afp_sl_stat(&vol_id, server_fullname, NULL, &st) != 0) {
        printf("File not found: %s\n", filename);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (recursive) {
            ret = delete_directory(server_fullname);

            if (ret == 0) {
                printf("Deleted directory: %s\n", filename);
            }

            return ret;
        } else {
            printf("%s is a directory (use rm -r to delete recursively)\n",
                   filename);
            return -1;
        }
    }

    ret = afp_sl_unlink(&vol_id, server_fullname, NULL);

    if (ret != 0) {
        if (ret == -ENOENT) {
            printf("File not found: %s\n", filename);
        } else if (ret == -EACCES) {
            printf("Permission denied: %s\n", filename);
        } else {
            printf("Failed to delete %s (error: %d)\n", filename, ret);
        }

        return -1;
    }

    return 0;
}

int com_mkdir(char *arg)
{
    char dirname[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(dirname, NULL, arg)) {
        printf("expecting format: mkdir <dirname>\n");
        return -1;
    }

    if (get_server_path(dirname, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    /* Call the stateless library mkdir function with default directory permissions */
    ret = afp_sl_mkdir(&vol_id, server_fullname, NULL, 0755);

    if (ret != 0) {
        if (ret == -EEXIST) {
            printf("Directory already exists: %s\n", dirname);
        } else if (ret == -EACCES) {
            printf("Permission denied: %s\n", dirname);
        } else if (ret == -ENOENT) {
            printf("Parent directory not found: %s\n", dirname);
        } else {
            printf("Failed to create directory %s (error: %d)\n", dirname, ret);
        }

        return -1;
    }

    return 0;
}

int com_rmdir(char *arg)
{
    char dirname[AFP_MAX_PATH];
    char server_fullname[AFP_MAX_PATH];
    struct stat st;
    int ret;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    if (escape_paths(dirname, NULL, arg)) {
        printf("expecting format: rmdir <dirname>\n");
        return -1;
    }

    if (get_server_path(dirname, server_fullname) < 0) {
        printf("Invalid path\n");
        return -1;
    }

    ret = afp_sl_stat(&vol_id, server_fullname, NULL, &st);

    if (ret != 0) {
        if (ret == -ENOENT) {
            printf("Directory not found: %s\n", dirname);
        } else if (ret == -EACCES) {
            printf("Permission denied: %s\n", dirname);
        } else {
            printf("Failed to stat %s (error: %d)\n", dirname, ret);
        }

        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        printf("Not a directory: %s\n", dirname);
        return -1;
    }

    ret = afp_sl_rmdir(&vol_id, server_fullname, NULL);

    if (ret != 0) {
        if (ret == -ENOENT) {
            printf("Directory not found: %s\n", dirname);
        } else if (ret == -EACCES) {
            printf("Permission denied: %s\n", dirname);
        } else if (ret == -ENOTEMPTY) {
            printf("Directory not empty: %s\n", dirname);
        } else {
            printf("Failed to remove directory %s (error: %d)\n", dirname, ret);
        }

        return -1;
    }

    return 0;
}

int com_status(char *unused _U_)
{
    char text[40960];
    unsigned int len = sizeof(text);
    int ret;
    ret = afp_sl_status(NULL, NULL, text, &len);

    if (ret != 0) {
        printf("Could not get status\n");
        return -1;
    }

    printf("%s", text);
    return 0;
}

int com_statvfs(char *unused _U_)
{
    struct statvfs stat;
    char server_path[AFP_MAX_PATH];
    int ret;
    unsigned long long total_bytes, free_bytes;
    unsigned long long total_mb, free_mb;
    int percent_used;

    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    get_server_path(".", server_path);
    ret = afp_sl_statfs(&vol_id, server_path, NULL, &stat);

    if (ret != 0) {
        if (ret == -ENOENT) {
            printf("Path not found\n");
        } else if (ret == -EACCES) {
            printf("Permission denied\n");
        } else {
            printf("Failed to get filesystem statistics\n");
        }

        return -1;
    }

    total_bytes = (unsigned long long)stat.f_blocks * stat.f_frsize;
    free_bytes = (unsigned long long)stat.f_bfree * stat.f_frsize;
    total_mb = total_bytes / (1024 * 1024);
    free_mb = free_bytes / (1024 * 1024);

    if (total_bytes > 0) {
        percent_used = (int)(((total_bytes - free_bytes) * 100) / total_bytes);
    } else {
        percent_used = 0;
    }

    printf("Filesystem statistics for volume:\n");
    printf("  Total space:     %10llu MB\n", total_mb);
    printf("  Free space:      %10llu MB\n", free_mb);
    printf("  Used:            %10d%%\n", percent_used);
    return 0;
}


int com_lcd(char * path)
{
    int ret;
    char curpath[PATH_MAX];
    ret = chdir(path);

    if (ret != 0) {
        perror("Changing directories");
    } else {
        if (getcwd(curpath, PATH_MAX) == NULL) {
            perror("Getting current directory");
        } else {
            printf("Now in local directory %s\n", curpath);
        }
    }

    return ret;
}

/* Change to the directory ARG, or attach to volume if not attached. */
int com_cd(char *arg)
{
    char path[AFP_MAX_PATH];
    char dir_path[AFP_MAX_PATH];
    struct stat statbuf;
    size_t arg_len;
    int ret = -1;
    int show_dir = 0;

    if (!connected) {
        printf("You're not connected to a server\n");
        goto error;
    }

    if (!arg) {
        if (vol_id) {
            snprintf(curdir, AFP_MAX_PATH, "/");
            show_dir = 1;
        } else {
            list_volumes();
        }

        goto out;
    }

    arg_len = strnlen(arg, AFP_MAX_PATH);

    if (arg_len >= AFP_MAX_PATH) {
        printf("Path too long\n");
        goto error;
    }

    if (arg_len == 0) {
        if (vol_id) {
            snprintf(curdir, AFP_MAX_PATH, "/");
            show_dir = 1;
        } else {
            list_volumes();
        }

        goto out;
    }

    if (escape_paths(path, NULL, arg)) {
        printf("Invalid path\n");
        goto error;
    }

    if (vol_id == NULL) {
        /* Not attached to a volume, treat arg as volume name */
        strlcpy(url.volumename, path, AFP_VOLUME_NAME_LEN);
        unsigned int volume_options = VOLUME_EXTRA_FLAGS_NO_LOCKING;
        ret = attach_volume_with_password_prompt(&vol_id, volume_options);

        if (ret != 0
                && is_recoverable_session_error(ret) && reconnect_session(0, 0) == 0) {
            ret = attach_volume_with_password_prompt(&vol_id, volume_options);
        }

        if (ret != 0) {
            if (ret == -EACCES) {
                printf("Could not attach to volume %s: authentication failed\n",
                       url.volumename);
            } else if (ret == -ENODEV) {
                printf("Volume %s does not exist on this server\n", path);
            } else {
                printf("Could not attach to volume %s\n", url.volumename);
            }

            url.volumename[0] = '\0';
            ret = -1;
            goto error;
        }

        printf("Attached to volume %s\n", url.volumename);
        snprintf(curdir, AFP_MAX_PATH, "/");
        goto out;
    }

    /* Attached to volume, treat arg as directory */

    if (strcmp(path, "..") == 0) {
        char *p = strrchr(curdir, '/');

        if (p && p != curdir) {
            *p = '\0';
        } else {
            snprintf(curdir, AFP_MAX_PATH, "/");
        }

        show_dir = 1;
        goto out;
    }

    if (strcmp(path, ".") == 0) {
        goto out;
    }

    if (path[0] == '/') {
        strlcpy(dir_path, path, AFP_MAX_PATH);
    } else {
        int path_len;

        if (strcmp(curdir, "/") == 0) {
            path_len = snprintf(dir_path, AFP_MAX_PATH, "/%s", path);
        } else {
            path_len = snprintf(dir_path, AFP_MAX_PATH, "%s/%s", curdir, path);
        }

        if (path_len < 0 || (size_t)path_len >= AFP_MAX_PATH) {
            printf("Path too long\n");
            goto error;
        }
    }

    ret = afp_sl_stat(&vol_id, dir_path, NULL, &statbuf);

    if (ret != 0 && is_recoverable_session_error(ret)
            && reconnect_session(1, 1) == 0) {
        ret = afp_sl_stat(&vol_id, dir_path, NULL, &statbuf);
    }

    if (ret != 0) {
        printf("Directory not found: %s\n", dir_path);
        goto error;
    }

    if (!S_ISDIR(statbuf.st_mode)) {
        printf("Not a directory: %s\n", dir_path);
        goto error;
    }

    strlcpy(curdir, dir_path, AFP_MAX_PATH);
    show_dir = 1;
out:
    ret = 0;

    if (show_dir) {
        printf("Now in directory %s\n", curdir);
    }

    return ret;
error:
    return ret;
}

/* Disconnect command - explicitly detach volume and terminate server connection */
int com_disconnect(char *unused _U_)
{
    if (!connected) {
        printf("You're not connected to a server\n");
        return -1;
    }

    if (vol_id) {
        afp_sl_detach(&vol_id, NULL);
        vol_id = NULL;
        explicit_bzero(url.volpassword, sizeof(url.volpassword));
    }

    if (server_id) {
        afp_sl_disconnect(&server_id);
        server_id = NULL;
    }

    afp_sl_exit();
    printf("Disconnected from %s\n", url.servername);
    connected = 0;
    return 0;
}

/* Exit command - detach from volume but remain connected */
int com_exit(char *unused _U_)
{
    if (!connected) {
        printf("You're not connected to a server\n");
        return -1;
    }

    if (vol_id) {
        afp_sl_detach(&vol_id, NULL);
        vol_id = NULL;
        explicit_bzero(url.volpassword, sizeof(url.volpassword));
        printf("Detached from volume\n");
    }

    snprintf(curdir, AFP_MAX_PATH, "/");
    return 0;
}

/* Print out the current working directory locally. */
int com_lpwd(char *unused _U_)
{
    char dir[PATH_MAX];

    if (getcwd(dir, PATH_MAX) == NULL) {
        perror("Getting current directory");
        return -1;
    }

    printf("Now in local directory %s\n", dir);
    return 0;
}

/* Print out the current working directory. */
int com_pwd(char *unused _U_)
{
    if (!vol_id) {
        printf("You're not attached to a volume\n");
        return -1;
    }

    printf("Now in directory %s\n", curdir);
    return 0;
}

void cmdline_set_log_level(int loglevel)
{
    cmdline_log_min_rank = loglevel_to_rank(loglevel);
}

void cmdline_set_verbose(int verbose)
{
    verbose_mode = verbose;
}

int cmdline_set_metadata_mode(const char *mode)
{
    return afp_metadata_mode_parse(mode, &transfer_metadata_mode);
}

static void cmdline_log_for_client(void *priv _U_, enum logtypes logtype _U_,
                                   int loglevel, const char *message)
{
    int type_rank = loglevel_to_rank(loglevel);

    if (type_rank < cmdline_log_min_rank) {
        return; /* Filter out less-verbose messages */
    }

    fprintf(stderr, "%s\n", message);
    syslog(loglevel, "%s", message);
}

static void cmdline_stateless_log(void *user_data _U_,
                                  int loglevel, const char *message)
{
    cmdline_log_for_client(NULL, AFPFSD, loglevel, message);
}

static struct libafpclient afpclient = {
    .unmount_volume = NULL,
    .log_for_client = cmdline_log_for_client,
    .forced_ending_hook = cmdline_forced_ending_hook,
    .scan_extra_fds = NULL,
    .loop_started = cmdline_loop_started,
};

static int cmdline_server_startup(int batch_mode)
{
    char mesg[MAX_ERROR_LEN];
    memset(mesg, 0, sizeof(mesg));
    unsigned int uam_mask;
    struct afp_server_basic basic;
    uam_mask = get_uam_mask_for_url();

    if (uam_mask == 0) {
        printf("I don't know about UAM %s\n", url.uamname);
        exit(1);
    }

    if (connect_servername[0] == '\0') {
        strlcpy(connect_servername, url.servername, sizeof(connect_servername));
    }

    if (afp_sl_connect(&url, uam_mask, &server_id, mesg)) {
        printf("Could not connect to server\n");
        return -1;
    }

    connected = 1;

    if (afp_sl_serverinfo(&url, &basic) == 0
            && basic.server_name_printable[0] != '\0') {
        snprintf(url.servername, sizeof(url.servername), "%s",
                 basic.server_name_printable);
    }

    printf("Connected to server %s\n", url.servername);

    if (url.volumename[0] != '\0') {
        unsigned int volume_options = VOLUME_EXTRA_FLAGS_NO_LOCKING;
        int ret;
        ret = attach_volume_with_password_prompt(&vol_id, volume_options);

        if (ret != 0) {
            if (ret == -EACCES) {
                printf("Could not attach to volume %s: authentication failed\n",
                       url.volumename);
            } else if (ret == -ENODEV) {
                printf("Volume %s does not exist on this server\n", url.volumename);
            } else {
                printf("Could not attach to volume %s\n", url.volumename);
            }

            return -1;
        }

        if (url.path[0] != '\0') {
            snprintf(curdir, AFP_MAX_PATH, "%s", url.path);
        } else {
            snprintf(curdir, AFP_MAX_PATH, "/");
        }

        /* In non-batch mode, validate that the path (if provided) is a directory */
        if (!batch_mode && url.path[0] != '\0') {
            struct stat st;

            if (afp_sl_stat(&vol_id, url.path, NULL, &st) != 0) {
                printf("Error: Cannot access path: %s\n", url.path);
                return -1;
            }

            if (!S_ISDIR(st.st_mode)) {
                printf("Error: Path points to a file, not a directory. Interactive mode requires a directory path.\n");
                return -1;
            }
        }
    } else {
        printf("Use 'ls' to list available volumes, 'cd' to attach to a volume\n");
    }

    return 0;
}

int cmdline_batch_transfer(char * local_path, int direction, int recursive)
{
    size_t local_path_len;
    unsigned long long bytes_transferred = 0;
    int ret = -1;
    metadata_warning_emitted = 0;

    if (!connected) {
        printf("Not connected to server.\n");
        goto error;
    }

    if (!vol_id) {
        printf("Not connected to a volume. URL must include volume name.\n");
        goto error;
    }

    /* Validate local_path parameter */
    if (!local_path) {
        printf("Invalid local path.\n");
        goto error;
    }

    local_path_len = strnlen(local_path, PATH_MAX);

    if (local_path_len >= PATH_MAX) {
        printf("Local path too long.\n");
        goto error;
    }

    /* direction: 0 = GET (remote->local), 1 = PUT (local->remote) */
    if (direction == 0) {
        struct stat st;
        char remote_path[AFP_MAX_PATH];

        if (url.path[0] == '\0' || url.path[1] == '\0') { /* Empty or just "/" */
            strlcpy(remote_path, "/", sizeof(remote_path));
        } else {
            if (strlcpy(remote_path, url.path,
                        sizeof(remote_path)) >= sizeof(remote_path)) {
                printf("Warning: remote path truncated\n");
            }
        }

        if (afp_sl_stat(&vol_id, remote_path, NULL, &st) != 0) {
            printf("Remote path not found: %s\n", remote_path);
            goto error;
        }

        if (S_ISLNK(st.st_mode)) {
            printf("Symlinks are not supported: %s\n", remote_path);
            goto error;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                printf("Remote path is a directory (start afpcmd with -r to download recursively)\n");
                goto error;
            }

            ret = download_directory(remote_path, local_path, &bytes_transferred);
            goto out;
        } else {
            /* It's a file. If local_path is a directory, append filename. */
            struct stat local_st;
            char dest_path[PATH_MAX];

            if (stat(local_path, &local_st) == 0 && S_ISDIR(local_st.st_mode)) {
                char *base = strrchr(remote_path, '/');

                if (base) {
                    base++;
                } else {
                    base = remote_path;
                }

                /* Use validated local_path_len from function entry */
                int path_len;

                if (local_path_len > 0 && local_path[local_path_len - 1] == '/') {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s%s", local_path, base);
                } else {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s/%s", local_path, base);
                }

                if (path_len < 0 || (size_t)path_len >= sizeof(dest_path)) {
                    printf("Destination path too long\n");
                    goto error;
                }
            } else {
                snprintf(dest_path, sizeof(dest_path), "%s", local_path);
            }

            int fd = open(dest_path, O_CREAT | O_TRUNC | O_RDWR, 0644);

            if (fd < 0) {
                perror("open");
                goto error;
            }

            ret = retrieve_file(remote_path, fd, &st, &bytes_transferred);
            close(fd);

            if (ret == 0
                    && copy_remote_metadata_to_local(remote_path, dest_path, &st) < 0) {
                printf("Could not preserve metadata for %s\n", remote_path);
                ret = -1;
            }

            goto out;
        }
    } else {
        /* PUT: url.path is the destination directory */
        struct stat st;

        if (lstat(local_path, &st) != 0) {
            perror("lstat");
            goto error;
        }

        if (S_ISLNK(st.st_mode)) {
            printf("Symlinks are not supported: %s\n", local_path);
            goto error;
        }

        char remote_base[AFP_MAX_PATH];

        if (url.path[0] == '\0') {
            strlcpy(remote_base, "/", AFP_MAX_PATH);
        } else {
            strlcpy(remote_base, url.path, AFP_MAX_PATH);
        }

        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                printf("Local path is a directory (start afpcmd with -r to upload recursively)\n");
                goto error;
            }

            char *base = basename(local_path);
            char dest_path[AFP_MAX_PATH];

            /* If local path is "." or equivalent, upload contents directly to remote_base */
            if (strcmp(base, ".") == 0) {
                strlcpy(dest_path, remote_base, AFP_MAX_PATH);
            } else {
                int path_len;

                if (strcmp(remote_base, "/") == 0) {
                    path_len = snprintf(dest_path, sizeof(dest_path), "/%s", base);
                } else {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s/%s", remote_base, base);
                }

                if (path_len < 0 || (size_t)path_len >= sizeof(dest_path)) {
                    printf("Destination path too long\n");
                    goto error;
                }
            }

            ret = upload_directory(local_path, dest_path, &bytes_transferred);
            goto out;
        } else {
            char *base = basename(local_path);
            char dest_path[AFP_MAX_PATH];
            struct stat remote_st;
            int is_dir = 0;
            int path_len;

            if (afp_sl_stat(&vol_id, remote_base, NULL, &remote_st) == 0
                    && S_ISDIR(remote_st.st_mode)) {
                is_dir = 1;
            }

            if (is_dir) {
                if (strcmp(remote_base, "/") == 0) {
                    path_len = snprintf(dest_path, sizeof(dest_path), "/%s", base);
                } else {
                    path_len = snprintf(dest_path, sizeof(dest_path), "%s/%s", remote_base, base);
                }
            } else {
                path_len = snprintf(dest_path, sizeof(dest_path), "%s", remote_base);
            }

            if (path_len < 0 || (size_t)path_len >= sizeof(dest_path)) {
                printf("Destination path too long\n");
                goto error;
            }

            ret = upload_file(local_path, dest_path, &bytes_transferred);
            goto out;
        }
    }

out:

    if (bytes_transferred > 0) {
        if (direction == 0) {
            printf("Transfer complete. %llu bytes received.\n", bytes_transferred);
        } else {
            printf("Transfer complete. %llu bytes sent.\n", bytes_transferred);
        }
    }

error:
    return ret;
}

void cmdline_afp_exit(void)
{
    /* Drop our socket and let the daemon clean up the client slot
     * while keeping connections alive. Other clients may be using
     * the same daemon with the same server connection. */
    vol_id = NULL;
    server_id = NULL;
    connected = 0;
}

void cmdline_afp_setup_client(void)
{
    openlog("afpcmd", LOG_PID | LOG_CONS, LOG_USER);
    libafpclient_register(&afpclient);
    afp_sl_set_log_callback(cmdline_stateless_log, NULL);
}


int cmdline_afp_setup(int batch_mode, char * url_string)
{
    snprintf(curdir, AFP_MAX_PATH, "%s", DEFAULT_DIRECTORY);
    memset(connect_servername, 0, sizeof(connect_servername));

    if (init_uams() < 0) {
        return -1;
    }

    afp_default_url(&url);

    if (url_string) {
        size_t url_len = strnlen(url_string, MAX_INPUT_LEN);

        if (url_len >= MAX_INPUT_LEN) {
            printf("URL too long.\n");
            return -1;
        }

        if (url_len > 1) {
            if (afp_parse_url(&url, url_string)) {
                printf("Could not parse url.\n");
                return -1;
            }

            /* If no username was specified in URL, use AFP guest user */
            if (url.username[0] == '\0') {
                strlcpy(url.username, "nobody", AFP_MAX_USERNAME_LEN);
            }

            strlcpy(connect_servername, url.servername, sizeof(connect_servername));
            cmdline_getpass();
            trigger_connected();

            if (cmdline_server_startup(batch_mode) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static char *escape_spaces(const char *str)
{
    size_t len, spaces = 0;

    if (!str) {
        return NULL;
    }

    len = strnlen(str, AFP_MAX_PATH);

    if (len >= AFP_MAX_PATH) {
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        if (str[i] == ' ') {
            spaces++;
        }
    }

    if (!spaces) {
        return strdup(str);
    }

    if (len > ((size_t) -1) - spaces - 1) {
        return NULL;
    }

    char *ret = malloc(len + spaces + 1);

    if (!ret) {
        return NULL;
    }

    char *dst = ret;
    const char *end = ret + len + spaces;
    const char *src = str;

    while (*src) {
        if (*src == ' ') {
            if (dst >= end) {
                break;
            }

            *dst++ = '\\';
        }

        if (dst >= end) {
            break;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
    return ret;
}

/* Helper to unescape backslash-escaped spaces in completion text */
static char *unescape_spaces(const char *str)
{
    if (!str) {
        return NULL;
    }

    size_t len = strnlen(str, AFP_MAX_PATH);

    if (len >= AFP_MAX_PATH) {
        return NULL;
    }

    /* Quick check: if no backslashes, just duplicate */
    int has_escape = 0;

    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\\' && i + 1 < len && str[i + 1] == ' ') {
            has_escape = 1;
            break;
        }
    }

    if (!has_escape) {
        return strdup(str);
    }

    char *ret = malloc(len + 1);

    if (!ret) {
        return NULL;
    }

    char *dst = ret;
    const char *src = str;

    while (*src) {
        if (*src == '\\' && *(src + 1) == ' ') {
            *dst++ = ' ';
            src += 2;  /* skip both \ and space */
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return ret;
}

char *afp_remote_file_generator(const char *text, int state)
{
    static struct afp_file_info_basic *filebase = NULL;
    static struct afp_volume_summary *volbase = NULL;
    static unsigned int count = 0;
    static unsigned int list_index = 0;
    static size_t len = 0;
    static int is_volume_list = 0;
    static char *basename_unescaped = NULL;
    /* Number of unescaped chars already in the buffer before readline's word
       start; non-zero when the library split the word at a backslash-escaped
       space (e.g. libedit gives text="q" for input "cd asdf\ q"). */
    static size_t return_offset = 0;
    const char *name;
    char *ret_str = NULL;

    if (!state) {
        /* Clean up from previous completion */
        if (basename_unescaped) {
            free(basename_unescaped);
            basename_unescaped = NULL;
        }

        if (filebase) {
            free(filebase);
            filebase = NULL;
        }

        if (volbase) {
            free(volbase);
            volbase = NULL;
        }

        count = 0;
        list_index = 0;
        return_offset = 0;

        if (!text) {
            return NULL;
        }

        size_t text_len = strnlen(text, MAX_INPUT_LEN);

        if (text_len >= MAX_INPUT_LEN) {
            return NULL;
        }

        is_volume_list = 0;

        if (!connected) {
            return NULL;
        }

        if (!vol_id) {
            unsigned int numvols = 0;
            volbase = malloc(sizeof(struct afp_volume_summary) * 100);

            if (!volbase) {
                return NULL;
            }

            /* Set up basename matching so the loop doesn't use stale values
               from a previous file-list completion. */
            basename_unescaped = unescape_spaces(text);

            if (!basename_unescaped) {
                free(volbase);
                return NULL;
            }

            len = strnlen(basename_unescaped, AFP_MAX_PATH);

            if (len >= AFP_MAX_PATH) {
                free(basename_unescaped);
                basename_unescaped = NULL;
                free(volbase);
                volbase = NULL;
                return NULL;
            }

            if (afp_sl_getvols(&url, 0, 100, &numvols, volbase) == 0) {
                count = numvols;
                is_volume_list = 1;
            } else {
                free(volbase);
                volbase = NULL;
                return NULL;
            }
        } else {
            char dir_path[AFP_MAX_PATH];
            const char *last_slash = strrchr(text, '/');
            char prefix[AFP_MAX_PATH] = {0};

            if (last_slash) {
                size_t dir_len = (size_t)(last_slash - text);
                int path_len;

                if (dir_len >= sizeof(prefix)) {
                    return NULL;
                }

                memcpy(prefix, text, dir_len);
                prefix[dir_len] = '\0';

                if (text[0] == '/') {
                    if (dir_len == 0) {
                        strlcpy(dir_path, "/", sizeof(dir_path));
                    } else {
                        path_len = snprintf(dir_path, sizeof(dir_path), "%s", prefix);

                        if (path_len < 0 || (size_t)path_len >= sizeof(dir_path)) {
                            return NULL;
                        }
                    }
                } else {
                    if (strcmp(curdir, "/") == 0) {
                        path_len = snprintf(dir_path, sizeof(dir_path), "/%s", prefix);
                    } else {
                        path_len = snprintf(dir_path, sizeof(dir_path), "%s/%s", curdir, prefix);
                    }

                    if (path_len < 0 || (size_t)path_len >= sizeof(dir_path)) {
                        return NULL;
                    }
                }

                /* Extract and unescape just the basename for matching */
                const char *basename_part = last_slash + 1;
                basename_unescaped = unescape_spaces(basename_part);

                if (!basename_unescaped) {
                    return NULL;
                }

                len = strnlen(basename_unescaped, AFP_MAX_PATH);

                if (len >= AFP_MAX_PATH) {
                    free(basename_unescaped);
                    basename_unescaped = NULL;
                    return NULL;
                }
            } else {
                strlcpy(dir_path, curdir, sizeof(dir_path));
                /* Some readline-compatible libraries (e.g. libedit) split
                   the word at backslash-escaped spaces, so for input like
                   "cd asdf\ q<TAB>" they pass text="q" instead of the full
                   "asdf\ q".  Detect this by scanning rl_line_buffer backward
                   from where the library put the word start; if there are
                   one or more "\ " sequences immediately before it, reconstruct
                   the full unescaped prefix for matching and remember how many
                   unescaped chars are already in the buffer (return_offset) so
                   we return only the suffix readline needs to insert. */
                int readline_start = rl_point - (int)text_len;
                int true_start = readline_start;

                while (true_start >= 2 &&
                        rl_line_buffer[true_start - 1] == ' ' &&
                        rl_line_buffer[true_start - 2] == '\\') {
                    true_start -= 2;

                    while (true_start > 0 &&
                            rl_line_buffer[true_start - 1] != ' ') {
                        true_start--;
                    }
                }

                if (true_start < readline_start) {
                    int raw_prefix_len = readline_start - true_start;
                    char escaped_prefix[AFP_MAX_PATH];

                    if (raw_prefix_len >= AFP_MAX_PATH) {
                        return NULL;
                    }

                    memcpy(escaped_prefix, rl_line_buffer + true_start, raw_prefix_len);
                    escaped_prefix[raw_prefix_len] = '\0';
                    char *unescaped_prefix = unescape_spaces(escaped_prefix);

                    if (!unescaped_prefix) {
                        return NULL;
                    }

                    char *unescaped_text = unescape_spaces(text);

                    if (!unescaped_text) {
                        free(unescaped_prefix);
                        return NULL;
                    }

                    size_t prefix_len = strnlen(unescaped_prefix, AFP_MAX_PATH);
                    size_t unescaped_text_len = strnlen(unescaped_text,
                                                        AFP_MAX_PATH);

                    if (prefix_len >= AFP_MAX_PATH
                            || unescaped_text_len >= AFP_MAX_PATH
                            || prefix_len > AFP_MAX_PATH - 1U
                            - unescaped_text_len) {
                        free(unescaped_prefix);
                        free(unescaped_text);
                        return NULL;
                    }

                    return_offset = prefix_len;
                    size_t combined_len = prefix_len + unescaped_text_len;
                    basename_unescaped = malloc(combined_len + 1);

                    if (!basename_unescaped) {
                        free(unescaped_prefix);
                        free(unescaped_text);
                        return NULL;
                    }

                    memcpy(basename_unescaped, unescaped_prefix, prefix_len);
                    memcpy(basename_unescaped + prefix_len, unescaped_text,
                           unescaped_text_len + 1U);
                    free(unescaped_prefix);
                    free(unescaped_text);
                } else {
                    /* Normal case: unescape the whole text for matching */
                    basename_unescaped = unescape_spaces(text);

                    if (!basename_unescaped) {
                        return NULL;
                    }
                }

                len = strnlen(basename_unescaped, AFP_MAX_PATH);

                if (len >= AFP_MAX_PATH) {
                    free(basename_unescaped);
                    basename_unescaped = NULL;
                    return NULL;
                }
            }

            int eod = 0;

            if (afp_sl_readdir(&vol_id, dir_path, NULL, 0, 1000, &count, &filebase,
                               &eod) != 0) {
                return NULL;
            }
        }
    }

    while (list_index < count) {
        if (is_volume_list) {
            name = volbase[list_index].volume_name_printable;
        } else {
            name = filebase[list_index].name;
        }

        list_index++;

        if (strncmp(name, basename_unescaped, len) == 0) {
            /* Return only the suffix after the prefix already in the buffer.
               In the normal case return_offset is 0 and the full escaped name
               is returned.  When the library split at an escaped space,
               return_offset > 0 and we skip the part already typed. */
            char *escaped_name = escape_spaces(name + return_offset);

            if (!escaped_name) {
                return NULL;
            }

            if (!is_volume_list) {
                const char *last_slash = strrchr(text, '/');

                if (last_slash) {
                    size_t dir_len = (size_t)(last_slash - text) + 1U;
                    size_t escaped_len = strnlen(escaped_name,
                                                 2U * AFP_MAX_PATH);

                    if (escaped_len >= 2U * AFP_MAX_PATH
                            || dir_len > SIZE_MAX - escaped_len - 1U) {
                        free(escaped_name);
                        return NULL;
                    }

                    size_t total_len = dir_len + escaped_len + 1U;
                    ret_str = malloc(total_len);

                    if (ret_str) {
                        memcpy(ret_str, text, dir_len);
                        memcpy(ret_str + dir_len, escaped_name, escaped_len + 1U);
                    }

                    free(escaped_name);
                    return ret_str;
                }
            }

            return escaped_name;
        }
    }

    return NULL;
}
