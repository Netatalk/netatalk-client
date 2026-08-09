#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/afp_internal.h"
#include "lib/afp_replies.h"
#include "lib/dsi_protocol.h"
#include "tap.h"

struct listxattr_reply {
    struct dsi_header header __attribute__((__packed__));
    uint16_t reserved;
    uint32_t datalength;
    char data[];
} __attribute__((__packed__));

int main(int argc, char **argv)
{
    unsigned char reply[sizeof(struct dsi_header) + 16];
    struct dsi_header *header = (struct dsi_header *)reply;
    uint32_t wire32;
    static const unsigned char wire64[] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };
    uint32_t written32 = 99;
    uint64_t written64 = 99;
    struct afp_extattr_info xattr_info;
    struct afp_server server;
    struct afp_icon icon;
    struct afp_comment comment;
    struct afp_icon_info icon_info;
    struct afp_appl appl;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
        uint32_t datalength;
        char data[4];
    } __attribute__((__packed__)) xattr_reply;
    test_tap_init(argc, argv);
    memset(&server, 0, sizeof(server));
    CHECK(!afp_server_has_valid_signature(&server));
    server.flags = kSrvrSig;
    CHECK(!afp_server_has_valid_signature(&server));
    server.signature[0] = 1;
    CHECK(afp_server_has_valid_signature(&server));
    server.flags = 0;
    CHECK(afp_server_has_valid_signature(&server));
    memset(reply, 0, sizeof(reply));
    wire32 = htonl(1234);
    memcpy(reply + sizeof(reply) - sizeof(wire32), &wire32, sizeof(wire32));
    CHECK(afp_write_reply(NULL, (char *)reply, sizeof(reply), &written32) == 0);
    CHECK(written32 == 1234);
    memcpy(reply + sizeof(reply) - sizeof(wire64), wire64, sizeof(wire64));
    CHECK(afp_writeext_reply(NULL, (char *)reply, sizeof(reply), &written64) == 0);
    CHECK(written64 == UINT64_C(0x0102030405060708));
    header->return_code.error_code = 1;
    written32 = 99;
    written64 = 99;
    afp_write_reply(NULL, (char *)reply, sizeof(reply), &written32);
    afp_writeext_reply(NULL, (char *)reply, sizeof(reply), &written64);
    CHECK(written32 == 0);
    CHECK(written64 == 0);
    written32 = 99;
    afp_write_reply(NULL, (char *)reply, sizeof(struct dsi_header) - 1,
                    &written32);
    CHECK(written32 == 0);
    CHECK(afp_write_reply(NULL, (char *)reply, sizeof(reply), NULL) == 0);
    CHECK(afp_writeext_reply(NULL, (char *)reply, sizeof(reply), NULL) == 0);
    memset(&xattr_reply, 0, sizeof(xattr_reply));
    memset(&xattr_info, 0, sizeof(xattr_info));
    xattr_info.maxsize = 2;
    xattr_reply.datalength = htonl(sizeof(xattr_reply.data));
    memcpy(xattr_reply.data, "abcd", sizeof(xattr_reply.data));
    CHECK(afp_getextattr_reply(NULL, (char *)&xattr_reply,
                               sizeof(xattr_reply), &xattr_info) == 0);
    CHECK(xattr_info.size == sizeof(xattr_reply.data));
    CHECK(xattr_info.copied == xattr_info.maxsize);
    CHECK(memcmp(xattr_info.data, "ab", xattr_info.copied) == 0);
    size_t large_size = AFP_EXTATTR_DATA_MAX + 257U;
    struct listxattr_reply *large_reply = calloc(1, sizeof(*large_reply)
                                          + large_size);
    CHECK(large_reply != NULL);
    large_reply->datalength = htonl((uint32_t)large_size);
    memset(large_reply->data, 'x', large_size);
    memset(&xattr_info, 0, sizeof(xattr_info));
    xattr_info.maxsize = (unsigned int)large_size;
    CHECK(afp_listextattrs_reply(NULL, (char *)large_reply,
                                 (unsigned int)(sizeof(*large_reply) + large_size),
                                 &xattr_info) == 0);
    CHECK(xattr_info.size == large_size);
    CHECK(xattr_info.copied == large_size);
    CHECK(xattr_info.data[0] == 'x'
          && xattr_info.data[large_size - 1U] == 'x');
    free(large_reply);
    {
        struct {
            struct dsi_header header;
            char data[2];
        } __attribute__((__packed__)) icon_reply = { 0 };
        char icon_data[4] = { 0 };
        memcpy(icon_reply.data, "ok", 2);
        icon.data = icon_data;
        icon.maxsize = sizeof(icon_data);
        icon.size = 0;
        CHECK(afp_geticon_reply(NULL, (char *)&icon_reply,
                                sizeof(icon_reply), &icon) == 0);
        CHECK(icon.size == 2 && memcmp(icon.data, "ok", 2) == 0);
    }
    {
        struct {
            struct dsi_header header;
            uint8_t length;
            char data[2];
        } __attribute__((__packed__)) comment_reply = { 0 };
        char comment_data[4] = { 0 };
        comment.data = comment_data;
        comment.maxsize = sizeof(comment_data);
        comment_reply.length = 2;
        memcpy(comment_reply.data, "hi", 2);
        CHECK(afp_getcomment_reply(NULL, (char *)&comment_reply,
                                   sizeof(comment_reply), &comment) == 0);
        CHECK(comment.size == 2 && memcmp(comment.data, "hi", 2) == 0);
    }
    {
        struct {
            struct dsi_header header;
            uint32_t tag;
            uint32_t type;
            uint8_t icon_type;
            uint8_t pad;
            uint16_t size;
        } __attribute__((__packed__)) icon_info_reply = { 0 };
        icon_info_reply.tag = htonl(42);
        icon_info_reply.type = htonl(UINT32_C(0x54455854));
        icon_info_reply.icon_type = 7;
        icon_info_reply.size = htons(512);
        CHECK(afp_geticoninfo_reply(NULL, (char *)&icon_info_reply,
                                    sizeof(icon_info_reply), &icon_info) == 0);
        CHECK(icon_info.tag == 42 && icon_info.filetype == UINT32_C(0x54455854)
              && icon_info.icontype == 7 && icon_info.size == 512);
    }
    {
        struct {
            struct dsi_header header;
            uint16_t bitmap;
            uint32_t tag;
            uint32_t did;
            uint16_t name_offset;
            uint8_t name_len;
            char name[3];
        } __attribute__((__packed__)) appl_reply = { 0 };
        appl_reply.bitmap = htons(kFPParentDirIDBit | kFPLongNameBit);
        appl_reply.tag = htonl(99);
        appl_reply.did = htonl(123);
        appl_reply.name_offset = htons(6);
        appl_reply.name_len = 3;
        memcpy(appl_reply.name, "App", 3);
        CHECK(afp_getappl_reply(&server, (char *)&appl_reply,
                                sizeof(appl_reply), &appl) == 0);
        CHECK(appl.tag == 99 && appl.file.did == 123
              && strcmp(appl.file.name, "App") == 0);
    }
    return test_tap_finish();
}
