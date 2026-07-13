#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/daemon_socket.h"
#include "tap.h"

static int create_stale_socket(const char *path)
{
    struct sockaddr_un address;
    size_t path_len = strlen(path);
    socklen_t address_len;
    int fd;

    if (path_len >= sizeof(address.sun_path)) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_len + 1U);
    address_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                              + path_len + 1U);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&address, address_len) != 0) {
        close(fd);
        return -1;
    }

    return close(fd);
}

int main(int argc, char **argv)
{
    char temporary[] = "/tmp/netatalk-daemon-socket-XXXXXX";
    char path[sizeof(temporary) + sizeof("/socket")];
    struct stat path_stat;
    int fd;
    int second_fd;

    test_tap_init(argc, argv);
    CHECK(mkdtemp(temporary) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/socket", temporary)
          < (int)sizeof(path));

    CHECK(daemon_socket_cleanup_stale(path) == 0);
    fd = daemon_socket_create(path, 2);
    CHECK(fd >= 0);
    CHECK(lstat(path, &path_stat) == 0 && S_ISSOCK(path_stat.st_mode));

    CHECK(daemon_socket_cleanup_stale(path) == -1);
    second_fd = daemon_socket_create(path, 2);
    CHECK(second_fd == -1);
    CHECK(lstat(path, &path_stat) == 0 && S_ISSOCK(path_stat.st_mode));

    daemon_socket_close(fd, path);
    CHECK(lstat(path, &path_stat) != 0 && errno == ENOENT);

    CHECK(create_stale_socket(path) == 0);
    CHECK(lstat(path, &path_stat) == 0 && S_ISSOCK(path_stat.st_mode));
    CHECK(daemon_socket_cleanup_stale(path) == 0);
    CHECK(lstat(path, &path_stat) != 0 && errno == ENOENT);

    fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    CHECK(daemon_socket_cleanup_stale(path) == -1);
    CHECK(lstat(path, &path_stat) == 0 && S_ISREG(path_stat.st_mode));
    CHECK(unlink(path) == 0);
    CHECK(rmdir(temporary) == 0);
    return test_tap_finish();
}
