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

static void escape_string(char * string, char c)
{
    char d;
    int inescape = 0;
    char tmpstring[1024];
    char *p = tmpstring;
    memset(tmpstring, 0, 1024);

    for (unsigned long i = 0; i < strlen(string); i++) {
        d = string[i]; /* convenience */

        if ((inescape) && (d == c)) {
            inescape = 0;
            continue;
        }

        *p = d;
        p++;

        if (d == c) {
            inescape = 1;
        }
    }

    strcpy(string, tmpstring);
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
int afp_parse_url(struct afpc_url * url, const char * toparse)
{
    char firstpart[AFP_HOSTNAME_LEN], secondpart[MAX_CLIENT_RESPONSE];
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
    url->path[0] = '\0';
    log_for_client(NULL, AFPFSD, LOG_DEBUG, "Parsing AFP URL: %s", toparse);

    /* if there is a ://, make sure it is preceeded by afp */

    if ((p = strstr(toparse, "://")) != NULL) {
        q = p - 3;

        if (p < toparse) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "URL does not start with afp://");
            return -1;
        }

        if (strncmp(q, "afp", 3) != 0) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "URL does not start with afp://");
            return -1;
        }

        p += 3;
    } else {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "This isn't a URL at all");
        return -1;
    }

    if (p == NULL) {
        p = (char *)toparse;
    }

    /* Now split on the first / */
    if (sscanf(p, "%[^/]/%[^$]",
               firstpart, secondpart) != 2) {
        /* Okay, so there's no volume. */
        skip_secondpart = 1;
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
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "Port appears to be zero");
            return -1;
        }
    }

    if (strlcpy(url->servername, p,
                sizeof(url->servername)) >= sizeof(url->servername)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Warning: servername truncated");
    }

    if (check_servername(url->servername)) {
        log_for_client(NULL, AFPFSD, LOG_ERR,
                       "This isn't a valid servername");
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
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Warning: password truncated");
        }
    }

    /* Now we're down to user[;AUTH=uamname] */
    p = firstpart;

    if ((q = strstr(p, ";AUTH="))) {
        *q = '\0';
        q += 6;

        if (strlcpy(url->uamname, q, sizeof(url->uamname)) >= sizeof(url->uamname)) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Warning: uamname truncated");
        }

        if (check_uamname(url->uamname)) {
            log_for_client(NULL, AFPFSD, LOG_ERR,
                           "This isn't a valid uamname");
            return -1;
        }
    }

    if (*p != '\0'
            && strlcpy(url->username, p, sizeof(url->username)) >= sizeof(url->username)) {
        log_for_client(NULL, AFPFSD, LOG_WARNING, "Warning: username truncated");
    }

parse_secondpart:

    if (skip_secondpart) {
        goto done;
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
        log_for_client(NULL, AFPFSD, LOG_WARNING,
                       "Warning: volumename truncated");
    }

    if (q) {
        url->path[0] = '/';

        if (strlcpy(url->path + 1, q, sizeof(url->path) - 1) >= sizeof(url->path) - 1) {
            log_for_client(NULL, AFPFSD, LOG_WARNING,
                           "Warning: path truncated");
        }
    }

done:
    escape_url(url);
    log_for_client(NULL, AFPFSD, LOG_DEBUG,
                   "Successful parsing of URL");
    return 0;
}
