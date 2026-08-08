#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "lib/afp_internal.h"
#include "lib/codepage.h"
#include "lib/utils.h"
#include "netatalk-client/path.h"
#include "tap.h"

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

int main(int argc, char **argv)
{
    struct afp_versions afp3 = { .av_name = "AFP3.0", .av_number = 30 };
    struct afp_server server;
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
    server.using_version = &afp3;
    server.path_encoding = kFPUTF8Name;
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
        CHECK(!invalid_filename(&server, name_255));
        CHECK(invalid_filename(&server, name_256));
        CHECK(!invalid_filename(&server, supplementary_name_255));
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
    }

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
