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

#include "afp_ipc.h"
#include "afp_server.h"
#include "afpsl.h"
#include "stateless_internal.h"
#include "tap.h"

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
    struct afp_server_response_header header = { 0 };
    struct afp_server_log_footer footer = {
        .magic = AFP_SERVER_LOG_MAGIC,
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
    struct afp_server_response_header header = { 0 };
    char response[64];
    size_t response_len = 123;
    int sockets[2];
    int directory;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    header.len = sizeof(header) + sizeof(struct afp_server_log_footer);
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

int main(int argc, char **argv)
{
    const char payload[] = "DATA";
    const char message[] = "daemon\nwarning";
    const char expected_message[] = "daemon\\nwarning";
    char response[sizeof(payload) - 1 + sizeof(struct afp_server_log_record) +
                  sizeof(message) - 1 + sizeof(struct afp_server_log_footer)];
    struct afp_server_log_record record = {
        .level = LOG_WARNING,
        .message_len = sizeof(message) - 1,
    };
    struct afp_server_log_footer footer = {
        .magic = AFP_SERVER_LOG_MAGIC,
        .log_len = sizeof(record) + sizeof(message) - 1,
    };
    struct capture capture = { 0 };
    size_t pos = 0;
    size_t content_len = 0;
    test_tap_init(argc, argv);
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
    return test_tap_finish();
}
