#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lib/proto_login.h"
#include "lib/utils.h"
#include "tap.h"

static void check_payload(const char *username,
                          const char *directory_domain,
                          const uint8_t *authinfo,
                          size_t authinfo_len,
                          const uint8_t *expected,
                          size_t expected_len)
{
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    CHECK(afp_loginext_build_payload("AFP3.4", "SRP", username,
                                     directory_domain, authinfo, authinfo_len,
                                     &payload, &payload_len) == 0);
    CHECK(payload != NULL);
    CHECK(payload_len == expected_len);
    CHECK(memcmp(payload, expected, expected_len) == 0);
    afp_loginext_payload_free(payload, payload_len);
}

static void check_rejected(const char *afp_version,
                           const char *uam_name,
                           const char *username,
                           const char *directory_domain,
                           const void *authinfo,
                           size_t authinfo_len,
                           int expected_error)
{
    uint8_t marker = 0;
    uint8_t *payload = &marker;
    size_t payload_len = 99;
    CHECK(afp_loginext_build_payload(afp_version, uam_name, username,
                                     directory_domain, authinfo, authinfo_len,
                                     &payload, &payload_len) == expected_error);
    CHECK(payload == NULL);
    CHECK(payload_len == 0);
}

static void check_initial(int version_number, const char *version,
                          const char *uam, const char *username, int pad,
                          const uint8_t *authinfo, size_t authinfo_len,
                          const uint8_t *expected, size_t expected_len)
{
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    CHECK(afp_login_initial_build_payload(version_number, version, uam,
                                          username, pad, authinfo,
                                          authinfo_len, &payload,
                                          &payload_len) == 0);
    CHECK(payload != NULL);
    CHECK(payload_len == expected_len);

    for (size_t i = 0; i < expected_len; i++) {
        if (payload[i] != expected[i]) {
            fprintf(stderr, "selected payload mismatch at %zu: got %02x, expected %02x\n",
                    i, payload[i], expected[i]);
            break;
        }
    }

    CHECK(memcmp(payload, expected, expected_len) == 0);
    afp_loginext_payload_free(payload, payload_len);
}

int main(int argc, char **argv)
{
    static const uint8_t ascii_authinfo[] = { 0xaa, 0xbb };
    static const uint8_t ascii_expected[] = {
        0x3f, 0x00, 0x00, 0x00,
        0x06, 'A', 'F', 'P', '3', '.', '4',
        0x03, 'S', 'R', 'P',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 'b', 'o', 'b',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00,
        0xaa, 0xbb,
    };
    static const uint8_t unicode_authinfo[] = { 0xcc };
    static const uint8_t unicode_expected[] = {
        0x3f, 0x00, 0x00, 0x00,
        0x06, 'A', 'F', 'P', '3', '.', '4',
        0x03, 'S', 'R', 'P',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x02, 0xc3, 0xa9,
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00,
        0x00,
        0xcc,
    };
    static const uint8_t domain_expected[] = {
        0x3f, 0x00, 0x00, 0x00,
        0x06, 'A', 'F', 'P', '3', '.', '4',
        0x03, 'S', 'R', 'P',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x01, 'a',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 0xe5, 0x9f, 0x9f,
        0x00,
    };
    static const uint8_t password[] = {
        'p', 'w', 0, 0, 0, 0, 0, 0,
    };
    static const uint8_t ma[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t srp_marker[] = { 0x00, 0x01 };
    static const uint8_t guest_legacy[] = {
        0x12, 0x06, 'A', 'F', 'P', '2', '.', '2',
        0x0f, 'N', 'o', ' ', 'U', 's', 'e', 'r', ' ',
        'A', 'u', 't', 'h', 'e', 'n', 't',
    };
    static const uint8_t guest_ext[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x0f, 'N', 'o', ' ', 'U', 's', 'e', 'r', ' ',
        'A', 'u', 't', 'h', 'e', 'n', 't',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00,
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    };
    static const uint8_t clear_legacy[] = {
        0x12, 0x06, 'A', 'F', 'P', '2', '.', '2',
        0x10, 'C', 'l', 'e', 'a', 'r', 't', 'x', 't', ' ',
        'P', 'a', 's', 's', 'w', 'r', 'd',
        0x03, 'b', 'o', 'b', 0x00,
        'p', 'w', 0, 0, 0, 0, 0, 0,
    };
    static const uint8_t clear_legacy_macroman[] = {
        0x12, 0x06, 'A', 'F', 'P', '2', '.', '2',
        0x10, 'C', 'l', 'e', 'a', 'r', 't', 'x', 't', ' ',
        'P', 'a', 's', 's', 'w', 'r', 'd',
        0x01, 0x8e, 0x00,
        'p', 'w', 0, 0, 0, 0, 0, 0,
    };
    static const uint8_t clear_ext[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x10, 'C', 'l', 'e', 'a', 'r', 't', 'x', 't', ' ',
        'P', 'a', 's', 's', 'w', 'r', 'd',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 'b', 'o', 'b',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        'p', 'w', 0, 0, 0, 0, 0, 0,
    };
    static const uint8_t clear_ext_unicode[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x10, 'C', 'l', 'e', 'a', 'r', 't', 'x', 't', ' ',
        'P', 'a', 's', 's', 'w', 'r', 'd',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x02, 0xc3, 0xa9,
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00,
        'p', 'w', 0, 0, 0, 0, 0, 0,
    };
    static const uint8_t rand_legacy[] = {
        0x12, 0x06, 'A', 'F', 'P', '2', '.', '2',
        0x10, 'R', 'a', 'n', 'd', 'n', 'u', 'm', ' ',
        'E', 'x', 'c', 'h', 'a', 'n', 'g', 'e',
        0x03, 'b', 'o', 'b',
    };
    static const uint8_t rand_ext[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x10, 'R', 'a', 'n', 'd', 'n', 'u', 'm', ' ',
        'E', 'x', 'c', 'h', 'a', 'n', 'g', 'e',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 'b', 'o', 'b',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    };
    static const uint8_t rand2_legacy[] = {
        0x12, 0x06, 'A', 'F', 'P', '2', '.', '2',
        0x16, '2', '-', 'W', 'a', 'y', ' ', 'R', 'a', 'n', 'd', 'n', 'u',
        'm', ' ', 'E', 'x', 'c', 'h', 'a', 'n', 'g', 'e',
        0x03, 'b', 'o', 'b',
    };
    static const uint8_t rand2_ext[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x16, '2', '-', 'W', 'a', 'y', ' ', 'R', 'a', 'n', 'd', 'n', 'u',
        'm', ' ', 'E', 'x', 'c', 'h', 'a', 'n', 'g', 'e',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 'b', 'o', 'b',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    };
    static const uint8_t dhx_legacy[] = {
        0x12, 0x06, 'A', 'F', 'P', '2', '.', '2',
        0x09, 'D', 'H', 'C', 'A', 'S', 'T', '1', '2', '8',
        0x03, 'b', 'o', 'b',
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t dhx_ext[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x09, 'D', 'H', 'C', 'A', 'S', 'T', '1', '2', '8',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 'b', 'o', 'b',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t dhx2_legacy[] = {
        0x12, 0x06, 'A', 'F', 'P', '2', '.', '2',
        0x04, 'D', 'H', 'X', '2', 0x03, 'b', 'o', 'b', 0x00,
    };
    static const uint8_t dhx2_ext[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x04, 'D', 'H', 'X', '2',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 'b', 'o', 'b',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    };
    static const uint8_t srp_ext[] = {
        0x3f, 0, 0, 0, 0x06, 'A', 'F', 'P', '3', '.', '4',
        0x03, 'S', 'R', 'P',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x03, 'b', 'o', 'b',
        0x03, 0x08, 0x00, 0x01, 0x03, 0x00, 0x00,
        0x00, 0x01,
    };
    char too_many_characters[257];
    char too_long_pascal[257];
    char max_username[(255U * 4U) + 1U];
    char legacy_max[AFP_LEGACY_MAX_USERNAME_BYTES + 1U];
    char legacy_too_long[AFP_LEGACY_MAX_USERNAME_BYTES + 2U];
    char max_username_url[sizeof("afp://@server")
                          + AFPC_MAX_USERNAME_BYTES];
    struct afpc_url parsed_url;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint8_t auth_marker = 0;
    test_tap_init(argc, argv);
    check_initial(22, "AFP2.2", "No User Authent", NULL, 0, NULL, 0,
                  guest_legacy, sizeof(guest_legacy));
    check_initial(34, "AFP3.4", "No User Authent", NULL, 0, NULL, 0,
                  guest_ext, sizeof(guest_ext));
    check_initial(22, "AFP2.2", "Cleartxt Passwrd", "bob", 1,
                  password, sizeof(password), clear_legacy,
                  sizeof(clear_legacy));
    check_initial(22, "AFP2.2", "Cleartxt Passwrd", "\xc3\xa9", 1,
                  password, sizeof(password), clear_legacy_macroman,
                  sizeof(clear_legacy_macroman));
    check_initial(34, "AFP3.4", "Cleartxt Passwrd", "bob", 1,
                  password, sizeof(password), clear_ext, sizeof(clear_ext));
    check_initial(34, "AFP3.4", "Cleartxt Passwrd", "\xc3\xa9", 1,
                  password, sizeof(password), clear_ext_unicode,
                  sizeof(clear_ext_unicode));
    check_initial(22, "AFP2.2", "Randnum Exchange", "bob", 0,
                  NULL, 0, rand_legacy, sizeof(rand_legacy));
    check_initial(34, "AFP3.4", "Randnum Exchange", "bob", 0,
                  NULL, 0, rand_ext, sizeof(rand_ext));
    check_initial(22, "AFP2.2", "2-Way Randnum Exchange", "bob", 0,
                  NULL, 0, rand2_legacy, sizeof(rand2_legacy));
    check_initial(34, "AFP3.4", "2-Way Randnum Exchange", "bob", 0,
                  NULL, 0, rand2_ext, sizeof(rand2_ext));
    check_initial(22, "AFP2.2", "DHCAST128", "bob", 1,
                  ma, sizeof(ma), dhx_legacy, sizeof(dhx_legacy));
    check_initial(34, "AFP3.4", "DHCAST128", "bob", 1,
                  ma, sizeof(ma), dhx_ext, sizeof(dhx_ext));
    check_initial(22, "AFP2.2", "DHX2", "bob", 1,
                  NULL, 0, dhx2_legacy, sizeof(dhx2_legacy));
    check_initial(34, "AFP3.4", "DHX2", "bob", 1,
                  NULL, 0, dhx2_ext, sizeof(dhx2_ext));
    check_initial(34, "AFP3.4", "SRP", "bob", 0,
                  srp_marker, sizeof(srp_marker), srp_ext, sizeof(srp_ext));
    /* Empty type-3 domain ends even: UserAuthInfo follows immediately. */
    check_payload("bob", "", ascii_authinfo, sizeof(ascii_authinfo),
                  ascii_expected, sizeof(ascii_expected));
    /* The two-byte UTF-8 name makes the domain end odd: one pad is present. */
    check_payload("\xc3\xa9", "", unicode_authinfo,
                  sizeof(unicode_authinfo), unicode_expected,
                  sizeof(unicode_expected));
    check_payload("a", "\xe5\x9f\x9f", NULL, 0,
                  domain_expected, sizeof(domain_expected));

    for (size_t i = 0; i < 255U; i++) {
        max_username[(i * 4U) + 0U] = (char)0xf0;
        max_username[(i * 4U) + 1U] = (char)0xa0;
        max_username[(i * 4U) + 2U] = (char)0x80;
        max_username[(i * 4U) + 3U] = (char)0x80;
    }

    max_username[255U * 4U] = '\0';
    CHECK(sizeof(((struct afpc_url *)0)->username)
          == AFPC_MAX_USERNAME_LEN);
    CHECK(afp_validate_username(max_username, NULL) == 0);
    CHECK(snprintf(max_username_url, sizeof(max_username_url),
                   "afp://%s@server", max_username)
          < (int)sizeof(max_username_url));
    afp_default_url(&parsed_url);
    CHECK(afp_parse_url_quiet(&parsed_url, max_username_url) == 0);
    CHECK(memcmp(parsed_url.username, max_username,
                 sizeof(max_username)) == 0);
    CHECK(afp_loginext_build_payload("AFP3.4", "SRP", max_username, "",
                                     NULL, 0, &payload, &payload_len) == 0);
    CHECK(payload_len == 1050U);
    CHECK(payload[20] == 0x03 && payload[21] == 0xfc);
    afp_loginext_payload_free(payload, payload_len);
    payload = NULL;
    payload_len = 0;
    CHECK(afp_loginext_build_payload("AFP3.4", "SRP",
                                     parsed_url.username, "", NULL, 0,
                                     &payload, &payload_len) == 0);
    CHECK(payload_len == 1050U);
    afp_loginext_payload_free(payload, payload_len);
    afpc_path_clear(&parsed_url.path);
    memset(legacy_max, 'l', sizeof(legacy_max) - 1U);
    legacy_max[sizeof(legacy_max) - 1U] = '\0';
    memset(legacy_too_long, 'l', sizeof(legacy_too_long) - 1U);
    legacy_too_long[sizeof(legacy_too_long) - 1U] = '\0';
    CHECK(afp_validate_legacy_username(legacy_max, NULL) == 0);
    CHECK(afp_validate_legacy_username(legacy_too_long, NULL)
          == -ENAMETOOLONG);
    memset(too_many_characters, 'x', sizeof(too_many_characters) - 1U);
    too_many_characters[sizeof(too_many_characters) - 1U] = '\0';
    memset(too_long_pascal, 'y', sizeof(too_long_pascal) - 1U);
    too_long_pascal[sizeof(too_long_pascal) - 1U] = '\0';
    check_rejected("AFP3.4", "SRP", "\xc0\xaf", "", NULL, 0, -EILSEQ);
    check_rejected("AFP3.4", "SRP", "\xed\xa0\x80", "", NULL, 0,
                   -EILSEQ);
    check_rejected("AFP3.4", "SRP", "\xf4\x90\x80\x80", "", NULL, 0,
                   -EILSEQ);
    check_rejected("AFP3.4", "SRP", "\xe2\x82", "", NULL, 0, -EILSEQ);
    check_rejected("AFP3.4", "SRP", too_many_characters, "", NULL, 0,
                   -ENAMETOOLONG);
    check_rejected("AFP3.4", "SRP", "user", too_many_characters, NULL, 0,
                   -ENAMETOOLONG);
    check_rejected(too_long_pascal, "SRP", "user", "", NULL, 0,
                   -EOVERFLOW);
    check_rejected("AFP3.4", too_long_pascal, "user", "", NULL, 0,
                   -EOVERFLOW);
    check_rejected("AFP3.4", "SRP", "user", "", NULL, 1, -EINVAL);
    check_rejected("AFP3.4", "SRP", "user", "", &auth_marker,
                   (size_t)INT_MAX, -EOVERFLOW);
    return test_tap_finish();
}
