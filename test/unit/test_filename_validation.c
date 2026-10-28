/*
    test_filename_validation.c: name validation and conversion checks

    Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "lib/afp_internal.h"
#include "lib/codepage.h"
#include "lib/utils.h"
#include "netatalk-client/path.h"
#include "tap.h"

/*!
 * @brief Build "/" and a run of U+4E00, three bytes each
 *
 * @param[in] characters  length of the run
 * @returns the name, which the caller frees, or NULL
 */
static char *utf8_name(size_t characters)
{
    char *name = malloc(1U + (characters * 3U) + 1U);
    char *p;

    if (!name) {
        return NULL;
    }

    p = name;
    *p++ = '/';

    for (size_t i = 0; i < characters; i++) {
        *p++ = (char)0xe4;
        *p++ = (char)0xb8;
        *p++ = (char)0x80;
    }

    *p = '\0';
    return name;
}

/*!
 * @brief Build "/" and a run of U+20000, four bytes each
 *
 * @param[in] characters  length of the run
 * @returns the name, which the caller frees, or NULL
 */
static char *supplementary_utf8_name(size_t characters)
{
    char *name = malloc(1U + (characters * 4U) + 1U);
    char *p;

    if (!name) {
        return NULL;
    }

    p = name;
    *p++ = '/';

    for (size_t i = 0; i < characters; i++) {
        *p++ = (char)0xf0;
        *p++ = (char)0xa0;
        *p++ = (char)0x80;
        *p++ = (char)0x80;
    }

    *p = '\0';
    return name;
}

/*!
 * @brief Swallow a log message
 */
static void discard_log_message(void *priv _U_, enum logtypes logtype _U_,
                                int loglevel _U_, const char *message _U_)
{
    /* The default client prints every message on stdout, the TAP stream. */
}

/*!
 * @brief Run the name validation and conversion checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    struct afp_versions afp3 = { .av_name = "AFP3.0", .av_number = 30 };
    struct afp_server server;
    struct afp_volume utf8_volume;
    struct afp_volume legacy_volume;
    char utf8_path[16] = { 0 };
    char legacy_path[16] = { 0 };
    char *name_255;
    char *name_256;
    char *supplementary_name_255;
    char *converted_supplementary_name;
    char legacy_name[16];
    char legacy_long_source[UINT8_MAX + 2U];
    char legacy_long_name[UINT8_MAX + 2U];
    char *long_url;
    struct afpc_url parsed_url;
    struct afpc_path owned_path = { 0 };
    struct afpc_path copied_path = { 0 };
    const size_t long_path_len = 20000U;
    test_tap_init(argc, argv);
    memset(&server, 0, sizeof(server));
    memset(&utf8_volume, 0, sizeof(utf8_volume));
    memset(&legacy_volume, 0, sizeof(legacy_volume));
    server.using_version = &afp3;
    utf8_volume.server = &server;
    utf8_volume.path_encoding = kFPUTF8Name;
    legacy_volume.server = &server;
    legacy_volume.path_encoding = kFPLongName;
    name_255 = utf8_name(AFP_MAX_UTF8_NAME_CHARS);
    name_256 = utf8_name(AFP_MAX_UTF8_NAME_CHARS + 1U);
    supplementary_name_255 = supplementary_utf8_name(AFP_MAX_UTF8_NAME_CHARS);
    converted_supplementary_name = malloc(AFPC_MAX_NAME_BYTES + 1U);
    long_url = malloc(sizeof("afp://server/volume/") + long_path_len);
    CHECK(name_255 != NULL && name_256 != NULL && supplementary_name_255 != NULL
          && converted_supplementary_name != NULL);
    CHECK(long_url != NULL);
    CHECK(afpc_path_set(&owned_path, "/parent", AFPC_MAX_UTF8_PATH_BYTES) == 0);
    CHECK(afpc_path_join(&copied_path, owned_path.data, "child",
                         AFPC_MAX_UTF8_PATH_BYTES) == 0);
    CHECK(strcmp(copied_path.data, "/parent/child") == 0);
    CHECK(afpc_path_copy(&owned_path, &copied_path,
                         AFPC_MAX_UTF8_PATH_BYTES) == 0);
    CHECK(owned_path.len == copied_path.len);
    CHECK(afpc_path_set(&owned_path, "/too-long", 4) == -ENAMETOOLONG);

    if (name_255 && name_256 && supplementary_name_255
            && converted_supplementary_name) {
        CHECK(!invalid_filename(&utf8_volume, name_255));
        CHECK(invalid_filename(&utf8_volume, name_256));
        CHECK(!invalid_filename(&utf8_volume, supplementary_name_255));
        CHECK(invalid_filename(&legacy_volume, name_255));
        CHECK(convert_path_to_afp(kFPUTF8Name, converted_supplementary_name,
                                  supplementary_name_255,
                                  AFPC_MAX_NAME_BYTES + 1U) == 0);
        CHECK(strcmp(converted_supplementary_name,
                     supplementary_name_255) == 0);
        CHECK(convert_path_to_afp(kFPLongName, legacy_name,
                                  supplementary_name_255,
                                  sizeof(legacy_name)) < 0);
        CHECK(convert_path_to_afp(kFPLongName, legacy_name, "\xe2\x82\xac",
                                  sizeof(legacy_name)) == 0);
        CHECK((unsigned char)legacy_name[0] == 0xdb);
        memset(legacy_long_source, 'a', UINT8_MAX + 1U);
        legacy_long_source[UINT8_MAX + 1U] = '\0';
        CHECK(convert_path_to_afp(kFPLongName, legacy_long_name,
                                  legacy_long_source,
                                  sizeof(legacy_long_name)) < 0);
        /* U+00E9 decomposes to "e" U+0301, three bytes and a terminator:
         * a 3-byte buffer is a failure, a 4-byte buffer the exact fit. */
        CHECK(convert_path_to_afp(kFPUTF8Name, legacy_name, "\xc3\xa9",
                                  3) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, legacy_name, "\xc3\xa9",
                                  4) == 0);
        CHECK(strcmp(legacy_name, "e\xcc\x81") == 0);
    }

    /* U+0390 grows most, two bytes to six: 255 of them need three bytes per
     * source byte, below the four convert_path_to_afp_alloc() allows. */
    {
        char decomposed_source[1U + 255U * 2U + 1U];
        char *decomposed;
        size_t source_len = sizeof(decomposed_source) - 1U;
        decomposed_source[0] = '/';

        for (size_t i = 0; i < 255U; i++) {
            decomposed_source[1U + i * 2U] = (char)0xce;
            decomposed_source[2U + i * 2U] = (char)0x90;
        }

        decomposed_source[source_len] = '\0';
        decomposed = malloc(source_len * 3U + 1U);
        CHECK(decomposed != NULL);
        CHECK(convert_path_to_afp(kFPUTF8Name, decomposed, decomposed_source,
                                  (int)(source_len * 3U + 1U)) == 0);
        CHECK(strlen(decomposed) == 1U + 255U * 6U);
        free(decomposed);
    }
    {
        /* ASCII converts to itself both ways on both encodings; an AFP '/'
         * is a Unix ':'; a path that does not fit is refused, not cut. */
        char out[64];
        char afp_name[] = "a/b";
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/dir/file.txt",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/dir/file.txt") == 0);
        CHECK(convert_path_to_afp(kFPLongName, out, "/dir/file.txt",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/dir/file.txt") == 0);
        CHECK(convert_path_to_unix(kFPUTF8Name, out, afp_name,
                                   sizeof(out)) == 0);
        CHECK(strcmp(out, "a:b") == 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/dir/file.txt",
                                  13) < 0);
        /* U+0100 and U+3000, low byte zero, convert as any other: the first
         * decomposes, the second does not. */
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xc4\x80",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/A\xcc\x84") == 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xe3\x80\x80",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/\xe3\x80\x80") == 0);
    }
    {
        /* A source the bound cuts is refused: "/caf" and U+00E9 in Mac Roman
         * fit a six-byte buffer, the whole name does not. */
        char cut[6];
        CHECK(convert_path_to_afp(kFPLongName, cut, "/caf\xc3\xa9x",
                                  sizeof(cut)) < 0);
    }
    {
        /* A decomposed name of 400 characters precomposes whole. */
        char long_decomposed[1U + 400U * 3U + 1U];
        char precomposed[sizeof(long_decomposed)];
        long_decomposed[0] = '/';

        for (size_t i = 0; i < 400U; i++) {
            long_decomposed[1U + i * 3U] = 'e';
            long_decomposed[2U + i * 3U] = (char)0xcc;
            long_decomposed[3U + i * 3U] = (char)0x81;
        }

        long_decomposed[sizeof(long_decomposed) - 1U] = '\0';
        CHECK(convert_path_to_unix(kFPUTF8Name, precomposed, long_decomposed,
                                   (int)sizeof(precomposed)) == 0);
        CHECK(strlen(precomposed) == 1U + 400U * 2U);
    }
    {
        /* U+1E69 decomposes in two steps, to s U+0323 U+0307; U+00E6 has no
         * decomposition. */
        char out[32];
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xe1\xb9\xa9",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/s\xcc\xa3\xcc\x87") == 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xc3\xa6",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/\xc3\xa6") == 0);
    }
    {
        /* Malformed UTF-8 is refused, the well-formed name converts, and a
         * verbatim supplementary path is refused when its other bytes do not
         * decode. */
        char out[32];
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/caf\xe9",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPLongName, out, "/caf\xe9",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\x80x",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xe2\x82",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xf0\x80\x80\xaf",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xf4\x90\x80\x80",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xed\xa0\x80",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/caf\xc3\xa9",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/cafe\xcc\x81") == 0);
        CHECK(convert_path_to_afp(kFPLongName, out, "/caf\xc3\xa9",
                                  sizeof(out)) == 0);
        CHECK((unsigned char)out[4] == 0x8e);
        CHECK(convert_path_to_afp(kFPUTF8Name, out, "/\xf0\x9f\x93\x81/caf\xe9",
                                  sizeof(out)) < 0);
        CHECK(convert_path_to_afp(kFPUTF8Name, out,
                                  "/\xf0\x9f\x93\x81/caf\xc3\xa9",
                                  sizeof(out)) == 0);
        CHECK(strcmp(out, "/\xf0\x9f\x93\x81/caf\xc3\xa9") == 0);
    }
    {
        /* A URL's volume name is matched precomposed: the Latin-1 "Caf\xe9"
         * names no volume, and a character beyond UCS-2 shows as '~' on both
         * sides and matches. */
        struct libafpclient quiet_client = {
            .unmount_volume = NULL,
            .log_for_client = discard_log_message,
            .forced_ending_hook = NULL,
            .scan_extra_fds = NULL,
            .loop_started = NULL,
        };
        struct afp_volume caf;
        char decomposed_name[] = "Cafe\xcc\x81";
        char plain_name[] = "Caf";
        char precomposed_name[] = "Caf\xc3\xa9";
        char latin1_name[] = "Caf\xe9";
        char supplementary_name[] = "\xf0\x9f\x93\x81" "Docs";
        memset(&caf, 0, sizeof(caf));
        strcpy(caf.volume_name_printable, "Caf\xc3\xa9");
        server.volumes = &caf;
        server.num_volumes = 1;
        libafpclient_register(&quiet_client);
        CHECK(find_volume_by_name(&server, decomposed_name) == &caf);
        CHECK(find_volume_by_name(&server, precomposed_name) == &caf);
        CHECK(find_volume_by_name(&server, plain_name) == NULL);
        strcpy(caf.volume_name_printable, "Caf");
        CHECK(find_volume_by_name(&server, plain_name) == &caf);
        CHECK(find_volume_by_name(&server, latin1_name) == NULL);
        CHECK(convert_utf8dec_to_utf8pre(supplementary_name,
                                         (int)strlen(supplementary_name),
                                         caf.volume_name_printable,
                                         AFP_VOLUME_NAME_UTF8_LEN) == 0);
        CHECK(strcmp(caf.volume_name_printable, "~Docs") == 0);
        CHECK(find_volume_by_name(&server, supplementary_name) == &caf);
        libafpclient_register(NULL);
    }
    CHECK(sizeof_path_header(&utf8_volume) == 7);
    CHECK(sizeof_path_header(&legacy_volume) == 2);
    copy_path(&utf8_volume, utf8_path, "abc", 3);
    copy_path(&legacy_volume, legacy_path, "abc", 3);
    CHECK((unsigned char)utf8_path[0] == kFPUTF8Name);
    CHECK((unsigned char)utf8_path[6] == 3);
    CHECK((unsigned char)legacy_path[0] == kFPLongName);
    CHECK((unsigned char)legacy_path[1] == 3);
    free(name_255);
    free(name_256);
    free(supplementary_name_255);
    free(converted_supplementary_name);

    if (long_url) {
        strcpy(long_url, "afp://server/volume/");
        memset(long_url + strlen(long_url), 'x', long_path_len);
        long_url[strlen("afp://server/volume/") + long_path_len] = '\0';
        afp_default_url(&parsed_url);
        CHECK(afp_parse_url_quiet(&parsed_url, long_url) == 0);
        CHECK(parsed_url.path.len == long_path_len + 1U);
        afpc_path_clear(&parsed_url.path);
    }

    free(long_url);
    afpc_path_clear(&owned_path);
    afpc_path_clear(&copied_path);
    return test_tap_finish();
}
