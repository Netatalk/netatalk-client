#include "afp.h"
#include "lib/afp_replies.h"
#include "lib/dsi_protocol.h"
#include "tap.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct listxattr_reply {
    struct dsi_header header __attribute__((__packed__));
    uint16_t reserved;
    uint32_t datalength;
    char data[];
} __attribute__((__packed__));

static uint64_t host_to_network64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)value) << 32)
           | htonl((uint32_t)(value >> 32));
#else
    return value;
#endif
}

int main(int argc, char **argv)
{
    unsigned char reply[sizeof(struct dsi_header) + 16];
    struct dsi_header *header = (struct dsi_header *)reply;
    uint32_t wire32;
    uint64_t wire64;
    uint32_t written32 = 99;
    uint64_t written64 = 99;
    struct afp_extattr_info xattr_info;
    struct {
        struct dsi_header header __attribute__((__packed__));
        uint16_t bitmap;
        uint32_t datalength;
        char data[4];
    } __attribute__((__packed__)) xattr_reply;
    test_tap_init(argc, argv);
    memset(reply, 0, sizeof(reply));
    wire32 = htonl(1234);
    memcpy(reply + sizeof(reply) - sizeof(wire32), &wire32, sizeof(wire32));
    CHECK(afp_write_reply(NULL, (char *)reply, sizeof(reply), &written32) == 0);
    CHECK(written32 == 1234);
    wire64 = host_to_network64(UINT64_C(0x0102030405060708));
    memcpy(reply + sizeof(reply) - sizeof(wire64), &wire64, sizeof(wire64));
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
    return test_tap_finish();
}
