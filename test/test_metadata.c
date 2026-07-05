#include "metadata.h"
#include "tap.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

static int create_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0) {
        return -1;
    }

    if (write(fd, "data", 4) != 4) {
        close(fd);
        return -1;
    }

    return close(fd);
}

static int overwrite(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0) {
        return -1;
    }

    if (size > 0 && write(fd, data, size) != (ssize_t)size) {
        close(fd);
        return -1;
    }

    return close(fd);
}

static int contains_name(const char *list, size_t size, const char *name)
{
    for (size_t pos = 0; pos < size;) {
        const char *entry = memchr(list + pos, '\0', size - pos);
        size_t length;

        if (entry == NULL) {
            return 0;
        }

        length = (size_t)(entry - (list + pos));

        if (strcmp(list + pos, name) == 0) {
            return 1;
        }

        pos += length + 1U;
    }

    return 0;
}

static int join_suffix(char *dst, size_t dst_size, const char *base,
                       const char *suffix)
{
    if (strlcpy(dst, base, dst_size) >= dst_size) {
        return -ENAMETOOLONG;
    }

    if (strlcat(dst, suffix, dst_size) >= dst_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

int main(int argc, char **argv)
{
    char temporary[] = "afpcmd-metadata-XXXXXX";
    char file[1024], missing[1024], macos_sidecar[1024], netatalk_dir[1024];
    char netatalk_sidecar[1024], ea_header[1024], ea_value[1024];
    unsigned char finder[32], actual_finder[32];
#ifndef __APPLE__
    unsigned char macos_finder[32];
#endif
    unsigned char resource[7000], actual_resource[7000];
    char *list = NULL;
    size_t list_size = 0;
    void *value = NULL;
    size_t value_size = 0;
    struct stat st;
    mode_t old_umask;
    enum afp_metadata_mode parsed_mode;
    volumeid_t dummy_volume = (volumeid_t)(uintptr_t)1;
    unsigned int warnings = UINT_MAX;
    test_tap_init(argc, argv);
    CHECK(AFP_SL_XATTR_CREATE == 0x1);
    CHECK(AFP_SL_XATTR_REPLACE == 0x2);
    CHECK(afp_sl_setxattr(&dummy_volume, "/file", "user.test", "x", 1,
                          AFP_SL_XATTR_CREATE | AFP_SL_XATTR_REPLACE) == -EINVAL);
    CHECK(afp_sl_setxattr(&dummy_volume, "/file", "user.test", "x", 1,
                          0x4) == -EINVAL);
    CHECK(afp_sl_setresourcefork(&dummy_volume, "/file", "xx", 2,
                                 INT_MAX) == -EFBIG);
    CHECK(afp_sl_truncateresourcefork(&dummy_volume, "/file",
                                      (unsigned long long)INT_MAX + 1) == -EFBIG);
    CHECK(afp_sl_recovery_for_error(-ECONNRESET)
          == AFP_SL_RECOVERY_RECONNECT);
    CHECK(afp_sl_recovery_for_error(-ECONNREFUSED)
          == AFP_SL_RECOVERY_RECONNECT);
    CHECK(afp_sl_recovery_for_error(-ESTALE) == AFP_SL_RECOVERY_REATTACH);
    CHECK(afp_sl_recovery_for_error(-ENOATTR) == AFP_SL_RECOVERY_NONE);
    CHECK(mkdtemp(temporary) != NULL);
    CHECK(join_suffix(file, sizeof(file), temporary, "/file") == 0);
    CHECK(join_suffix(missing, sizeof(missing), temporary, "/missing") == 0);
    CHECK(join_suffix(macos_sidecar, sizeof(macos_sidecar), temporary,
                      "/._file") == 0);
    CHECK(join_suffix(netatalk_dir, sizeof(netatalk_dir), temporary,
                      "/.AppleDouble") == 0);
    CHECK(join_suffix(netatalk_sidecar, sizeof(netatalk_sidecar),
                      netatalk_dir, "/file") == 0);
    CHECK(join_suffix(ea_header, sizeof(ea_header),
                      netatalk_sidecar, "::EA") == 0);
    CHECK(join_suffix(ea_value, sizeof(ea_value),
                      netatalk_sidecar, "::EA::user.afpcmd-test") == 0);
    CHECK(create_file(file) == 0);
    CHECK(local_finderinfo_get(file, AFP_METADATA_MACOS,
                               actual_finder) == -ENOATTR);
    CHECK(local_resourcefork_size(file, AFP_METADATA_MACOS) == -ENOATTR);
    CHECK(local_metadata_list(file, AFP_METADATA_NETATALK,
                              &list, &list_size) == 0);
    CHECK(list == NULL && list_size == 0);
    CHECK(local_finderinfo_get(missing, AFP_METADATA_NETATALK,
                               actual_finder) == -ENOENT);
    CHECK(local_finderinfo_get(missing, AFP_METADATA_XATTR,
                               actual_finder) == -ENOENT);
    CHECK(local_finderinfo_get(missing, AFP_METADATA_MACOS,
                               actual_finder) == -ENOENT);
    CHECK(local_resourcefork_size(missing, AFP_METADATA_MACOS) == -ENOENT);
    CHECK(local_metadata_list(missing, AFP_METADATA_NETATALK,
                              &list, &list_size) == -ENOENT);
    CHECK(afp_metadata_clear_local(missing, AFP_METADATA_AUTO,
                                   &warnings) == -ENOENT);
    CHECK(afp_metadata_mode_parse("auto", &parsed_mode) == 0
          && parsed_mode == AFP_METADATA_AUTO);
    CHECK(strcmp(afp_metadata_mode_name(AFP_METADATA_AUTO), "auto") == 0);
    CHECK(afp_metadata_mode_parse("netatalk", &parsed_mode) == 0
          && parsed_mode == AFP_METADATA_NETATALK);
    CHECK(strcmp(afp_metadata_mode_name(AFP_METADATA_NETATALK),
                 "netatalk") == 0);
    CHECK(afp_metadata_mode_parse("xattr", &parsed_mode) == 0
          && parsed_mode == AFP_METADATA_XATTR);
    CHECK(strcmp(afp_metadata_mode_name(AFP_METADATA_XATTR), "xattr") == 0);
    CHECK(afp_metadata_mode_parse("sys", &parsed_mode) == -EINVAL);
    CHECK(afp_metadata_mode_parse("native", &parsed_mode) == -EINVAL);
    CHECK(afp_metadata_mode_parse("invalid", &parsed_mode) == -EINVAL);
    CHECK(afp_metadata_clear_local(file, AFP_METADATA_NONE, NULL) == 0);
    CHECK(metadata_name_filtered("org.netatalk.Metadata"));
    CHECK(metadata_name_filtered("user.org.netatalk.Metadata"));
    CHECK(metadata_name_filtered("com.apple.finder.copy.source"));

    for (size_t i = 0; i < sizeof(finder); i++) {
        finder[i] = (unsigned char)(i + 1);
    }

    for (size_t i = 0; i < sizeof(resource); i++) {
        resource[i] = (unsigned char)(i * 17);
    }

    CHECK(chmod(file, 0640) == 0);
    old_umask = umask(0000);
    CHECK(local_finderinfo_set(file, AFP_METADATA_MACOS, finder) == 0);
    umask(old_umask);
    CHECK(stat(macos_sidecar, &st) == 0);
    CHECK((st.st_mode & 0777) == 0640);
    CHECK(local_finderinfo_get(file, AFP_METADATA_MACOS, actual_finder) == 0);
    CHECK(memcmp(finder, actual_finder, sizeof(finder)) == 0);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS,
                                   resource, 4096, 0) == 0);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS,
                                   resource + 4096,
                                   sizeof(resource) - 4096, 4096) == 0);
    CHECK(local_resourcefork_size(file,
                                  AFP_METADATA_MACOS) == (off_t)sizeof(resource));
    CHECK(local_resourcefork_read(file, AFP_METADATA_MACOS, actual_resource,
                                  sizeof(actual_resource), 0)
          == (ssize_t)sizeof(actual_resource));
    CHECK(local_resourcefork_read(file, AFP_METADATA_MACOS, actual_resource,
                                  sizeof(actual_resource), -1) == -EINVAL);
    CHECK(local_resourcefork_read(file, AFP_METADATA_MACOS, NULL, 1, 0)
          == -EINVAL);
    CHECK(memcmp(resource, actual_resource, sizeof(resource)) == 0);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS, resource, 10, 0) == 0);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS, resource, 1, -1)
          == -EINVAL);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS, NULL, 1, 0)
          == -EINVAL);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS, resource, 2,
                                   SSIZE_MAX) == -EFBIG);
    CHECK(local_resourcefork_size(file, AFP_METADATA_MACOS) == 10);
    CHECK(overwrite(macos_sidecar, "bad", 3) == 0);
    CHECK(local_finderinfo_get(file, AFP_METADATA_MACOS, actual_finder) < 0);
    CHECK(local_finderinfo_set(file, AFP_METADATA_MACOS, finder) == 0);
    {
        int fd = open(macos_sidecar, O_RDWR);
        uint32_t duplicate_id = htonl(9);
        CHECK(fd >= 0);
        CHECK(pwrite(fd, &duplicate_id, sizeof(duplicate_id), 38)
              == (ssize_t)sizeof(duplicate_id));
        CHECK(close(fd) == 0);
        CHECK(local_finderinfo_get(file, AFP_METADATA_MACOS, actual_finder) == -EINVAL);
        CHECK(local_finderinfo_set(file, AFP_METADATA_MACOS, finder) == 0);
    }
    CHECK(local_finderinfo_set(file, AFP_METADATA_MACOS, finder) == 0);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS,
                                   resource, sizeof(resource), 0) == 0);
    CHECK(afp_metadata_clear_local(file, AFP_METADATA_MACOS, &warnings) == 0);
    CHECK(warnings == AFP_METADATA_WARNING_NONE);
    CHECK(local_finderinfo_get(file, AFP_METADATA_MACOS,
                               actual_finder) == -ENOATTR);
    CHECK(local_resourcefork_size(file, AFP_METADATA_MACOS) <= 0);
    CHECK(local_finderinfo_remove(file, AFP_METADATA_MACOS) == 0);
    CHECK(local_resourcefork_remove(file, AFP_METADATA_MACOS) == 0);
    CHECK(chmod(temporary, 0750) == 0);
    old_umask = umask(0000);
    CHECK(local_finderinfo_set(file, AFP_METADATA_NETATALK, finder) == 0);
    CHECK(local_resourcefork_write(file, AFP_METADATA_NETATALK,
                                   resource, sizeof(resource), 0) == 0);
    CHECK(local_metadata_set(file, AFP_METADATA_NETATALK, "user.afpcmd-test",
                             resource, 37) == 0);
    umask(old_umask);
    CHECK(stat(netatalk_dir, &st) == 0);
    CHECK((st.st_mode & 0777) == 0750);
    CHECK(stat(netatalk_sidecar, &st) == 0);
    CHECK((st.st_mode & 0777) == 0640);
    CHECK(stat(ea_header, &st) == 0);
    CHECK((st.st_mode & 0777) == 0640);
    CHECK(stat(ea_value, &st) == 0);
    CHECK((st.st_mode & 0777) == 0640);
    CHECK(local_metadata_list(file, AFP_METADATA_NETATALK, &list, &list_size) == 0);
    CHECK(contains_name(list, list_size, "user.afpcmd-test"));
    free(list);
    list = NULL;
    CHECK(local_metadata_get(file, AFP_METADATA_NETATALK, "user.afpcmd-test",
                             &value, &value_size) == 0);
    CHECK(value_size == 37 && memcmp(value, resource, value_size) == 0);
    free(value);
    value = NULL;
    CHECK(local_metadata_remove(file, AFP_METADATA_NETATALK,
                                "user.afpcmd-test") == 0);
    CHECK(local_metadata_get(file, AFP_METADATA_NETATALK, "user.afpcmd-test",
                             &value, &value_size) < 0);
    CHECK(local_metadata_set(file, AFP_METADATA_NETATALK, "bad/name",
                             resource, 1) == -EINVAL);
#ifndef __APPLE__
    memset(macos_finder, 0xa5, sizeof(macos_finder));
    CHECK(local_finderinfo_set(file, AFP_METADATA_MACOS, macos_finder) == 0);
    CHECK(local_resourcefork_write(file, AFP_METADATA_MACOS,
                                   "macos", 5, 0) == 0);
    CHECK(local_finderinfo_get(file, AFP_METADATA_AUTO, actual_finder) == 0);
    CHECK(memcmp(actual_finder, macos_finder, sizeof(actual_finder)) == 0);
    CHECK(local_resourcefork_size(file, AFP_METADATA_AUTO) == 5);
    CHECK(local_resourcefork_read(file, AFP_METADATA_AUTO, actual_resource,
                                  sizeof(actual_resource), 0) == 5);
    CHECK(memcmp(actual_resource, "macos", 5) == 0);
    CHECK(local_finderinfo_remove(file, AFP_METADATA_AUTO) == 0);
    CHECK(local_resourcefork_remove(file, AFP_METADATA_AUTO) == 0);
    CHECK(local_finderinfo_get(file, AFP_METADATA_MACOS,
                               actual_finder) == -ENOATTR);
    CHECK(local_resourcefork_size(file, AFP_METADATA_MACOS) <= 0);
#endif
    CHECK(local_finderinfo_remove(file, AFP_METADATA_NONE) == -ENOTSUP);
    CHECK(local_resourcefork_remove(file, AFP_METADATA_NONE) == -ENOTSUP);
    {
        unsigned char duplicate[] = {
            0x61, 0x64, 0x45, 0x41, 0x00, 0x01, 0x00, 0x02,
            0x00, 0x00, 0x00, 0x01, 'd', 'u', 'p', 0,
            0x00, 0x00, 0x00, 0x01, 'd', 'u', 'p', 0,
        };
        CHECK(overwrite(ea_header, duplicate, sizeof(duplicate)) == 0);
        CHECK(local_metadata_list(file, AFP_METADATA_NETATALK,
                                  &list, &list_size) == -EINVAL);
        CHECK(unlink(ea_header) == 0);
    }
    {
        unsigned char oversized[] = {
            0x61, 0x64, 0x45, 0x41, 0x00, 0x01, 0x04, 0x01,
        };
        CHECK(overwrite(ea_header, oversized, sizeof(oversized)) == 0);
        CHECK(local_metadata_list(file, AFP_METADATA_NETATALK,
                                  &list, &list_size) == -EINVAL);
        CHECK(unlink(ea_header) == 0);
    }
    {
        int sys_ret = local_metadata_set(file, AFP_METADATA_XATTR,
                                         "user.afpcmd-system", resource, 19);

        if (sys_ret == 0) {
            CHECK(local_resourcefork_write(file, AFP_METADATA_XATTR,
                                           resource, 64, 0) == 0);
            CHECK(local_resourcefork_read(file, AFP_METADATA_XATTR,
                                          actual_resource, 20, 7) == 20);
            CHECK(memcmp(actual_resource, resource + 7, 20) == 0);
            CHECK(local_resourcefork_write(file, AFP_METADATA_XATTR,
                                           resource + 100, 8, 11) == 0);
            CHECK(local_resourcefork_read(file, AFP_METADATA_XATTR,
                                          actual_resource, 64, 0) == 64);
            CHECK(memcmp(actual_resource, resource, 11) == 0);
            CHECK(memcmp(actual_resource + 11, resource + 100, 8) == 0);
            CHECK(memcmp(actual_resource + 19, resource + 19, 45) == 0);
            CHECK(local_metadata_get(file, AFP_METADATA_XATTR, "user.afpcmd-system",
                                     &value, &value_size) == 0);
            CHECK(value_size == 19 && memcmp(value, resource, value_size) == 0);
            free(value);
            value = NULL;
            CHECK(local_metadata_remove(file, AFP_METADATA_XATTR,
                                        "user.afpcmd-system") == 0);
            CHECK(local_metadata_set(file, AFP_METADATA_XATTR,
                                     "user.afpcmd-merge", "system", 6) == 0);
            CHECK(local_metadata_get(file, AFP_METADATA_AUTO,
                                     "user.afpcmd-merge", &value, &value_size) == 0);
            CHECK(value_size == 6 && memcmp(value, "system", 6) == 0);
            free(value);
            value = NULL;
            CHECK(local_metadata_remove(file, AFP_METADATA_AUTO,
                                        "user.afpcmd-merge") == 0);
            CHECK(local_metadata_get(file, AFP_METADATA_AUTO,
                                     "user.afpcmd-merge", &value, &value_size) < 0);
        } else {
            CHECK(sys_ret == -ENOTSUP || sys_ret == -EOPNOTSUPP);
        }
    }
    unlink(ea_value);
    unlink(ea_header);
    unlink(netatalk_sidecar);
    unlink(macos_sidecar);
    unlink(file);
    rmdir(netatalk_dir);
    rmdir(temporary);
    return test_tap_finish();
}
