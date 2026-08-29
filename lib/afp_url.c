/*
 *  afp_url.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "afp_internal.h"
#include "client.h"
#include "uam_registry.h"
#include "utils.h"

void afp_default_url(struct afpc_url *url)
{
    memset(url, 0, sizeof(struct afpc_url));
    url->protocol = AFPC_TRANSPORT_TCPIP;
    url->port = 548;
}

static int check_servername(char * servername)
{
    if (strchr(servername, '/')) {
        return -1;
    }

    return 0;
}

static int check_port(char * port)
{
    long long ret = 0;
    errno = 0;
    ret = strtol(port, NULL, 10);

    if ((ret < 0) || (ret > 32767)) {
        return -1;
    }

    if (errno) {
        printf("port error\n");
        return -1;
    }

    return 0;
}

static int check_uamname(char * uam)
{
    return !uam_string_to_bitmap(uam);
}

static void escape_string(char *string, char c)
{
    char *read_cursor = string;
    char *write_cursor = string;

    while (*read_cursor != '\0') {
        *write_cursor++ = *read_cursor;

        if (*read_cursor == c && read_cursor[1] == c) {
            read_cursor += 2;
        } else {
            read_cursor++;
        }
    }

    *write_cursor = '\0';
}

static void escape_url(struct afpc_url * url)
{
    escape_string(url->password, '@');
    escape_string(url->username, ':');
}


static char *escape_strrchr(const char * haystack, int c, const char *toescape)
{
    char *p;

    if (strchr(toescape, c) == NULL) {
        return strrchr(haystack, c);
    }

    if ((p = strrchr(haystack, c)) == NULL) {
        return NULL;
    }

    if (p == haystack) {
        return p;
    }

    if (*(p - 1) != c) {
        return p;
    }

    p -= 2;
    return escape_strrchr(p, c, toescape);
}

static char *escape_strchr(const char * haystack, int c, const char * toescape)
{
    char *p;
    size_t diff;

    if (strchr(toescape, c) == NULL) {
        return strchr(haystack, c);
    }

    if ((p = strchr(haystack, c)) == NULL) {
        return NULL;
    }

    diff = p - haystack;

    if (diff == strlen(haystack)) {
        return p;
    }

    if (*(p + 1) != c) {
        return p;
    }

    p += 2;
    return escape_strchr(p, c, toescape);
}

/* The most complex AFP URL is:
 *
 * afp://user;AUTH=uamname:password@server-name:port/volume-name/path
 *
 * where the optional parms are user, password, AUTH and port, so the
 * simplest is:
 *
 * afp://server-name/volume-name/path
 *
 */
static int parse_url(struct afpc_url *url, const char *toparse,
                     int log_messages)
{
    /* Escaped delimiters can make the authority longer than any individual
     * decoded field. */
    char firstpart[AFPC_MAX_USERNAME_BYTES
                   + (2U * AFPC_MAX_PASSWORD_LEN)
                   + AFP_HOSTNAME_LEN + 128U];
    char *secondpart = NULL;
    const char *path_part;
    char *p, *q;
    int firstpartlen;
    int skip_earliestpart = 0;
    int skip_secondpart = 0;
    char *lastchar;
    int foundv6literal = 0;
    url->username[0] = '\0';
    url->servername[0] = '\0';
    url->uamname[0] = '\0';
    url->password[0] = '\0';
    url->volumename[0] = '\0';
    afpc_path_clear(&url->path);
#define URL_LOG(level, ...) \
    do { \
        if (log_messages) { \
            log_for_client(NULL, AFPFSD, level, __VA_ARGS__); \
        } \
    } while (0)
    /* Never log the URL: its authority may contain authentication data. */
    URL_LOG(LOG_DEBUG, "Parsing AFP URL");

    /* if there is a ://, make sure it is preceeded by afp */

    if ((p = strstr(toparse, "://")) != NULL) {
        q = p - 3;

        if (p < toparse) {
            URL_LOG(LOG_ERR, "URL does not start with afp://");
            return -1;
        }

        if (strncmp(q, "afp", 3) != 0) {
            URL_LOG(LOG_ERR, "URL does not start with afp://");
            return -1;
        }

        p += 3;
    } else {
        URL_LOG(LOG_ERR, "This isn't a URL at all");
        return -1;
    }

    if (p == NULL) {
        p = (char *)toparse;
    }

    /* Split the authority from the volume/path without putting a full path
     * into a fixed parser buffer. */
    path_part = strchr(p, '/');

    if (!path_part) {
        if (strlcpy(firstpart, p, sizeof(firstpart)) >= sizeof(firstpart)) {
            URL_LOG(LOG_ERR, "Server authority is too long");
            return -1;
        }

        skip_secondpart = 1;
    } else {
        size_t firstpart_len = (size_t)(path_part - p);

        if (firstpart_len >= sizeof(firstpart)) {
            URL_LOG(LOG_ERR, "Server authority is too long");
            return -1;
        }

        memcpy(firstpart, p, firstpart_len);
        firstpart[firstpart_len] = '\0';
    }

    firstpartlen = strlen(firstpart);
    lastchar = firstpart + firstpartlen - 1;

    /* First part could be something like:
    	user;AUTH=uamname:password

       We'll assume that the breakout is:
                user;  optional user name
            AUTH=uamname:
    */

    /* Let's see if there's a ';'.  q is the end of the username */

    if ((p = escape_strchr(firstpart, '@', "@"))) {
        *p = '\0';
        p++;
    } else {
        skip_earliestpart = 1;
        p = firstpart;
    }

    /* p now points to the start of the server name*/

    /* square brackets denote a literal ipv6 address */
    if (*p == '[' &&
            (q = strchr(p, ']'))) {
        foundv6literal = 1;
        p++;
        *q = '\0';
        q++;
    }

    /* see if we have a port number */

    if ((foundv6literal && (q = strchr(q, ':'))) ||
            (!foundv6literal && (q = strchr(p, ':')))) {
        *q = '\0';
        q++;

        if (check_port(q)) {
            return -1;
        }

        if ((url->port = atoi(q)) == 0) {
            URL_LOG(LOG_ERR, "Port appears to be zero");
            return -1;
        }
    }

    if (strlcpy(url->servername, p,
                sizeof(url->servername)) >= sizeof(url->servername)) {
        URL_LOG(LOG_WARNING, "Warning: servername truncated");
    }

    if (check_servername(url->servername)) {
        URL_LOG(LOG_ERR, "This isn't a valid servername");
        return -1;
    }

    if ((p == NULL) || ((strlen(p) + p - 1) == lastchar)) {
        /* afp://server */
    }

    if ((q) && ((strlen(q) + q - 1) == lastchar)) {
        /* afp://server:port */
    }

    /* Earliest part */

    if (skip_earliestpart) {
        p += strlen(p);
        goto parse_secondpart;
    }

    p = firstpart;

    /* Now we're left with something like user[;AUTH=uamname][:password] */

    /* Look for :password */

    if ((q = escape_strrchr(p, ':', ":"))) {
        *q = '\0';
        q++;

        if (strlcpy(url->password, q, sizeof(url->password)) >= sizeof(url->password)) {
            URL_LOG(LOG_ERR, "Password is too long");
            return -1;
        }
    }

    /* Now we're down to user[;AUTH=uamname] */
    p = firstpart;

    if ((q = strstr(p, ";AUTH="))) {
        *q = '\0';
        q += 6;

        if (strlcpy(url->uamname, q, sizeof(url->uamname)) >= sizeof(url->uamname)) {
            URL_LOG(LOG_ERR, "UAM name is too long");
            return -1;
        }

        if (check_uamname(url->uamname)) {
            URL_LOG(LOG_ERR, "This isn't a valid uamname");
            return -1;
        }
    }

    if (*p != '\0'
            && strlcpy(url->username, p, sizeof(url->username)) >= sizeof(url->username)) {
        URL_LOG(LOG_ERR, "Username is too long");
        return -1;
    }

parse_secondpart:

    if (skip_secondpart) {
        goto done;
    }

    secondpart = strdup(path_part + 1);

    if (!secondpart) {
        return -1;
    }

    if (secondpart[0] == '\0') {
        goto done;
    }

    {
        size_t splen = strlen(secondpart);

        if (splen > 0 && secondpart[splen - 1] == '/') {
            secondpart[splen - 1] = '\0';
        }
    }

    p = secondpart;

    if ((q = strchr(p, '/'))) {
        *q = '\0';
        q++;
    }

    if (strlcpy(url->volumename, p,
                sizeof(url->volumename)) >= sizeof(url->volumename)) {
        URL_LOG(LOG_WARNING, "Warning: volumename truncated");
    }

    if (q && afpc_path_join(&url->path, "/", q,
                            AFPC_MAX_UTF8_PATH_BYTES) < 0) {
        URL_LOG(LOG_ERR, "AFP path exceeds the UTF-8 wire limit");
        free(secondpart);
        return -1;
    }

done:
    escape_url(url);

    if (afp_validate_username(url->username, NULL) != 0) {
        URL_LOG(LOG_ERR, "Username is not valid UTF-8 or exceeds 255 characters");
        free(secondpart);
        return -1;
    }

    free(secondpart);
    URL_LOG(LOG_DEBUG, "Successful parsing of URL");
#undef URL_LOG
    return 0;
}

int afp_parse_url(struct afpc_url *url, const char *text)
{
    return parse_url(url, text, 1);
}

int afp_parse_url_quiet(struct afpc_url *url, const char *text)
{
    return parse_url(url, text, 0);
}
