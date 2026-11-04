#ifndef __CODE_PAGE_H_
#define __CODE_PAGE_H_

#include <stddef.h>

int convert_utf8dec_to_utf8pre(const char *src, int src_len,
                               char *dest, int dest_len);
int convert_utf8pre_to_utf8dec(const char *src, int src_len,
                               char *dest, int dest_len);
int convert_mac_roman_to_utf8(const char *src, int src_len,
                              char *dest, int dest_len);
int convert_name_to_unix(char encoding, const char *src, size_t src_len,
                         char *dest, size_t dest_len);
int convert_path_to_unix(char encoding, char *dest,
                         char *src, int dest_len);
int convert_path_to_afp(char encoding, char *dest,
                        const char *src, int dest_len);
#endif
