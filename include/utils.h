#ifndef __UTILS_H_
#define __UTILS_H_
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>

#include "afp.h"

static inline uint64_t hton64(uint64_t value)
{
    if (htonl(UINT32_C(1)) == UINT32_C(1)) {
        return value;
    }

    return (uint64_t)htonl((uint32_t)(value >> 32))
           | ((uint64_t)htonl((uint32_t)value) << 32);
}

static inline uint64_t ntoh64(uint64_t value)
{
    return hton64(value);
}

#define min(a,b) (((a)<(b)) ? (a) : (b))
#define max(a,b) (((a)>(b)) ? (a) : (b))

unsigned char unixpath_to_afppath(
    struct afp_server * server,
    char *buf);
unsigned char sizeof_path_header(struct afp_server * server);

unsigned char copy_from_pascal(char *dest, char *pascal, unsigned int max_len) ;
unsigned short copy_from_pascal_two(char *dest, char *pascal,
                                    unsigned int max_len);

unsigned char copy_to_pascal(char *dest, const char *src);
unsigned short copy_to_pascal_two(char *dest, const char *src);

void copy_path(struct afp_server * server, char * dest, const char * pathname,
               size_t len);
char *create_path(struct afp_server * server, char * pathname,
                  unsigned short *len);

int invalid_filename(struct afp_server * server, const char * filename);

void trigger_exit(void);
void afp_set_auto_shutdown_on_unmount(int enabled);
int afp_get_auto_shutdown_on_unmount(void);

void sanitize_text(const char *text, char *sanitized, size_t size);

/* Log level conversion functions */
const char *log_level_to_string(int level);
int string_to_log_level(const char *str, int *level_out);
int loglevel_to_rank(int loglevel);

#endif
