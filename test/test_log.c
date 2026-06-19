#include <stdio.h>
#include <string.h>

#include "libafpclient.h"
#include "utils.h"

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
    char sanitized[sizeof("line 1\\r\\nline\\t2\\x01\\x7f\\r\\n")];
    struct libafpclient test_client = {
        .unmount_volume = NULL,
        .log_for_client = capture_log_message,
        .forced_ending_hook = NULL,
        .scan_extra_fds = NULL,
        .loop_started = NULL,
    };
    const char input[] = "line 1\r\nline\t2\x01\x7f\r\n";
    const char expected_sanitized[] =
        "line 1\\r\\nline\\t2\\x01\\x7f\\r\\n";
    const char expected_log[] = "line 1\\r\\nline\\t2\\x01\\x7f";
    sanitize_text(input, sanitized, sizeof(sanitized));

    if (strcmp(sanitized, expected_sanitized) != 0) {
        fprintf(stderr, "sanitize expected: %s\nactual:            %s\n",
                expected_sanitized, sanitized);
        return 1;
    }

    libafpclient_register(&test_client);
    log_for_client(NULL, AFPFSD, LOG_INFO, "%s", input);
    libafpclient_register(NULL);

    if (strcmp(captured_message, expected_log) != 0) {
        fprintf(stderr, "expected: %s\nactual:   %s\n",
                expected_log, captured_message);
        return 1;
    }

    return 0;
}
