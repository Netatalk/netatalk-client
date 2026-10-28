/*
 *  codepage.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2024-2026 Daniel Markstedt <daniel@mindani.net>
 *  Copyright (C) 2026 Andy Lemin (andylemin)
 *
 *  This program can be distributed under the terms of the GNU GPL.
 *  See the file COPYING.
 */

/*!
 * @file
 * @brief Name conversion between client UTF-8 and the AFP name encodings
 *
 * Supports UTF-8 (kFPUTF8Name) and Mac Roman (kFPLongName) encodings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afp_protocol.h"
#include "mac_roman.h"
#include "unicode.h"
#include "utils.h"

int convert_utf8dec_to_utf8pre(const char *src, int src_len,
                               char *dest, int dest_len);
int convert_utf8pre_to_utf8dec(const char *src, int src_len,
                               char *dest, int dest_len);

/*!
 * @brief Tell whether a UTF-8 string has a supplementary-plane character
 *
 * UCS-2 cannot hold one, so such a path passes verbatim, not decomposed.
 *
 * @param[in]  src      UTF-8 string
 * @param[in]  src_len  bytes of src to examine
 * @returns 1 when a four-byte sequence is present, else 0
 */
static int contains_supplementary_utf8(const char *src, size_t src_len)
{
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c >= 0xf0 && c <= 0xf4 && i + 3U < src_len
                && (((unsigned char)src[i + 1U] & 0xc0U) == 0x80U)
                && (((unsigned char)src[i + 2U] & 0xc0U) == 0x80U)
                && (((unsigned char)src[i + 3U] & 0xc0U) == 0x80U)) {
            return 1;
        }
    }

    return 0;
}

/*!
 * @brief Tell whether a string is ASCII, the same in every name encoding
 *
 * @param[in]  src      the string
 * @param[in]  src_len  bytes of src to examine
 * @returns 1 when every byte is below 0x80, else 0
 */
static int utf8_is_ascii(const char *src, size_t src_len)
{
    for (size_t i = 0; i < src_len; i++) {
        if ((unsigned char)src[i] >= 0x80U) {
            return 0;
        }
    }

    return 1;
}

/*!
 * @brief Copy a string that converts to itself
 *
 * @param[in]  src       the string
 * @param[in]  src_len   bytes of src to copy
 * @param[out] dest      receives the string, or "" when it does not fit
 * @param[in]  dest_len  capacity of dest
 * @returns 0, or -1 when dest cannot hold src and its terminator
 */
static int copy_identity(const char *src, size_t src_len, char *dest,
                         size_t dest_len)
{
    if (src_len >= dest_len) {
        if (dest_len) {
            dest[0] = '\0';
        }

        return -1;
    }

    memcpy(dest, src, src_len);
    dest[src_len] = '\0';
    return 0;
}

/*!
 * @brief Copy a path with a supplementary-plane character verbatim
 *
 * @param[in]  src       UTF-8 path
 * @param[in]  src_len   bytes of src to examine
 * @param[out] dest      receives the copy
 * @param[in]  dest_len  capacity of dest
 * @returns 1 when copied, 0 when src has no such character, -1 when dest
 *          cannot hold it
 */
static int copy_if_supplementary_utf8(const char *src, size_t src_len,
                                      char *dest, size_t dest_len)
{
    if (!contains_supplementary_utf8(src, src_len)) {
        return 0;
    }

    if (copy_identity(src, src_len, dest, dest_len) < 0) {
        return -1;
    }

    return 1;
}

/* A separator and a 255-character name, the longest the protocol allows,
 * decomposed fourfold; a longer path takes the heap. */
#define UCS2_STACK_CHARS ((AFP_MAX_UTF8_NAME_CHARS + 1U) * 4U + 1U)

struct ucs2_buffer {
    char16 stack[UCS2_STACK_CHARS];
    char16 *heap;
};

/*!
 * @brief Take a UCS-2 buffer, on the stack when it fits
 *
 * @param[out] buffer  the holder, released with ucs2_buffer_put()
 * @param[in]  chars   characters to hold
 * @returns the buffer, or NULL when the heap refuses it
 */
static char16 *ucs2_buffer_get(struct ucs2_buffer *buffer, size_t chars)
{
    buffer->heap = NULL;

    if (chars <= UCS2_STACK_CHARS) {
        return buffer->stack;
    }

    buffer->heap = malloc(chars * sizeof(char16));
    return buffer->heap;
}

/*!
 * @brief Release a buffer ucs2_buffer_get() took
 *
 * @param[in] buffer  the holder
 */
static void ucs2_buffer_put(struct ucs2_buffer *buffer)
{
    free(buffer->heap);
}

/*!
 * @brief Tell whether a UTF-8 string decodes whole and unchanged
 *
 * @param[in]  src      UTF-8 string
 * @param[in]  src_len  bytes of src to decode
 * @returns 1 when every byte decodes with no replacement, else 0
 */
static int utf8_decodes_whole(const char *src, int src_len)
{
    struct ucs2_buffer decoded;
    char16 *chars;
    size_t bytes_consumed;
    unsigned int substituted;
    int count;
    chars = ucs2_buffer_get(&decoded, (size_t)src_len + 1U);

    if (!chars) {
        return 0;
    }

    count = UTF8toUCS2(src, (size_t)src_len, chars, (size_t)src_len + 1U,
                       &bytes_consumed, &substituted);
    ucs2_buffer_put(&decoded);

    if (count < 0 || substituted != 0 || bytes_consumed != (size_t)src_len) {
        return 0;
    }

    return 1;
}

/*!
 * @brief Convert a Mac Roman string to UTF-8
 *
 * @param[in]  src       Mac Roman string
 * @param[in]  src_len   bytes of src to convert, 255 at most
 * @param[out] dest      receives the UTF-8 string
 * @param[in]  dest_len  capacity of dest
 * @returns 0, or -1 for an empty argument or a result dest cannot hold
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

/*!
 * @brief Map a UCS-2 character to its Mac Roman byte
 *
 * @param[in] uc  the character
 * @returns the byte, or 0 when Mac Roman has no such character
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

/*!
 * @brief Convert a UTF-8 string to Mac Roman
 *
 * A result is never longer than its source, so a dest_len above src_len
 * holds it.
 *
 * @param[in]  src       UTF-8 string
 * @param[in]  src_len   bytes of src to convert
 * @param[out] dest      receives the Mac Roman string, "" on refusal
 * @param[in]  dest_len  capacity of dest, above src_len
 * @returns 0, or -1 for a string that does not decode whole or has a
 *          character Mac Roman lacks
 */
static int convert_utf8_to_mac_roman(const char *src, int src_len,
                                     char *dest, int dest_len)
{
    struct ucs2_buffer ucs2;
    char16 *chars;
    size_t bytes_consumed;
    unsigned int substituted;
    int count;
    int out = 0;

    if (!src || !dest || src_len <= 0 || dest_len <= 0) {
        return -1;
    }

    chars = ucs2_buffer_get(&ucs2, (size_t)src_len + 1U);

    if (!chars) {
        return -1;
    }

    count = UTF8toUCS2(src, (size_t)src_len, chars, (size_t)src_len + 1U,
                       &bytes_consumed, &substituted);

    /* A path that does not decode whole names something else. */
    if (count < 0 || substituted != 0
            || bytes_consumed != (size_t)src_len) {
        ucs2_buffer_put(&ucs2);
        dest[0] = '\0';
        return -1;
    }

    for (int i = 0; i < count; i++) {
        char16 c = chars[i];

        if (c < 0x80) {
            dest[out++] = (char)c;
        } else {
            unsigned char mr = unicode_to_mac_roman(c);

            if (!mr) {
                ucs2_buffer_put(&ucs2);
                dest[0] = '\0';
                return -1;
            }

            dest[out++] = (char)mr;
        }
    }

    dest[out] = '\0';
    ucs2_buffer_put(&ucs2);
    return 0;
}

/*!
 * @brief Convert a path from the server into the UTF-8 the client shows
 *
 * A name from the server is shown as it decodes, a replaced sequence
 * included.
 *
 * @param[in]  encoding  kFPUTF8Name or kFPLongName
 * @param[out] dest      receives the UTF-8 path, AFP slashes as colons
 * @param[in]  src       the server's path, read up to dest_len bytes
 * @param[in]  dest_len  capacity of dest
 * @returns 0, or -1 for an unknown encoding or a UTF-8 path that does not
 *          convert or fit
 */
int convert_path_to_unix(char encoding, char *dest,
                         char *src, int dest_len)
{
    char *p;
    int src_len;

    if (!src || !dest || dest_len <= 0) {
        return -1;
    }

    dest[0] = '\0';
    src_len = (int)strnlen(src, (size_t)dest_len);

    switch (encoding) {
    case kFPUTF8Name:
        if (utf8_is_ascii(src, (size_t)src_len)) {
            if (copy_identity(src, (size_t)src_len, dest,
                              (size_t)dest_len) < 0) {
                return -1;
            }

            break;
        }

        if (copy_if_supplementary_utf8(src, src_len, dest,
                                       (size_t)dest_len) < 0) {
            return -1;
        }

        if (!dest[0] && src_len > 0
                && convert_utf8dec_to_utf8pre(src, src_len, dest,
                                              dest_len) < 0) {
            return -1;
        }

        break;

    case kFPLongName:
        convert_mac_roman_to_utf8(src, strnlen(src, dest_len), dest, dest_len);
        break;

    default:
        return -1;
    }

    /* The reverse of unixpath_to_afppath(): an AFP '/' is a Unix ':'. */
    p = dest;

    while (*p && (p < dest + dest_len - 1)) {
        if (*p == '/') {
            *p = ':';
        }

        p++;
    }

    return 0;
}

/*!
 * @brief Convert a UTF-8 path to the volume's AFP name encoding
 *
 * A path must decode whole and unchanged; on a UTF-8 volume one with a
 * supplementary character passes verbatim instead of decomposed.
 *
 * @param[in]  encoding  kFPUTF8Name or kFPLongName
 * @param[out] dest      receives the AFP path, "" on refusal
 * @param[in]  src       NUL-terminated UTF-8 path
 * @param[in]  dest_len  capacity of dest
 * @returns 0, or -1 when the path does not convert or fit, or a Mac Roman
 *          result exceeds 255 bytes
 */
int convert_path_to_afp(char encoding, char *dest,
                        const char *src, int dest_len)
{
    int src_len;
    int ascii;
    int copied;

    if (!src || !dest || dest_len <= 0) {
        return -1;
    }

    dest[0] = '\0';
    src_len = (int)strnlen(src, (size_t)dest_len);

    /* A source the bound cut converts to part of a name. */
    if (src_len >= dest_len) {
        return -1;
    }

    ascii = utf8_is_ascii(src, (size_t)src_len);

    switch (encoding) {
    case kFPUTF8Name:
        if (ascii) {
            return copy_identity(src, (size_t)src_len, dest,
                                 (size_t)dest_len);
        }

        copied = copy_if_supplementary_utf8(src, src_len, dest,
                                            (size_t)dest_len);

        /* A verbatim copy must still decode whole elsewhere. */
        if (copied < 0
                || (copied > 0 && !utf8_decodes_whole(src, src_len))) {
            dest[0] = '\0';
            return -1;
        }

        if (!copied && src_len > 0
                && convert_utf8pre_to_utf8dec(src, src_len, dest,
                                              dest_len) < 0) {
            return -1;
        }

        break;

    case kFPLongName:
        if (ascii) {
            if (copy_identity(src, (size_t)src_len, dest,
                              (size_t)dest_len) < 0) {
                return -1;
            }
        } else if (contains_supplementary_utf8(src, src_len)
                   || convert_utf8_to_mac_roman(src, src_len, dest,
                                                dest_len) < 0) {
            return -1;
        }

        /* kFPLongName carries an 8-bit length of the converted name. */
        if (strnlen(dest, (size_t)dest_len) > UINT8_MAX) {
            dest[0] = '\0';
            return -1;
        }

        break;

    default:
        return -1;
    }

    return 0;
}

/*!
 * @brief Precompose a decomposed UTF-8 string, the AFP form, into the
 *        form the client shows
 *
 * The result holds what decoded; a caller that must not change a name
 * reads the return value.
 *
 * @param[in]  src       decomposed UTF-8 string
 * @param[in]  src_len   bytes of src to convert
 * @param[out] dest      receives the precomposed string, "" on failure
 * @param[in]  dest_len  capacity of dest
 * @returns 0 for a string that decoded whole, 1 for one with a replaced
 *          or cut sequence, or -1 when dest cannot hold the result
 */
int convert_utf8dec_to_utf8pre(const char *src, int src_len,
                               char *dest, int dest_len)
{
    struct ucs2_buffer decomposed;
    struct ucs2_buffer precomposed;
    char16 *path16dec, *path16pre, c, prev, *p16dec, *p16pre;
    int comp;
    int count;
    int ret;
    size_t ucs2_len;
    size_t bytes_consumed;
    unsigned int substituted;

    if (!src || !dest || src_len <= 0 || dest_len <= 0) {
        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    path16dec = ucs2_buffer_get(&decomposed, (size_t)src_len + 1U);

    if (!path16dec) {
        *dest = '\0';
        return -1;
    }

    count = UTF8toUCS2(src, (size_t)src_len, path16dec,
                       (size_t)src_len + 1U, &bytes_consumed, &substituted);

    if (count < 0) {
        ucs2_buffer_put(&decomposed);
        *dest = '\0';
        return -1;
    }

    /* Precomposition never grows a string. */
    path16pre = ucs2_buffer_get(&precomposed, (size_t)count + 1U);

    if (!path16pre) {
        ucs2_buffer_put(&decomposed);
        *dest = '\0';
        return -1;
    }

    p16dec = path16dec;
    p16pre = path16pre;
    prev = 0;
    ucs2_len = 0;

    while (*p16dec != 0) {
        c = *p16dec;

        if (prev > 0) {
            comp = UCS2precompose(prev, c);

            if (comp != -1) {
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

        if (*p16dec == 0) {
            *p16pre = prev;
            p16pre++;
            ucs2_len++;
        }
    }

    *p16pre = 0;
    ret = UCS2toUTF8(path16pre, ucs2_len, dest, dest_len);
    ucs2_buffer_put(&precomposed);
    ucs2_buffer_put(&decomposed);

    if (ret != 0) {
        *dest = '\0';
        return -1;
    }

    /* A replaced or cut sequence: the name shown is not the name given. */
    if (substituted != 0 || bytes_consumed != (size_t)src_len) {
        return 1;
    }

    return 0;
}

/*!
 * @brief Append the full decomposition of a character
 *
 * @param[in]     c     the character
 * @param[in,out] dest  write position, advanced past what was written
 */
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

/*!
 * @brief Decompose a precomposed UTF-8 string, the client's form, into
 *        the AFP form
 *
 * @param[in]  src       precomposed UTF-8 string
 * @param[in]  src_len   bytes of src to convert
 * @param[out] dest      receives the decomposed string, "" on refusal
 * @param[in]  dest_len  capacity of dest
 * @returns 0, or -1 for a string that does not decode whole or a result
 *          dest cannot hold
 */
int convert_utf8pre_to_utf8dec(const char *src, int src_len,
                               char *dest, int dest_len)
{
    struct ucs2_buffer precomposed;
    struct ucs2_buffer decomposed;
    char16 *path16pre;
    char16 *path16dec, *p16dec;
    const char16 *p16pre;
    size_t bytes_consumed;
    unsigned int substituted;
    int count;
    int ret;

    if (!src || !dest || src_len <= 0 || dest_len <= 0) {
        if (dest && dest_len > 0) {
            *dest = '\0';
        }

        return -1;
    }

    path16pre = ucs2_buffer_get(&precomposed, (size_t)src_len + 1U);

    if (!path16pre) {
        *dest = '\0';
        return -1;
    }

    count = UTF8toUCS2(src, (size_t)src_len, path16pre,
                       (size_t)src_len + 1U, &bytes_consumed, &substituted);

    /* A path that does not decode whole names something else. */
    if (count < 0 || substituted != 0
            || bytes_consumed != (size_t)src_len) {
        ucs2_buffer_put(&precomposed);
        *dest = '\0';
        return -1;
    }

    /* A decomposition grows a character to four at most. */
    path16dec = ucs2_buffer_get(&decomposed, (size_t)count * 4U + 1U);

    if (!path16dec) {
        ucs2_buffer_put(&precomposed);
        *dest = '\0';
        return -1;
    }

    p16pre = path16pre;
    p16dec = path16dec;

    while (*p16pre != 0) {
        decompose_char(*p16pre, &p16dec);
        p16pre++;
    }

    *p16dec = 0;
    ret = UCS2toUTF8(path16dec, (size_t)(p16dec - path16dec), dest,
                     dest_len);
    ucs2_buffer_put(&decomposed);
    ucs2_buffer_put(&precomposed);

    if (ret != 0) {
        *dest = '\0';
        return -1;
    }

    return 0;
}
