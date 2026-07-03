/*
 *
 *  Copyright (C) 2008 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <syslog.h>

#include "afp.h"
#include "afp_protocol.h"
#include "compat.h"
#include "uams_def.h"
#include "utils.h"

#define FLAG_COUNT 16

static bool show_icon = false;
static int log_min_rank;

static void getstatus_log_for_client(void *priv _U_,
                                     enum logtypes logtype _U_,
                                     int loglevel, const char *message)
{
    if (loglevel_to_rank(loglevel) < log_min_rank) {
        return;
    }

    printf("%s\n", message);
}

static struct libafpclient getstatus_client = {
    .unmount_volume = NULL,
    .log_for_client = getstatus_log_for_client,
    .forced_ending_hook = NULL,
    .scan_extra_fds = NULL,
    .loop_started = NULL,
};

const char *flag_descriptions[FLAG_COUNT] = {
    "SupportsCopyFile",
    "SupportsChgPwd",
    "DontAllowSavePwd",
    "SupportsServerMessages",
    "SupportsServerSignature",
    "SupportsTCP/IP",
    "SupportsSrvrNotifications",
    "SupportsReconnect",
    "SupportsOpenDirectory",
    "SupportsUTF8Servername",
    "SupportsUUIDs",
    "SupportsExtSleep",
    "Undocumented Bit12 (Supports GSS-UAM SPNEGO blob)",
    "Undocumented Bit13",
    "Undocumented Bit14",
    "SupportsSuperClient"
};

char **parse_afp_flags(uint16_t flags, int *count)
{
    char **flags_list = malloc(FLAG_COUNT * sizeof(char *));

    if (!flags_list) {
        return NULL;
    }

    int flag_index = 0;

    for (int i = 0; i < FLAG_COUNT; i++) {
        if (flags & (1 << i)) {
            flags_list[flag_index] = malloc(50);

            if (!flags_list[flag_index]) {
                for (int j = 0; j < flag_index; j++) {
                    free(flags_list[j]);
                }

                free(flags_list);
                return NULL;
            }

            snprintf(flags_list[flag_index], 50, "\t%s", flag_descriptions[i]);
            flag_index++;
        }
    }

    *count = flag_index;
    return flags_list;
}

void draw_icon(int offset, char icon[])
{
    int cols = 0;
    int i, j;

    /* icons are 32x32 bitmaps; 128-byte icon + 128-byte mask */
    for (i = 0; i < AFP_SERVER_ICON_LEN; i++) {
        char c = icon[i + offset];

        for (j = 7; j >= 0; j--) {
            if (c & (1 << j)) {
                printf("#");
            } else {
                printf(" ");
            }
        }

        cols++;

        if (cols == 4) {
            cols = 0;
            printf("\n");
        }
    }

    printf("\n");
}

static int getstatus(char *address_string, unsigned int port)
{
    struct afp_server *server;
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *p;
    int ret;
    struct afp_versions *tmpversion;
    char ipstr[INET6_ADDRSTRLEN];
    char port_str[6];
    int count;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%u", port);

    if ((ret = getaddrinfo(address_string, port_str, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    if (res->ai_family == AF_INET6) {
        printf("AFP response from [%s]:%d via IPv6\n", address_string, port);
    } else if (res->ai_family == AF_INET) {
        printf("AFP response from %s:%d via IPv4\n", address_string, port);
    } else {
        printf("AFP response from %s:%d via unknown address family\n",
               address_string, port);
    }

    server = afp_server_init(res);
    ret = afp_server_connect(server, 1);

    if (ret < 0) {
        perror("Connecting to server");
        freeaddrinfo(res);
        return -1;
    }

    char **flags = parse_afp_flags(server->flags, &count);
    printf("Server name: %s\n", server->server_name_printable);
    printf("Server type: %s\n", server->machine_type);
    printf("AFP versions:\n");

    for (int j = 0; j < SERVER_MAX_VERSIONS; j++) {
        for (tmpversion = afp_versions; tmpversion->av_name; tmpversion++) {
            if (tmpversion->av_number == server->versions[j]) {
                printf("\t%s\n", tmpversion->av_name);
                break;
            }
        }
    }

    printf("UAMs:\n");

    for (int j = 1; j < 0x200; j <<= 1) {
        if (j & server->supported_uams) {
            printf("\t%s\n", uam_bitmap_to_string(j));
        }
    }

    printf("Flags:\n");

    if (flags) {
        for (int i = 0; i < count; i++) {
            printf("%s\n", flags[i]);
            free(flags[i]);
        }

        free(flags);
    }

    printf("Signature:\n\t");

    for (int j = 0; j < AFP_SIGNATURE_LEN; j++) {
        printf("%02x ", (unsigned char)server->signature[j]);
    }

    printf("\n\t");

    for (int j = 0; j < AFP_SIGNATURE_LEN; j++) {
        unsigned char c = (unsigned char)server->signature[j];

        if (c >= 32 && c <= 126) {
            printf("%c", c);
        } else {
            printf(".");
        }
    }

    printf("\n");

    for (p = res; p != NULL; p = p->ai_next) {
        void *addr;
        char *ipver;

        if (p->ai_family == AF_INET) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
            ipver = "IPv4";
        } else {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
            ipver = "IPv6";
        }

        inet_ntop(p->ai_family, addr, ipstr, sizeof(ipstr));
        printf("Resolved %s address: %s\n", ipver, ipstr);
    }

    if (show_icon) {
        draw_icon(0, server->icon);
    }

    /* Guest authentication and volume listing */
    if (!(server->supported_uams & UAM_NOUSERAUTHENT)) {
        printf("Shared volumes:\n\t(guest access not supported)\n");
        freeaddrinfo(res);
        afp_server_remove(server);
        return 0;
    }

    unsigned char versions[SERVER_MAX_VERSIONS];
    unsigned int uams = server->supported_uams;
    memcpy(versions, server->versions, SERVER_MAX_VERSIONS);
    loop_disconnect(server);

    if (afp_server_connect(server, 0) != 0) {
        fprintf(stderr, "Reconnect failed\n");
        freeaddrinfo(res);
        afp_server_remove(server);
        return -1;
    }

    if (afp_server_complete_connection(
                NULL, server, versions, uams,
                "", "", 0, UAM_NOUSERAUTHENT) == NULL) {
        /* afp_server_complete_connection calls afp_server_remove on failure */
        freeaddrinfo(res);
        return -1;
    }

    printf("Shared volumes:\n");

    for (int i = 0; i < server->num_volumes; i++) {
        printf("\t%s\n", server->volumes[i].volume_name_printable);
    }

    afp_logout(server, 1);
    freeaddrinfo(res);
    afp_server_remove(server);
    return 0;
}

static void usage(void)
{
    printf("Netatalk Client %s - get Apple Filing Protocol server status\n"
           "Usage:\n"
           "\tgetstatus [afp_url|ipaddress[:port]] [-i] [-v loglevel]\n"
           "Options:\n"
           "\t-i             Show server icon\n"
           "\t-v loglevel    Set log level (debug, info, notice, warning, error)\n"
           "\t-h             Show this help\n", NETATALK_CLIENT_VERSION);
}

void afp_wait_for_started_loop(void);

static void bail_out(int signum)
{
    (void)signum;
    _exit(1);
}

int main(int argc, char *argv[])
{
    int option_index = 0;
    int c;
    int log_level = LOG_WARNING;
    struct option long_options[] = {
        {"help", 0, 0, 'h'},
        {"icon", 0, 0, 'i'},
        {"loglevel", 1, 0, 'v'},
        {NULL, 0, NULL, 0},
    };
    unsigned int port = 548;
    struct afp_url url;
    char *servername = NULL;

    while (1) {
        c = getopt_long(argc, argv, "hiv:",
                        long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();
            return 0;

        case 'i':
            show_icon = true;
            break;

        case 'v': {
            int parsed_loglevel;

            if (string_to_log_level(optarg, &parsed_loglevel) != 0) {
                printf("Unknown log level %s\n", optarg);
                usage();
                return -1;
            }

            log_level = parsed_loglevel;
            break;
        }

        default:
            usage();
            return -1;
        }
    }

    if (optind < argc) {
        servername = argv[optind];
    }

    if (servername == NULL) {
        usage();
        return -1;
    }

    log_min_rank = loglevel_to_rank(log_level);
    libafpclient_register(&getstatus_client);
    afp_default_url(&url);
    /* Prepend afp:// if not already an AFP URL, and wrap bare IPv6
     * addresses in brackets so the URL parser doesn't treat the
     * colons as port delimiters */
    char url_buf[AFP_MAX_PATH];

    if (strncmp(servername, "afp://", 6) == 0) {
        snprintf(url_buf, sizeof(url_buf), "%s", servername);
    } else if (strchr(servername, ':') && servername[0] != '[') {
        snprintf(url_buf, sizeof(url_buf), "afp://[%s]", servername);
    } else {
        snprintf(url_buf, sizeof(url_buf), "afp://%s", servername);
    }

    if (afp_parse_url(&url, url_buf) != 0) {
        fprintf(stderr, "Could not parse address: %s\n", servername);
        usage();
        return -1;
    }

    servername = url.servername;
    port = url.port;
    afp_main_quick_startup(NULL);
    afp_wait_for_started_loop();

    if (init_uams() < 0) {
        return -1;
    }

    signal(SIGINT, bail_out);

    if (getstatus(servername, port) == 0) {
        return 0;
    } else {
        return -1;
    }
}
