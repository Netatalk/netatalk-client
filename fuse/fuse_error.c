/*
 *  fuse_error.c
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief libfuse's error output for a mount's client, and mount error text
 *
 * Part of afpfsd, the FUSE front end, and outside AFP.
 * Lane: control.
 * Northbound: the mount daemon's commands and file operations.
 * Southbound: none.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

#include "lib/client.h"

#include "fuse_error.h"

/* Simple global capture state (not thread-safe, but adequate for mount startup) */
static int captured_fd = -1;
/* FD for captured stderr temp file */
static int captured_stderr_fd = -1;
static fpos_t pos;

#define FUSE_ERROR_PREFIX "FUSE error: "
#define FUSE_OUTPUT_PREFIX "FUSE: "

/*!
 * @brief End the capture of standard error and log what it caught
 *
 * Output of a mount that has started, such as libfuse's debug output under
 * -d, is logged at debug level; a failed or unfinished start's is an error.
 *
 * @param[in] log_context  the client the message goes to, or NULL
 * @param[in] started      1 when the mount has started, 0 otherwise
 */
void report_fuse_errors(void *log_context, int started)
{
    char buf[MAX_ERROR_LEN - sizeof(FUSE_ERROR_PREFIX)];
    char message[MAX_ERROR_LEN];
    ssize_t len;

    if (captured_stderr_fd < 0) {
        return;  /* No capture was started */
    }

    fflush(stderr);
    dup2(captured_fd, fileno(stderr));
    close(captured_fd);
    captured_fd = -1;
    clearerr(stderr);
    fsetpos(stderr, &pos);        /* for C9X */
    len = lseek(captured_stderr_fd, 0, SEEK_SET) < 0 ? -1
          : read(captured_stderr_fd, buf, sizeof(buf) - 1);
    close(captured_stderr_fd);
    captured_stderr_fd = -1;

    if (len > 0) {
        buf[len] = '\0';
        snprintf(message, sizeof(message), "%s%s",
                 started ? FUSE_OUTPUT_PREFIX : FUSE_ERROR_PREFIX, buf);
        (log_for_client)(log_context, AFPFSD, started ? LOG_DEBUG : LOG_ERR,
                         message);
    }
}

/*!
 * @brief Send standard error to a capture until report_fuse_errors()
 */
void fuse_capture_stderr_start(void)
{
    int fd;
    char tmpl[] = "/tmp/fuse_stderr_XXXXXX";
    fd = mkstemp(tmpl);

    if (fd < 0) {
        captured_stderr_fd = -1;
        return;
    }

    /* The descriptor keeps the file until the capture closes it. */
    unlink(tmpl);
    captured_stderr_fd = fd;
    fflush(stderr);
    fgetpos(stderr, &pos);
    captured_fd = dup(fileno(stderr));

    if (captured_fd >= 0) {
        dup2(fd, fileno(stderr));
    } else {
        close(fd);
        captured_stderr_fd = -1;
    }
}

/*!
 * @brief Describe a result of fuse_main()
 *
 * @param[in] fuse_result  the result
 * @returns its description
 */
const char *fuse_result_to_string(int fuse_result)
{
    switch (fuse_result) {
    case 0:
        return "Success";

    case 1:
        return "Invalid option arguments or generic error";

    case 2:
        return "No mount point specified";

    case 3:
        return "FUSE setup failed";

    case 4:
        return "Mounting failed";

    case 5:
        return "Failed to daemonize (detach from session)";

    case 6:
        return "Failed to set up signal handlers";

    case 7:
        return "Error occurred during the life of the filesystem";

    default:
        return "Unknown FUSE error code";
    }
}

/*!
 * @brief Describe the errno of a failed mount, with a hint where one helps
 *
 * @param[in] err  the errno
 * @returns its description
 */
const char *mount_errno_to_string(int err)
{
    switch (err) {
    case 0:
        return "No error";

    case EACCES:
        return "Permission denied - check access to FUSE device and mountpoint";

    case EPERM:
        return "Operation not permitted - may need root/sudo privileges";

    case EBUSY:
        return "Mountpoint is already in use or device is busy";

    case ENOTDIR:
        return "Mountpoint is not a directory";

    case ENOENT:
        return "Mountpoint or FUSE device does not exist";

    case ENODEV:
        return "FUSE kernel module not loaded - try 'modprobe fuse' or load macfuse";

    case EINVAL:
        return "Invalid mount options or arguments";

    case ENOSPC:
        return "No space available for mount table entry";

    case ENOMEM:
        return "Insufficient memory to complete mount operation";

    case ENOTBLK:
        return "Block device required but not provided";

    case EFAULT:
        return "Bad address in mount parameters";

    default:
        return strerror(err);
    }
}
