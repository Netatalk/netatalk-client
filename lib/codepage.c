/*
 *  codepage.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2024-2026 Daniel Markstedt <daniel@mindani.net>
 *
 *  These routines handle code page conversions.
 *
 *  Supports UTF-8 (kFPUTF8Name) and Mac Roman (kFPLongName) encodings.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afp_protocol.h"
#include "mac_roman.h"
#include "unicode.h"
#include "utils.h"

int convert_utf8dec_to_utf8pre(char *src, int src_len,
                               char *dest, int dest_len);
int convert_utf8pre_to_utf8dec(char * src, int src_len,
                               char *dest, int dest_len);

/*
 * convert_mac_roman_to_utf8()
 *
 * Convert a Mac Roman encoded string to UTF-8.
 * Mac Roman bytes 0x00-0x7F are ASCII, 0x80-0xFF are looked up
 * in the mac_roman_2uni[] table to get UCS-2, then converted to UTF-8.
 */
int convert_mac_roman_to_utf8(const char *src, int src_len,
                              char *dest, int dest_len)
{
    char16 ucs2buf[256];
    int ucs2_len = 0;

    if (!src || !dest || src_len <= 0 || dest_len <= 0) {
        return -1;
    }

    for (int i = 0; i < src_len && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];

        if (ucs2_len >= 255) {
            break;
        }

        if (c < 0x80) {
            ucs2buf[ucs2_len++] = (char16)c;
        } else {
            ucs2buf[ucs2_len++] = mac_roman_2uni[c - 0x80];
        }
    }

    ucs2buf[ucs2_len] = 0;
    return UCS2toUTF8(ucs2buf, ucs2_len, dest, dest_len);
}

/*
 * unicode_to_mac_roman()
 *
 * Look up a single UCS-2 codepoint in the reverse page tables.
 * Returns the Mac Roman byte, or 0 if the codepoint has no mapping.
 */
static unsigned char unicode_to_mac_roman(char16 uc)
{
    unsigned char r;

    if (uc >= 0x00A0 && uc <= 0x00FF) {
        r = mac_roman_page00[uc - 0x00A0];

        if (r) {
            return r;
        }
    } else if (uc >= 0x0131 && uc <= 0x0192) {
        r = mac_roman_page01[uc - 0x0131];

        if (r) {
            return r;
        }
    } else if (uc >= 0x02C6 && uc <= 0x02DD) {
        r = mac_roman_page02[uc - 0x02C6];

        if (r) {
            return r;
        }
    } else if (uc == 0x03C0) {
        return 0xB9;
    } else if (uc >= 0x2013 && uc <= 0x2044) {
        r = mac_roman_page20[uc - 0x2013];

        if (r) {
            return r;
        }
    } else if (uc == 0x20AC) {
        return 0xDB;
    } else if (uc >= 0x2122 && uc <= 0x2126) {
        r = mac_roman_page21[uc - 0x2122];

        if (r) {
            return r;
        }
    } else if (uc >= 0x2202 && uc <= 0x2265) {
        r = mac_roman_page22[uc - 0x2202];

        if (r) {
            return r;
        }
    } else if (uc == 0x25CA) {
        return 0xD7;
    } else if (uc == 0xF8FF) {
        return 0xF0;
    } else if (uc >= 0xFB01 && uc <= 0xFB02) {
        return mac_roman_pagefb[uc - 0xFB01];
    }

    return 0;
}

/*
 * convert_utf8_to_mac_roman()
 *
 * Convert a UTF-8 string to Mac Roman encoding.
 * First converts UTF-8 to UCS-2 using UTF8toUCS2(), then maps each
 * codepoint to Mac Roman using the reverse page tables.
 * Unmappable characters are replaced with '?'.
 */
static int convert_utf8_to_mac_roman(const char *src, int src_len,
                                     char *dest, int dest_len)
{
    char16 *ucs2;
    size_t bytes_consumed;
    int out = 0;

    if (!src || !dest || src_len <= 0 || dest_len <= 0) {
        return -1;
    }

    ucs2 = UTF8toUCS2(src, src_len, &bytes_consumed);

    if (!ucs2) {
        return -1;
    }

    for (int i = 0; ucs2[i] != 0; i++) {
        if (out >= dest_len - 1) {
            break;
        }

        char16 c = ucs2[i];

        if (c < 0x80) {
            dest[out++] = (char)c;
        } else {
            unsigned char mr = unicode_to_mac_roman(c);
            dest[out++] = mr ? (char)mr : '?';
        }
    }

    dest[out] = '\0';
    free(ucs2);
    return 0;
}

/*
 * convert_path_to_unix()
 *
 * This converts an AFP-generated path to Unix's UTF8.  This function
 * does the appropriate encoding lookup.
 */

int convert_path_to_unix(char encoding, char * dest,
                         char *src, int dest_len)
{
    char *p;

    if (!src || !dest || dest_len <= 0) {
        return -1;
    }

    memset(dest, 0, dest_len);

    switch (encoding) {
    case kFPUTF8Name:
        convert_utf8dec_to_utf8pre(src, strnlen(src, dest_len), dest, dest_len);
        break;

    case kFPLongName:
        convert_mac_roman_to_utf8(src, strnlen(src, dest_len), dest, dest_len);
        break;

    default:
        return -1;
    }

    /* Convert AFP/Mac path separators back to Unix filename characters
     * This is the reverse of what unixpath_to_afppath() does:
     * - '/' in AFP filenames (which were ':' in Unix) → ':'
     */
    p = dest;

    while (*p && (p < dest + dest_len - 1)) {
        if (*p == '/') {
            *p = ':';  /* Slash in AFP filename becomes colon in Unix */
        }

        p++;
    }

    return 0;
}

/*
 * convert_path_to_afp()
 *
 * Given a null terminated source, converts the path to an AFP path
 * given the encoding.
 */

int convert_path_to_afp(char encoding, char * dest,
                        char *src, int dest_len)
{
    if (!src || !dest || dest_len <= 0) {
        return -1;
    }

    memset(dest, 0, dest_len);

    switch (encoding) {
    case kFPUTF8Name:
        convert_utf8pre_to_utf8dec(src, strnlen(src, dest_len), dest, dest_len);
        break;

    case kFPLongName:
        convert_utf8_to_mac_roman(src, strnlen(src, dest_len), dest, dest_len);
        break;

    default:
        return -1;
    }

    return 0;
}

/* convert_utf8dec_to_utf8pre()
 *
 * Conversion for text from Decomposed UTF8 used in AFP to Precomposed
 * UTF8 used elsewhere.
 *
 * This is for converting *from* UTF-8-MAC
 */

int convert_utf8dec_to_utf8pre(char *src, int src_len,
                               char *dest, int dest_len)
{
    char16 *path16dec, c, prev, *p16dec, *p16pre;
    char16 path16pre[384];  /* max 127 * 3 byte UTF8 characters */
    int comp;
    size_t bytes_consumed;
    size_t ucs2_len;

    if (!src || !dest || src_len <= 0 || dest_len <= 0) {
        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    path16dec = UTF8toUCS2(src, src_len, &bytes_consumed);

    if (!path16dec) {
        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    p16dec = path16dec;
    p16pre = path16pre;
    prev = 0;
    ucs2_len = 0;

    while (*p16dec != 0 && ucs2_len < 383) {
        c = *p16dec;

        if (prev > 0) {
            comp = UCS2precompose(prev, c);

            if (comp != -1) {
                /* Keep and try to combine again on next loop */
                prev = (char16)comp;
            } else {
                *p16pre = prev;
                prev = c;
                p16pre++;
                ucs2_len++;
            }
        } else {
            prev = c;
        }

        p16dec++;

        if (*p16dec == 0 && ucs2_len < 383) {
            /* Add last char */
            *p16pre = prev;
            p16pre++;
            ucs2_len++;
        }
    }

    *p16pre = 0; /* Terminate string */

    if (UCS2toUTF8(path16pre, ucs2_len, dest, dest_len) != 0) {
        free(path16dec);

        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    free(path16dec);
    return 0;
}

static void decompose_char(char16 c, char16 **dest)
{
    char16 first, second;

    if (UCS2decompose(c, &first, &second)) {
        decompose_char(first, dest);
        decompose_char(second, dest);
    } else {
        **dest = c;
        (*dest)++;
    }
}

/* convert_utf8pre_to_utf8dec()
 *
 * Conversion for text from Precomposed UTF8 to Decomposed UTF8.
 *
 */

int convert_utf8pre_to_utf8dec(char *src, int src_len,
                               char *dest, int dest_len)
{
    char16 *path16pre;
    char16 *path16dec, *p16dec;
    const char16 *p16pre;
    size_t ucs2len;
    size_t bytes_consumed;

    if (!src || !dest || src_len <= 0 || dest_len <= 0) {
        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    path16pre = UTF8toUCS2(src, src_len, &bytes_consumed);

    if (!path16pre) {
        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    ucs2len = str16len(path16pre, src_len);
    /* Allocate enough space. Max decomposition expansion is usually small. */
    path16dec = malloc((ucs2len * 4 + 1) * sizeof(char16));

    if (!path16dec) {
        free(path16pre);

        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    p16pre = path16pre;
    p16dec = path16dec;

    while (*p16pre != 0) {
        decompose_char(*p16pre, &p16dec);
        p16pre++;
    }

    *p16dec = 0;

    if (UCS2toUTF8(path16dec, p16dec - path16dec, dest, dest_len) != 0) {
        free(path16pre);
        free(path16dec);

        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    free(path16pre);
    free(path16dec);
    return 0;
}
