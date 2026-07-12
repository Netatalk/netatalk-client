
#ifndef __CLIENT_H_
#define __CLIENT_H_

#include <unistd.h>
#include <syslog.h>
#include <sys/select.h>
#include <stdarg.h>
#include <stdio.h>

#define MAX_CLIENT_RESPONSE 16384


enum logtypes {
    AFPFSD,
};

struct afp_server;
struct afp_volume;

struct libafpclient {
    int (*unmount_volume)(struct afp_volume * volume);
    void (*log_for_client)(void * priv,
                           enum logtypes logtype, int loglevel, const char *message);
    void (*forced_ending_hook)(void);
    int (*scan_extra_fds)(int command_fd, fd_set *set, int * max_fd);
    void (*loop_started)(void);
} ;

extern struct libafpclient *libafpclient;

void libafpclient_register(struct libafpclient * tmpclient);
void signal_main_thread(void);

/* These are logging functions */

#define MAXLOGSIZE 2048
#define MAX_ERROR_LEN 1024
#define VOLNAME_LEN 1024

#define LOG_METHOD_SYSLOG 1
#define LOG_METHOD_STDOUT 2

void set_log_method(int m);

/* Public logging API: message is complete text, not a printf format. */
void log_for_client(void * priv,
                    enum logtypes logtype, int loglevel,
                    const char *message);

/* Internal convenience for literal printf-style formats. */
static inline void log_for_clientf(void * priv,
                                   enum logtypes logtype, int loglevel,
                                   const char *format, ...)
__attribute__((format(printf, 4, 5)));

static inline void log_for_clientf(void * priv,
                                   enum logtypes logtype, int loglevel,
                                   const char *format, ...)
{
    va_list ap;
    char message[MAX_ERROR_LEN];

    if (format == NULL) {
        log_for_client(priv, logtype, loglevel, "(null)");
        return;
    }

    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    log_for_client(priv, logtype, loglevel, message);
}

#ifndef AFPCLIENT_NO_LOG_MACRO
#define log_for_client(...) log_for_clientf(__VA_ARGS__)
#endif

void stdout_log_for_client(void * priv,
                           enum logtypes logtype, int loglevel, const char *message);

#endif
