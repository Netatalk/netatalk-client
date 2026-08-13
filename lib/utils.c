/*
 *  utils.c
 *
 *  Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afp_internal.h"
#include "afp_protocol.h"
#include "unicode.h"
#include "utils.h"

struct afp_path_header_long {
    unsigned char type;
    unsigned char len;
}  __attribute__((__packed__)) ;

struct afp_path_header_unicode {
    uint8_t type;
    uint32_t hint;
    uint16_t unicode;
}  __attribute__((__packed__)) ;

int afp_parse_version(const char *text, int *version_out)
{
    long version;
    char *end;

    if (!text || !version_out) {
        return -1;
    }

    if (strchr(text, '.')) {
        long major, minor;
        char trailing;

        if (sscanf(text, "%ld.%ld%c", &major, &minor, &trailing) != 2
                || major < 0 || minor < 0 || minor > 9
                || major > (INT_MAX - minor) / 10) {
            return -1;
        }

        version = major * 10 + minor;
    } else {
        errno = 0;
        version = strtol(text, &end, 10);

        if (errno != 0 || end == text || *end != '\0') {
            return -1;
        }
    }

    if (version < 0 || version > INT_MAX) {
        return -1;
    }

    *version_out = (int)version;
    return 0;
}

unsigned short utf8_to_string(char * dest, char * buf, unsigned short maxlen)
{
    return copy_from_pascal_two(dest, buf + 4, maxlen);
}

unsigned char unixpath_to_afppath(
    const struct afp_volume *volume,
    char *buf)
{
    unsigned char encoding = volume->path_encoding;
    char *p = NULL, *end;
    unsigned short len = 0;

    switch (encoding) {
    case kFPUTF8Name: {
        unsigned short *len_p = (unsigned short *)(buf + 5);
        p = buf + 7;
        len = ntohs(*len_p);
    }
    break;

    case kFPLongName: {
        unsigned char *len_p = (unsigned char *)(buf + 1);
        p = buf + 2;
        len = *len_p;
    }
    }

    end = p + len;

    while (p < end) {
        if (*p == '/') {
            *p = '\0';  /* Unix path separator becomes null */
        } else if (*p == ':') {
            *p = '/';   /* Colon in Unix filename becomes slash for AFP/Mac */
        }

        p++;
    }

    return 0;
}

void afp_unixpriv_to_stat(struct afp_file_info *fp,
                          struct stat *stat)
{
    memset(stat, 0, sizeof(*stat));

    if (fp->unixprivs.permissions) {
        stat->st_mode = fp->unixprivs.permissions;
    } else {
        stat->st_mode = fp->unixprivs.ua_permissions;
    }

    stat->st_uid = fp->unixprivs.uid;
    stat->st_gid = fp->unixprivs.gid;
}


unsigned char copy_from_pascal(char *dest, char *pascal, unsigned int max_len)
{
    unsigned char len;
    unsigned char copy_len;

    if (!pascal || max_len == 0) {
        return 0;
    }

    len = *pascal;
    copy_len = len;

    /* Reserve one byte for null terminator */
    if (copy_len >= max_len) {
        copy_len = max_len - 1;
    }

    memset(dest, 0, max_len);
    memcpy(dest, pascal + 1, copy_len);
    /* Return original length so caller can advance pointer correctly */
    return len;
}

unsigned short copy_from_pascal_two(char *dest, char *pascal,
                                    unsigned int max_len)
{
    unsigned short len;
    unsigned short copy_len;

    if (!pascal || max_len == 0) {
        return 0;
    }

    unsigned short *len_p = (unsigned short *) pascal;
    len = ntohs(*len_p);

    if (len == 0) {
        return 0;
    }

    copy_len = len;

    /* Reserve one byte for null terminator */
    if (copy_len >= max_len) {
        copy_len = max_len - 1;
    }

    memset(dest, 0, max_len);
    memcpy(dest, pascal + 2, copy_len);
    /* Return original length so caller can advance pointer correctly */
    return len;
}

unsigned char copy_to_pascal(char *dest, const char *src)
{
    unsigned char len = (unsigned char)strnlen(src, UINT8_MAX);
    dest[0] = len;
    memcpy(dest + 1, src, len);
    return len;
}

unsigned short copy_to_pascal_two(char *dest, const char *src)
{
    unsigned short *sendlen = (void *) dest;
    char *data = dest + 2;
    unsigned short len ;

    if (!src) {
        dest[0] = 0;
        dest[1] = 0;
        return 2;
    }

    len = (unsigned short) strlen(src);
    *sendlen = htons(len);
    memcpy(data, src, len);
    return len;
}

unsigned char sizeof_path_header(const struct afp_volume *volume)
{
    switch (volume->path_encoding) {
    case kFPUTF8Name:
        return (sizeof(struct afp_path_header_unicode));

    case kFPLongName:
        return (sizeof(struct afp_path_header_long));
    }

    return 0;
}


void copy_path(
    const struct afp_volume *volume,
    char *dest,
    const char *pathname,
    size_t len
)
{
    unsigned char encoding = volume->path_encoding;
    struct afp_path_header_unicode * header_unicode = (void *) dest;
    struct afp_path_header_long * header_long = (void *) dest;

    if (!pathname) {
        pathname = "";
        len = 0;
    }

    switch (encoding) {
    case kFPUTF8Name:
        if (len > UINT16_MAX) {
            len = UINT16_MAX;
        }

        header_unicode->type = encoding;
        header_unicode->hint = htonl(0x08000103);
        header_unicode->unicode = htons((uint16_t)len);
        memcpy(dest + sizeof(struct afp_path_header_unicode), pathname, len);
        break;

    case kFPLongName:
        if (len > UINT8_MAX) {
            len = UINT8_MAX;
        }

        header_long->type = encoding;
        header_long->len = (uint8_t)len;
        memcpy(dest + sizeof(struct afp_path_header_long), pathname, len);
    }
}

int invalid_filename(const struct afp_volume *volume, const char *filename)
{
    const struct afp_server *server;
    size_t maxlen;
    const char *p, *q;

    if (!volume || !volume->server || !volume->server->using_version
            || !filename) {
        return 1;
    }

    server = volume->server;

    if ((filename[0] == '/') && (filename[1] == '\0')) {
        return 0;
    }

    if (server->using_version->av_number < 30) {
        maxlen = 31;
    } else if (volume->path_encoding == kFPUTF8Name) {
        maxlen = AFP_MAX_UTF8_NAME_CHARS;
    } else {
        maxlen = 255;
    }

    p = filename;

    while (*p) {
        while (*p == '/') {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        q = strchr(p, '/');

        if (volume->path_encoding == kFPUTF8Name
                && server->using_version->av_number >= 30) {
            size_t component_len = q ? (size_t)(q - p) : strlen(p);

            if (mbStrLen(p, component_len) > maxlen) {
                return 1;
            }
        } else if (q ? (size_t)(q - p) > maxlen : strlen(p) > maxlen) {
            return 1;
        }

        if (!q) {
            break;
        }

        p = q + 1;
    }

    return 0;
}

void sanitize_text(const char *text, char *sanitized, size_t size)
{
    size_t pos = 0;

    if (size == 0) {
        return;
    }

    if (!text) {
        sanitized[0] = '\0';
        return;
    }

    while (*text && pos + 1 < size) {
        unsigned char ch = (unsigned char) * text++;

        if (ch == '\r' || ch == '\n' || ch == '\t') {
            char escaped;

            if (ch == '\r') {
                escaped = 'r';
            } else if (ch == '\n') {
                escaped = 'n';
            } else {
                escaped = 't';
            }

            if (pos + 2 >= size) {
                break;
            }

            sanitized[pos++] = '\\';
            sanitized[pos++] = escaped;
        } else if (ch < 0x20 || ch == 0x7f) {
            if (pos + 4 >= size) {
                break;
            }

            static const char hex[] = "0123456789abcdef";
            sanitized[pos++] = '\\';
            sanitized[pos++] = 'x';
            sanitized[pos++] = hex[ch >> 4];
            sanitized[pos++] = hex[ch & 0x0f];
        } else {
            sanitized[pos++] = (char)ch;
        }
    }

    sanitized[pos] = '\0';
}

const char *log_level_to_string(int level)
{
    switch (level) {
    case LOG_DEBUG:
        return "debug";

    case LOG_INFO:
        return "info";

    case LOG_NOTICE:
        return "notice";

    case LOG_WARNING:
        return "warning";

    case LOG_ERR:
        return "err";

    default:
        return "notice";
    }
}

int string_to_log_level(const char *str, int *level_out)
{
    if (!str || !level_out) {
        return -1;
    }

    if (strcasecmp(str, "debug") == 0 || strcasecmp(str, "LOG_DEBUG") == 0) {
        *level_out = LOG_DEBUG;
    } else if (strcasecmp(str, "info") == 0 || strcasecmp(str, "LOG_INFO") == 0) {
        *level_out = LOG_INFO;
    } else if (strcasecmp(str, "notice") == 0
               || strcasecmp(str, "LOG_NOTICE") == 0) {
        *level_out = LOG_NOTICE;
    } else if (strcasecmp(str, "warning") == 0 || strcasecmp(str, "warn") == 0 ||
               strcasecmp(str, "LOG_WARNING") == 0) {
        *level_out = LOG_WARNING;
    } else if (strcasecmp(str, "err") == 0 || strcasecmp(str, "error") == 0 ||
               strcasecmp(str, "LOG_ERR") == 0) {
        *level_out = LOG_ERR;
    } else {
        return -1;
    }

    return 0;
}

int loglevel_to_rank(int loglevel)
{
    switch (loglevel) {
    case LOG_DEBUG:
        return 0;

    case LOG_INFO:
        return 1;

    case LOG_NOTICE:
        return 2;

    case LOG_WARNING:
        return 3;

    case LOG_ERR:
        return 4;

    default:
        return 4; /* Treat unknown as error-level to avoid dropping */
    }
}
