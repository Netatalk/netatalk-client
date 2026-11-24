/*!
 * @file
 * @brief libfuse's captured errors and the text of mount errors
 *
 * The interface of fuse_error.c.
 * Lane: control.
 * Northbound: the mount daemon's commands and file operations.
 */

#ifndef NETATALK_CLIENT_FUSE_ERROR_H
#define NETATALK_CLIENT_FUSE_ERROR_H

void report_fuse_errors(void *log_context, int started);
void fuse_capture_stderr_start(void);
const char *fuse_result_to_string(int fuse_result);
const char *mount_errno_to_string(int err);

#endif
