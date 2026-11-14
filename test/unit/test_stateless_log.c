/*
 * test_stateless_log.c: libafpsl's framing of the daemon's answers and its
 * heartbeats, and its log lines
 *
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 * Copyright (C) 2026 Andy Lemin (andylemin)
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

/*!
 * @file
 * @brief Unit tests of libafpsl's framing, heartbeats and log lines
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "daemon/stateless_ipc.h"
#include "netatalk-client/afpsl.h"
#include "stateless_internal.h"
#include "tap.h"

#ifdef AFPSL_IPC_COMMAND_PING
#error "Ping must not be part of the stateless IPC protocol"
#endif

struct capture {
    int calls;
    int level;
    char message[64];
};

static void capture_log(void *user_data, int loglevel, const char *message)
{
    struct capture *capture = user_data;
    capture->calls++;
    capture->level = loglevel;
    snprintf(capture->message, sizeof(capture->message), "%s", message);
}

static size_t make_response(char *response, size_t capacity)
{
    struct afpsl_ipc_response_header header = { 0 };
    struct afpsl_ipc_log_footer footer = {
        .magic = AFPSL_IPC_LOG_MAGIC,
        .log_len = 0,
    };
    size_t len = sizeof(header) + sizeof(footer);

    if (capacity < len) {
        return 0;
    }

    header.len = (unsigned int) len;
    memcpy(response, &header, sizeof(header));
    memcpy(response + sizeof(header), &footer, sizeof(footer));
    return len;
}

static int check_framed_read_errors(void)
{
    struct afpsl_ipc_response_header header = { 0 };
    char response[64];
    size_t response_len = 123;
    int sockets[2];
    int directory;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    header.len = sizeof(header) + sizeof(struct afpsl_ipc_log_footer);
    CHECK(write(sockets[1], &header, sizeof(header))
          == (ssize_t) sizeof(header));
    close(sockets[1]);
    CHECK(afp_sl_read_framed_response(sockets[0], response, sizeof(response),
                                      &response_len) == -1);
    CHECK(response_len == 0);
    close(sockets[0]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    header.len = sizeof(response) + 1;
    CHECK(write(sockets[1], &header, sizeof(header))
          == (ssize_t) sizeof(header));
    CHECK(afp_sl_read_framed_response(sockets[0], response, sizeof(response),
                                      &response_len) == -1);
    CHECK(response_len == 0);
    close(sockets[0]);
    close(sockets[1]);
    directory = open(".", O_RDONLY);
    CHECK(directory >= 0);
    CHECK(afp_sl_read_framed_response(directory, response, sizeof(response),
                                      &response_len) == -1);
    CHECK(response_len == 0);
    close(directory);
    return 0;
}

static volatile sig_atomic_t alarm_received;

static void handle_alarm(int signal_number)
{
    (void) signal_number;
    alarm_received = 1;
}

static int check_interrupted_framed_read(void)
{
    struct sigaction action = { 0 };
    struct sigaction old_action;
    struct itimerval timer = { 0 };
    struct timespec delay = { .tv_nsec = 200000000 };
    char expected[64];
    char response[64];
    size_t expected_len;
    size_t response_len;
    pid_t child;
    int sockets[2];
    int status;
    expected_len = make_response(expected, sizeof(expected));
    CHECK(expected_len > 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    child = fork();

    if (child < 0) {
        CHECK(child >= 0);
    }

    if (child == 0) {
        close(sockets[0]);
        nanosleep(&delay, NULL);

        if (write(sockets[1], expected, expected_len)
                != (ssize_t) expected_len) {
            _exit(1);
        }

        close(sockets[1]);
        _exit(0);
    }

    CHECK(child > 0);
    close(sockets[1]);
    action.sa_handler = handle_alarm;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, &old_action) == 0);
    timer.it_value.tv_usec = 50000;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    CHECK(afp_sl_read_framed_response(sockets[0], response, sizeof(response),
                                      &response_len) == 0);
    timer.it_value.tv_usec = 0;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    CHECK(sigaction(SIGALRM, &old_action, NULL) == 0);
    CHECK(alarm_received == 1);
    CHECK(response_len == expected_len);
    CHECK(memcmp(response, expected, expected_len) == 0);
    close(sockets[0]);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

/*!
 * @brief Check that the frames saying a command runs are skipped
 *
 * Two of them and the answer come in one write, as a read may find them.
 *
 * @returns 0 once the checks have run
 */
static int check_skipped_heartbeats(void)
{
    struct {
        struct afpsl_ipc_response_header header;
        struct afpsl_ipc_log_footer footer;
    } frame;
    char expected[64];
    char frames[2 * sizeof(frame) + sizeof(expected)];
    char response[64];
    size_t expected_len;
    size_t frames_len;
    size_t response_len = 0;
    int sockets[2];
    memset(&frame, 0, sizeof(frame));
    frame.header.result = AFPSL_IPC_RESULT_WORKING;
    frame.header.len = sizeof(frame);
    frame.footer.magic = AFPSL_IPC_LOG_MAGIC;
    expected_len = make_response(expected, sizeof(expected));
    CHECK(expected_len > 0);
    memcpy(frames, &frame, sizeof(frame));
    memcpy(frames + sizeof(frame), &frame, sizeof(frame));
    memcpy(frames + 2 * sizeof(frame), expected, expected_len);
    frames_len = 2 * sizeof(frame) + expected_len;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(write(sockets[1], frames, frames_len) == (ssize_t) frames_len);
    CHECK(afp_sl_read_framed_response(sockets[0], response, sizeof(response),
                                      &response_len) == 0);
    CHECK(response_len == expected_len);
    CHECK(memcmp(response, expected, expected_len) == 0);
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}

/*!
 * @brief Check the start of an answer read in parts
 *
 * A frame saying the command runs comes first and is skipped, and a signal
 * while the read waits does not end it.
 *
 * @returns 0 once the checks have run
 */
static int check_answer_start(void)
{
    struct sigaction action = { 0 };
    struct sigaction old_action;
    struct itimerval timer = { 0 };
    struct timespec delay = { .tv_nsec = 200000000 };
    struct {
        struct afpsl_ipc_response_header header;
        struct afpsl_ipc_log_footer footer;
    } frame;
    char expected[16];
    char answer[sizeof(expected)];
    pid_t child;
    int sockets[2];
    int status;
    memset(&frame, 0, sizeof(frame));
    frame.header.result = AFPSL_IPC_RESULT_WORKING;
    frame.header.len = sizeof(frame);
    frame.footer.magic = AFPSL_IPC_LOG_MAGIC;
    memset(expected, 0x5a, sizeof(expected));
    memset(expected, 0, sizeof(struct afpsl_ipc_response_header));
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    child = fork();

    if (child < 0) {
        CHECK(child >= 0);
    }

    if (child == 0) {
        close(sockets[0]);
        nanosleep(&delay, NULL);

        if (write(sockets[1], &frame, sizeof(frame)) != (ssize_t) sizeof(frame)
                || write(sockets[1], expected, sizeof(expected))
                != (ssize_t) sizeof(expected)) {
            _exit(1);
        }

        close(sockets[1]);
        _exit(0);
    }

    CHECK(child > 0);
    close(sockets[1]);
    alarm_received = 0;
    action.sa_handler = handle_alarm;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, &old_action) == 0);
    timer.it_value.tv_usec = 50000;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    CHECK(afp_sl_read_answer_start(sockets[0], answer, sizeof(answer)) == 0);
    timer.it_value.tv_usec = 0;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    CHECK(sigaction(SIGALRM, &old_action, NULL) == 0);
    CHECK(alarm_received == 1);
    CHECK(memcmp(answer, expected, sizeof(expected)) == 0);
    close(sockets[0]);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

/*!
 * @brief Run libafpsl's framing, heartbeat and log checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    const char payload[] = "DATA";
    const char message[] = "daemon\nwarning";
    const char expected_message[] = "daemon\\nwarning";
    char response[sizeof(payload) - 1 + sizeof(struct afpsl_ipc_log_record) +
                  sizeof(message) - 1 + sizeof(struct afpsl_ipc_log_footer)];
    struct afpsl_ipc_log_record record = {
        .level = LOG_WARNING,
        .message_len = sizeof(message) - 1,
    };
    struct afpsl_ipc_log_footer footer = {
        .magic = AFPSL_IPC_LOG_MAGIC,
        .log_len = sizeof(record) + sizeof(message) - 1,
    };
    struct capture capture = { 0 };
    afpc_volume_t dummy_volume = (afpc_volume_t)(uintptr_t)1;
    unsigned char oversized_comment[AFP_SL_DESKTOP_COMMENT_MAX + 1U] = { 0 };
    size_t pos = 0;
    size_t content_len = 0;
    test_tap_init(argc, argv);
    CHECK(afp_sl_desktop_set_comment(&dummy_volume, "/file",
                                     oversized_comment,
                                     sizeof(oversized_comment), NULL,
                                     NULL) == -E2BIG);
    memcpy(response + pos, payload, sizeof(payload) - 1);
    pos += sizeof(payload) - 1;
    memcpy(response + pos, &record, sizeof(record));
    pos += sizeof(record);
    memcpy(response + pos, message, sizeof(message) - 1);
    pos += sizeof(message) - 1;
    memcpy(response + pos, &footer, sizeof(footer));
    afp_sl_set_log_callback(capture_log, &capture);
    CHECK(afp_sl_response_content_length(response, sizeof(response),
                                         &content_len)
          == 0);
    CHECK(content_len == sizeof(payload) - 1);
    CHECK(afp_sl_dispatch_response_logs(response, sizeof(response)) == 0);
    CHECK(capture.calls == 1);
    CHECK(capture.level == LOG_WARNING);
    CHECK(strcmp(capture.message, expected_message) == 0);
    footer.magic = 0;
    memcpy(response + sizeof(response) - sizeof(footer), &footer, sizeof(footer));
    CHECK(afp_sl_response_content_length(response, sizeof(response),
                                         &content_len)
          == -1);
    CHECK(check_framed_read_errors() == 0);
    CHECK(check_interrupted_framed_read() == 0);
    CHECK(check_skipped_heartbeats() == 0);
    CHECK(check_answer_start() == 0);
    return test_tap_finish();
}
