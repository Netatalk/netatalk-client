#ifndef NETATALK_CLIENT_FUSE_ERROR_H
#define NETATALK_CLIENT_FUSE_ERROR_H

void report_fuse_errors(void *log_context);
void fuse_capture_stderr_start(void);
const char *fuse_result_to_string(int fuse_result);
const char *mount_errno_to_string(int err);

#endif
