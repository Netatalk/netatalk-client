#include <stdio.h>
#include <string.h>

#include "libafpclient.h"

static char captured_message[MAX_ERROR_LEN * 4];

static void capture_log_message(
    __attribute__((unused)) void *priv,
    __attribute__((unused)) enum logtypes logtype,
    __attribute__((unused)) int loglevel,
    const char *message)
{
    snprintf(captured_message, sizeof(captured_message), "%s", message);
}

int main(void)
{
    struct libafpclient test_client = {
        .unmount_volume = NULL,
        .log_for_client = capture_log_message,
        .forced_ending_hook = NULL,
        .scan_extra_fds = NULL,
        .loop_started = NULL,
    };
    const char input[] = "line 1\r\nline\t2\x01\x7f";
    const char expected[] = "line 1\\r\\nline\\t2\\x01\\x7f";
    libafpclient_register(&test_client);
    log_for_client(NULL, AFPFSD, LOG_INFO, "%s", input);
    libafpclient_register(NULL);

    if (strcmp(captured_message, expected) != 0) {
        fprintf(stderr, "expected: %s\nactual:   %s\n",
                expected, captured_message);
        return 1;
    }

    return 0;
}
