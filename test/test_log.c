#include <stdio.h>
#include <string.h>

#include "compat.h"
#include "libafpclient.h"
#include "tap.h"
#include "utils.h"

static char captured_message[MAX_ERROR_LEN * 4];

static void capture_log_message(
    void *priv _U_,
    enum logtypes logtype _U_,
    int loglevel _U_,
    const char *message)
{
    snprintf(captured_message, sizeof(captured_message), "%s", message);
}

int main(int argc, char **argv)
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
    test_tap_init(argc, argv);
    sanitize_text(input, sanitized, sizeof(sanitized));
    CHECK(strcmp(sanitized, expected_sanitized) == 0);
    libafpclient_register(&test_client);
    log_for_client(NULL, AFPFSD, LOG_INFO, "%s", input);
    CHECK(strcmp(captured_message, expected_log) == 0);
    (log_for_client)(NULL, AFPFSD, LOG_INFO, input);
    CHECK(strcmp(captured_message, expected_log) == 0);
    (log_for_client)(NULL, AFPFSD, LOG_INFO, NULL);
    libafpclient_register(NULL);
    CHECK(strcmp(captured_message, "(null)") == 0);
    return test_tap_finish();
}
