#ifndef NETATALK_CLIENT_TYPES_H
#define NETATALK_CLIENT_TYPES_H

#include <stdint.h>

#define AFPC_SERVER_NAME_UTF8_LEN 255
#define AFPC_VOLUME_NAME_UTF8_LEN 255
#define AFPC_SIGNATURE_LEN 16
#define AFPC_MACHINE_TYPE_LEN 16
#define AFPC_SERVER_ICON_LEN 256
#define AFPC_MAX_USERNAME_LEN 127
#define AFPC_MAX_PASSWORD_LEN 127
#define AFPC_ZONE_LEN 32
#define AFPC_MAX_PATH 768
#define AFPC_MAX_VERSIONS 10

enum afpc_network_transport {
    AFPC_TRANSPORT_TCPIP,
    AFPC_TRANSPORT_APPLETALK,
};

struct afpc_url {
    enum afpc_network_transport protocol;
    char username[AFPC_MAX_USERNAME_LEN];
    char uamname[50];
    char password[AFPC_MAX_PASSWORD_LEN];
    char servername[AFPC_SERVER_NAME_UTF8_LEN];
    int port;
    char volumename[AFPC_VOLUME_NAME_UTF8_LEN];
    char path[AFPC_MAX_PATH];
    int requested_version;
    char zone[AFPC_ZONE_LEN];
    char volpassword[9];
};

enum afpc_server_type {
    AFPC_SERVER_TYPE_UNKNOWN,
    AFPC_SERVER_TYPE_NETATALK,
    AFPC_SERVER_TYPE_AIRPORT,
    AFPC_SERVER_TYPE_MACINTOSH,
    AFPC_SERVER_TYPE_TIMECAPSULE,
    AFPC_SERVER_TYPE_WINDOWS,
};

struct afpc_server_info {
    char server_name_printable[AFPC_SERVER_NAME_UTF8_LEN];
    char machine_type[AFPC_MACHINE_TYPE_LEN];
    char icon[AFPC_SERVER_ICON_LEN];
    char signature[AFPC_SIGNATURE_LEN];
    unsigned char versions[AFPC_MAX_VERSIONS];
    unsigned int supported_uams;
    unsigned short flags;
    enum afpc_server_type server_type;
};

struct afpc_unix_privileges {
    uint32_t uid;
    uint32_t gid;
    uint32_t permissions;
    uint32_t ua_permissions;
};

struct afpc_file_info {
    char name[AFPC_MAX_PATH];
    unsigned int creation_date;
    unsigned int modification_date;
    struct afpc_unix_privileges unixprivs;
    unsigned long long size;
};

struct afpc_volume_info {
    char volume_name_printable[AFPC_VOLUME_NAME_UTF8_LEN];
    char flags;
};

struct afpc_server_handle;
struct afpc_volume_handle;

typedef struct afpc_server_handle *afpc_server_t;
typedef struct afpc_volume_handle *afpc_volume_t;

#endif
