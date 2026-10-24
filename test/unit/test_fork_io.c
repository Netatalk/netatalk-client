#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/lowlevel.h"
#include "tap.h"

int main(int argc, char **argv)
{
    struct afp_versions afp2 = { "AFP2.2", 22 };
    struct afp_server server;
    struct afp_volume volume;
    struct afp_file_info fork;
    char data[16];
    size_t written;
    test_tap_init(argc, argv);
    memset(&server, 0, sizeof(server));
    memset(&volume, 0, sizeof(volume));
    memset(&fork, 0, sizeof(fork));
    memset(data, 'x', sizeof(data));
    /* A volume of a server that is not connected: every request fails
     * before it is sent, so only a refusal decided earlier shows. */
    server.using_version = &afp2;
    server.quantum = 65536;
    volume.server = &server;
    volume.extra_flags = VOLUME_EXTRA_FLAGS_NO_LOCKING;
    fork.forkid = 1;
    /* An AFP 2 session cannot address a write that ends past 4 GiB. */
    written = 1;
    CHECK(ll_write(&volume, data, sizeof(data),
                   (off_t) AFP_MAX_AFP2_FILESIZE - 8, &fork,
                   &written) == -EFBIG);
    CHECK(written == 0);
    /* A write ending exactly at 4 GiB is refused too: the reply's 32-bit
     * offset would wrap to zero. */
    written = 1;
    CHECK(ll_write(&volume, data, sizeof(data),
                   (off_t) AFP_MAX_AFP2_FILESIZE - (off_t) sizeof(data),
                   &fork, &written) == -EFBIG);
    CHECK(written == 0);
    /* A read whose request fails reads nothing and says so: an empty
     * result would be the end of the file. */
    {
        char buf[16];
        int eof = 0;
        CHECK(ll_read(&volume, buf, sizeof(buf), 0, &fork, &eof) == -EIO);
        CHECK(eof == 0);
    }
    return test_tap_finish();
}
