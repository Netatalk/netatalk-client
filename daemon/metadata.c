/*
 * Local metadata storage and transfer helpers for libafpsl.
 * The "netatalk" metadata storage uses the .AppleDouble sidecar file format
 * invented by the authors of Netatalk, while "xattr" and "macos" emulate
 * the macOS extended attribute storage format.
 *
 * Rules, layouts, and algorithms are informed by Netatalk's AppleDouble
 * implementation, notably libatalk/adouble/ad_open.c and
 * libatatalk/vfs/ea_ad.c
 *
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "metadata.h"
#include "afpsl.h"
#include "afp_xattr.h"

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

#if defined(HAVE_ATTR_XATTR_H)
#include <attr/xattr.h>
#elif defined(HAVE_SYS_XATTR_H)
#include <sys/xattr.h>
#elif defined(HAVE_SYS_EXTATTR_H)
#include <sys/extattr.h>
#endif

#define COPY_SOURCE "com.apple.finder.copy.source"
#define COPY_CHECKPOINT "com.apple.finder.copy.checkpoint#"
#define RESUMABLE_COPY "com.apple.metadata:kMDItemResumableCopy"

#define SYS_NAME_MAX (AFP_XATTR_USER_PREFIX_LEN + AFP_XATTR_NAME_MAX)
#define SYS_NAME_SIZE (SYS_NAME_MAX + 1U)

#define AD_MAGIC 0x00051607U
#define AD_VERSION 0x00020000U
#define AD_HEADER_SIZE 26U
#define AD_ENTRY_SIZE 12U
#define AD_FINDER_ID 9U
#define AD_RESOURCE_ID 2U
#define AD_MACOS_DATA_OFFSET 82U
#define AD_NETATALK_DATA_OFFSET 741U

#define EA_MAGIC 0x61644541U
#define EA_VERSION 1U
#define EA_HEADER_SIZE 8U

struct ad_entry {
    uint32_t id;
    uint32_t offset;
    uint32_t length;
    size_t header_offset;
};

struct ad_info {
    struct ad_entry finder;
    struct ad_entry resource;
};

struct ea_item {
    char *name;
    uint32_t size;
};

static int metadata_absent(int error)
{
    return error == ENOATTR || error == ENODATA;
}

static int metadata_unsupported(int error)
{
    return error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS;
}

static int local_path_check(const char *path)
{
    struct stat st;

    if (!path) {
        return -EINVAL;
    }

    return stat(path, &st) == 0 ? 0 : -errno;
}

int afp_metadata_mode_parse(const char *name, enum afp_metadata_mode *mode)
{
    if (!name || !mode) {
        return -EINVAL;
    }

    if (strcmp(name, "auto") == 0) {
        *mode = AFP_METADATA_AUTO;
    } else if (strcmp(name, "netatalk") == 0) {
        *mode = AFP_METADATA_NETATALK;
    } else if (strcmp(name, "xattr") == 0) {
        *mode = AFP_METADATA_XATTR;
    } else if (strcmp(name, "macos") == 0) {
        *mode = AFP_METADATA_MACOS;
    } else if (strcmp(name, "none") == 0) {
        *mode = AFP_METADATA_NONE;
    } else {
        return -EINVAL;
    }

    return 0;
}

static int metadata_mode_valid(enum afp_metadata_mode mode)
{
    return mode >= AFP_METADATA_AUTO && mode <= AFP_METADATA_NONE;
}

const char *afp_metadata_mode_name(enum afp_metadata_mode mode)
{
    switch (mode) {
    case AFP_METADATA_AUTO:
        return "auto";

    case AFP_METADATA_NETATALK:
        return "netatalk";

    case AFP_METADATA_XATTR:
        return "xattr";

    case AFP_METADATA_MACOS:
        return "macos";

    case AFP_METADATA_NONE:
        return "none";
    }

    return "unknown";
}

int metadata_name_filtered(const char *name)
{
    if (!name) {
        return 1;
    }

    name = afp_xattr_strip_user_prefix(name);
    /* Filter out metadata of internal macOS or Netatalk use */
    return strncmp(name, NETATALK_XATTR_META, NETATALK_XATTR_META_LEN) == 0
           || strcmp(name, COPY_SOURCE) == 0
           || strcmp(name, RESUMABLE_COPY) == 0
           || strncmp(name, COPY_CHECKPOINT,
                      sizeof(COPY_CHECKPOINT) - 1U) == 0;
}

static int metadata_name_valid(const char *name)
{
    size_t length;

    if (!name) {
        return 0;
    }

    length = strnlen(name, AFP_XATTR_NAME_MAX + 1U);
    return length > 0 && length <= AFP_XATTR_NAME_MAX
           && memchr(name, '/', length) == NULL;
}

static const char *sys_name(const char *name, char mapped[SYS_NAME_SIZE])
{
#if defined(__APPLE__)
    (void)mapped;
    return name;
#else

    if (strncmp(name, AFP_XATTR_USER_PREFIX,
                AFP_XATTR_USER_PREFIX_LEN) == 0) {
        return name;
    }

    snprintf(mapped, SYS_NAME_SIZE, AFP_XATTR_USER_PREFIX "%s", name);
    return mapped;
#endif
}

static ssize_t sys_get(const char *path, const char *name, void *value,
                       size_t size)
{
    char mapped[SYS_NAME_SIZE];
    const char *actual = sys_name(name, mapped);
#if defined(__APPLE__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
    return getxattr(path, actual, value, size, 0, 0);
#elif defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H)
    return getxattr(path, actual, value, size);
#elif defined(HAVE_SYS_EXTATTR_H)
    const char *bare = afp_xattr_strip_user_prefix(actual);
    return extattr_get_file(path, EXTATTR_NAMESPACE_USER, bare, value, size);
#else
    (void)path;
    (void)actual;
    (void)value;
    (void)size;
    errno = ENOTSUP;
    return -1;
#endif
}

static int sys_set(const char *path, const char *name,
                   const void *value, size_t size)
{
    char mapped[SYS_NAME_SIZE];
    const char *actual = sys_name(name, mapped);
#if defined(__APPLE__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
    return setxattr(path, actual, value, size, 0, 0);
#elif defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H)
    return setxattr(path, actual, value, size, 0);
#elif defined(HAVE_SYS_EXTATTR_H)
    const char *bare = afp_xattr_strip_user_prefix(actual);
    return extattr_set_file(path, EXTATTR_NAMESPACE_USER, bare, value,
                            size) < 0 ? -1 : 0;
#else
    (void)path;
    (void)actual;
    (void)value;
    (void)size;
    errno = ENOTSUP;
    return -1;
#endif
}

static int sys_remove(const char *path, const char *name)
{
    char mapped[SYS_NAME_SIZE];
    const char *actual = sys_name(name, mapped);
#if defined(__APPLE__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
    return removexattr(path, actual, 0);
#elif defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H)
    return removexattr(path, actual);
#elif defined(HAVE_SYS_EXTATTR_H)
    const char *bare = afp_xattr_strip_user_prefix(actual);
    return extattr_delete_file(path, EXTATTR_NAMESPACE_USER, bare);
#else
    (void)path;
    (void)actual;
    errno = ENOTSUP;
    return -1;
#endif
}

static int sys_xattrs_supported(const char *path)
{
    ssize_t ret;
#if defined(__APPLE__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
    ret = listxattr(path, NULL, 0, 0);
#elif defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H)
    ret = listxattr(path, NULL, 0);
#elif defined(HAVE_SYS_EXTATTR_H)
    ret = extattr_list_file(path, EXTATTR_NAMESPACE_USER, NULL, 0);
#else
    (void)path;
    return 0;
#endif

    if (ret >= 0) {
        return 1;
    }

    return metadata_unsupported(errno) ? 0 : -errno;
}

static int resolve_generic_mode(const char *path, enum afp_metadata_mode mode,
                                enum afp_metadata_mode *resolved)
{
    int supported;

    if (mode != AFP_METADATA_AUTO) {
        *resolved = mode;
        return 0;
    }

    supported = sys_xattrs_supported(path);

    if (supported < 0) {
        return supported;
    }

    *resolved = supported ? AFP_METADATA_XATTR : AFP_METADATA_NETATALK;
    return 0;
}

static int apple_metadata_uses_sys(enum afp_metadata_mode mode)
{
#if defined(__APPLE__)
    return mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR;
#else
    (void)mode;
    return 0;
#endif
}

static int append_name(char **list, size_t *used, const char *name)
{
    size_t name_len = strnlen(name, SYS_NAME_SIZE);
    size_t length;
    char *grown;
    size_t pos = 0;

    if (name_len == SYS_NAME_SIZE) {
        return -ENAMETOOLONG;
    }

    length = name_len + 1U;

    while (pos < *used) {
        size_t current_len = strnlen(*list + pos, *used - pos);

        if (current_len == *used - pos) {
            return -EIO;
        }

        if (strcmp(*list + pos, name) == 0) {
            return 0;
        }

        pos += current_len + 1U;
    }

    grown = realloc(*list, *used + length);

    if (!grown) {
        return -ENOMEM;
    }

    memcpy(grown + *used, name, length);
    *list = grown;
    *used += length;
    return 0;
}

static int sys_list(const char *path, char **list, size_t *size)
{
    ssize_t needed;
    char *raw;
    size_t used = 0;
#if defined(__APPLE__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
    needed = listxattr(path, NULL, 0, 0);
#elif defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H)
    needed = listxattr(path, NULL, 0);
#elif defined(HAVE_SYS_EXTATTR_H)
    needed = extattr_list_file(path, EXTATTR_NAMESPACE_USER, NULL, 0);
#else
    (void)path;
    (void)list;
    (void)size;
    return -ENOTSUP;
#endif

    if (needed < 0) {
        return -errno;
    }

    if (needed == 0) {
        *list = NULL;
        *size = 0;
        return 0;
    }

    raw = malloc((size_t)needed);

    if (!raw) {
        return -ENOMEM;
    }

#if defined(__APPLE__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
    needed = listxattr(path, raw, (size_t)needed, 0);
#elif defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H)
    needed = listxattr(path, raw, (size_t)needed);
#elif defined(HAVE_SYS_EXTATTR_H)
    needed = extattr_list_file(path, EXTATTR_NAMESPACE_USER, raw, (size_t)needed);
#endif

    if (needed < 0) {
        int error = errno;
        free(raw);
        return -error;
    }

#if defined(HAVE_SYS_EXTATTR_H) && !defined(HAVE_SYS_XATTR_H) && !defined(HAVE_ATTR_XATTR_H)

    for (size_t pos = 0; pos < (size_t)needed;) {
        unsigned char length = (unsigned char)raw[pos++];
        char name[SYS_NAME_SIZE];

        if (length == 0 || pos + length > (size_t)needed) {
            free(raw);
            return -EIO;
        }

        snprintf(name, sizeof(name), AFP_XATTR_USER_PREFIX "%.*s", length,
                 raw + pos);

        if (!metadata_name_filtered(name)) {
            int ret = append_name(list, &used, name);

            if (ret < 0) {
                free(raw);
                free(*list);
                *list = NULL;
                return ret;
            }
        }

        pos += length;
    }

#else

    for (size_t pos = 0; pos < (size_t)needed;) {
        size_t length = strnlen(raw + pos, (size_t)needed - pos);

        if (length == (size_t)needed - pos) {
            free(raw);
            free(*list);
            return -EIO;
        }

        if (!metadata_name_filtered(raw + pos)) {
            int ret = append_name(list, &used, raw + pos);

            if (ret < 0) {
                free(raw);
                free(*list);
                *list = NULL;
                return ret;
            }
        }

        pos += length + 1;
    }

#endif
    free(raw);
    *size = used;
    return 0;
}

static int sidecar_path(const char *path, int netatalk, char *out, size_t size)
{
    struct stat st;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t dirlen = slash ? (size_t)(slash - path + 1) : 0;
    int directory = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
    int n;

    if (!netatalk) {
        n = snprintf(out, size, "%.*s._%s", (int)dirlen, path, base);
    } else if (directory) {
        n = snprintf(out, size, "%s/.AppleDouble/.Parent", path);
    } else {
        n = snprintf(out, size, "%.*s.AppleDouble/%s", (int)dirlen, path, base);
    }

    return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

static int ensure_sidecar_dir(const char *sidecar)
{
    char parent[PATH_MAX];
    char containing[PATH_MAX];
    char *slash;
    struct stat st;
    mode_t mode;
    strlcpy(parent, sidecar, sizeof(parent));
    slash = strrchr(parent, '/');

    if (!slash) {
        return 0;
    }

    *slash = '\0';
    slash = strrchr(parent, '/');

    if (strcmp(slash ? slash + 1 : parent, ".AppleDouble") != 0) {
        return 0;
    }

    if (!slash) {
        strlcpy(containing, ".", sizeof(containing));
    } else if (slash == parent) {
        strlcpy(containing, "/", sizeof(containing));
    } else {
        size_t length = (size_t)(slash - parent);
        memcpy(containing, parent, length);
        containing[length] = '\0';
    }

    if (stat(containing, &st) < 0) {
        return -errno;
    }

    mode = st.st_mode & 0777;

    if (mkdir(parent, mode) < 0
            && errno != EEXIST) {
        return -errno;
    }

    return 0;
}

static int sidecar_file_mode(const char *path, mode_t *mode)
{
    struct stat st;

    if (stat(path, &st) < 0) {
        return -errno;
    }

    *mode = st.st_mode & 0666;
    return 0;
}

static int read_ad_info(int fd, struct ad_info *info)
{
    unsigned char header[AD_HEADER_SIZE];
    uint32_t value;
    uint16_t count;
    struct stat st;
    memset(info, 0, sizeof(*info));

    if (pread(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header)) {
        return -EIO;
    }

    memcpy(&value, header, 4);

    if (ntohl(value) != AD_MAGIC) {
        return -EINVAL;
    }

    memcpy(&value, header + 4, 4);

    if (ntohl(value) != AD_VERSION) {
        return -EINVAL;
    }

    memcpy(&count, header + 24, 2);
    count = ntohs(count);

    if (count == 0 || count > 32 || fstat(fd, &st) < 0) {
        return -EINVAL;
    }

    for (uint16_t i = 0; i < count; i++) {
        unsigned char entry[AD_ENTRY_SIZE];
        struct ad_entry parsed;

        if (pread(fd, entry, sizeof(entry), AD_HEADER_SIZE + i * AD_ENTRY_SIZE)
                != (ssize_t)sizeof(entry)) {
            return -EIO;
        }

        memcpy(&value, entry, 4);
        parsed.id = ntohl(value);
        memcpy(&value, entry + 4, 4);
        parsed.offset = ntohl(value);
        memcpy(&value, entry + 8, 4);
        parsed.length = ntohl(value);
        parsed.header_offset = AD_HEADER_SIZE + i * AD_ENTRY_SIZE;

        if ((uint64_t)parsed.offset + parsed.length > (uint64_t)st.st_size) {
            return -EINVAL;
        }

        if (parsed.id == AD_FINDER_ID) {
            if (info->finder.offset != 0) {
                return -EINVAL;
            }

            info->finder = parsed;
        }

        if (parsed.id == AD_RESOURCE_ID) {
            if (info->resource.offset != 0) {
                return -EINVAL;
            }

            info->resource = parsed;
        }
    }

    return 0;
}

static void put_u32(unsigned char *p, uint32_t value)
{
    value = htonl(value);
    memcpy(p, &value, sizeof(value));
}

static void put_u16(unsigned char *p, uint16_t value)
{
    value = htons(value);
    memcpy(p, &value, sizeof(value));
}

static int initialize_ad(int fd, int netatalk)
{
    unsigned int data_offset = netatalk ? AD_NETATALK_DATA_OFFSET :
                               AD_MACOS_DATA_OFFSET;
    uint16_t count = netatalk ? 13 : 2;
    unsigned char *header = calloc(1, data_offset);
    size_t pos = AD_HEADER_SIZE;
    static const uint32_t ids[] = {2, 3, 4, 8, 9, 11, 13, 14, 15,
                                   0x80444556, 0x80494e4f, 0x8053594e, 0x8053567e
                                  };
    static const uint32_t offsets[] = {741, 182, 437, 637, 653, 705, 693,
                                       689, 685, 713, 721, 729, 737
                                      };
    static const uint32_t lengths[] = {0, 0, 200, 16, 32, 8, 0, 4, 4, 8, 8, 8, 4};

    if (!header) {
        return -ENOMEM;
    }

    put_u32(header, AD_MAGIC);
    put_u32(header + 4, AD_VERSION);

    if (netatalk) {
        memcpy(header + 8, "Netatalk        ", 16);
    }

    put_u16(header + 24, count);

    if (netatalk) {
        for (unsigned int i = 0; i < count; i++, pos += AD_ENTRY_SIZE) {
            put_u32(header + pos, ids[i]);
            put_u32(header + pos + 4, offsets[i]);
            put_u32(header + pos + 8, lengths[i]);
        }
    } else {
        put_u32(header + pos, AD_FINDER_ID);
        put_u32(header + pos + 4, 50);
        put_u32(header + pos + 8, 32);
        pos += AD_ENTRY_SIZE;
        put_u32(header + pos, AD_RESOURCE_ID);
        put_u32(header + pos + 4, AD_MACOS_DATA_OFFSET);
    }

    if (ftruncate(fd, data_offset) < 0) {
        int error = errno;
        free(header);
        return -error;
    }

    if (pwrite(fd, header, data_offset, 0) != (ssize_t)data_offset) {
        free(header);
        return -EIO;
    }

    free(header);
    return 0;
}

static int sidecar_open_error(const char *path, int error)
{
    struct stat st;

    if (error != ENOENT) {
        return -error;
    }

    if (stat(path, &st) == 0) {
        return -ENOATTR;
    }

    return -errno;
}

static int open_ad(const char *path, int netatalk, int write,
                   int *fd, struct ad_info *info)
{
    char sidecar[PATH_MAX];
    mode_t mode = 0;
    int ret = sidecar_path(path, netatalk, sidecar, sizeof(sidecar));

    if (ret < 0) {
        return ret;
    }

    if (write && ((ret = ensure_sidecar_dir(sidecar)) < 0
                  || (ret = sidecar_file_mode(path, &mode)) < 0)) {
        return ret;
    }

    *fd = write ? open(sidecar, O_RDWR | O_CREAT, mode)
          : open(sidecar, O_RDONLY);

    if (*fd < 0) {
        return write ? -errno : sidecar_open_error(path, errno);
    }

    ret = read_ad_info(*fd, info);

    if (ret < 0 && write) {
        ret = initialize_ad(*fd, netatalk);

        if (ret == 0) {
            ret = read_ad_info(*fd, info);
        }
    }

    if (ret < 0) {
        close(*fd);
        *fd = -1;
    }

    return ret;
}

static int ad_finder_get(const char *path, int netatalk,
                         unsigned char value[32])
{
    struct ad_info info = {0};
    int fd, ret = open_ad(path, netatalk, 0, &fd, &info);

    if (ret < 0) {
        return ret;
    }

    if (info.finder.offset == 0 || info.finder.length < 32) {
        ret = -ENOATTR;
    } else if (pread(fd, value, 32, info.finder.offset) != 32) {
        ret = -EIO;
    } else {
        unsigned char empty[32] = {0};
        ret = memcmp(value, empty, 32) == 0 ? -ENOATTR : 0;
    }

    close(fd);
    return ret;
}

static int ad_finder_set(const char *path, int netatalk,
                         const unsigned char value[32])
{
    struct ad_info info = {0};
    int fd, ret = open_ad(path, netatalk, 1, &fd, &info);

    if (ret < 0) {
        return ret;
    }

    if (info.finder.offset == 0
            || pwrite(fd, value, 32, info.finder.offset) != 32) {
        ret = -EIO;
    }

    close(fd);
    return ret;
}

static off_t ad_resource_size(const char *path, int netatalk)
{
    struct ad_info info = {0};
    int fd, ret = open_ad(path, netatalk, 0, &fd, &info);

    if (ret < 0) {
        return ret;
    }

    close(fd);
    return info.resource.length ? (off_t)info.resource.length : -ENOATTR;
}

static ssize_t ad_resource_read(const char *path, int netatalk,
                                void *buf, size_t size, off_t offset)
{
    struct ad_info info = {0};
    int fd, ret = open_ad(path, netatalk, 0, &fd, &info);
    ssize_t amount;

    if (ret < 0) {
        return ret;
    }

    if (offset < 0 || (uint64_t)offset >= info.resource.length) {
        amount = 0;
    } else {
        size_t available = info.resource.length - (size_t)offset;

        if (size > available) {
            size = available;
        }

        amount = pread(fd, buf, size, info.resource.offset + offset);

        if (amount < 0) {
            amount = -errno;
        }
    }

    close(fd);
    return amount;
}

static int ad_resource_write(const char *path, int netatalk,
                             const void *buf, size_t size, off_t offset)
{
    struct ad_info info = {0};
    int fd, ret = open_ad(path, netatalk, 1, &fd, &info);
    uint64_t end;

    if (ret < 0) {
        return ret;
    }

    if (offset < 0 || info.resource.offset == 0
            || (uint64_t)offset > UINT32_MAX
            || size > UINT32_MAX - (uint64_t)offset) {
        close(fd);
        return -EFBIG;
    }

    if (offset == 0 && ftruncate(fd, info.resource.offset) < 0) {
        ret = -errno;
    }

    if (ret == 0 && size > 0
            && pwrite(fd, buf, size, info.resource.offset + offset) != (ssize_t)size) {
        ret = -EIO;
    }

    end = (uint64_t)offset + size;

    if (ret == 0 && offset != 0 && end < info.resource.length) {
        end = info.resource.length;
    }

    if (ret == 0) {
        unsigned char length[4];
        put_u32(length, (uint32_t)end);

        if (pwrite(fd, length, 4, info.resource.header_offset + 8) != 4) {
            ret = -EIO;
        }
    }

    close(fd);
    return ret;
}

static int ea_paths(const char *path, char *header, size_t header_size,
                    const char *name, char *value, size_t value_size)
{
    char adpath[PATH_MAX];
    int ret = sidecar_path(path, 1, adpath, sizeof(adpath));

    if (ret < 0) {
        return ret;
    }

    if (snprintf(header, header_size, "%s::EA", adpath) >= (int)header_size) {
        return -ENAMETOOLONG;
    }

    if (name && snprintf(value, value_size, "%s::EA::%s", adpath,
                         name) >= (int)value_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

static void free_ea_items(struct ea_item *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(items[i].name);
    }

    free(items);
}

static int load_ea_items(const char *path, struct ea_item **items,
                         size_t *count)
{
    char header[PATH_MAX], unused[PATH_MAX];
    unsigned char fixed[EA_HEADER_SIZE];
    struct stat st;
    int fd, ret = ea_paths(path, header, sizeof(header), NULL, unused,
                           sizeof(unused));
    uint16_t n;
    off_t pos = EA_HEADER_SIZE;

    if (ret < 0) {
        return ret;
    }

    fd = open(header, O_RDONLY);

    if (fd < 0) {
        return sidecar_open_error(path, errno);
    }

    if (fstat(fd, &st) < 0 || st.st_size < EA_HEADER_SIZE
            || read(fd, fixed, sizeof(fixed)) != (ssize_t)sizeof(fixed)) {
        close(fd);
        return -EIO;
    }

    uint32_t magic;
    memcpy(&magic, fixed, 4);
    uint16_t version;
    memcpy(&version, fixed + 4, 2);
    memcpy(&n, fixed + 6, 2);

    if (ntohl(magic) != EA_MAGIC || ntohs(version) != EA_VERSION
            || ntohs(n) > 1024) {
        close(fd);
        return -EINVAL;
    }

    *count = ntohs(n);
    *items = calloc(*count, sizeof(**items));

    if (*count && !*items) {
        close(fd);
        return -ENOMEM;
    }

    for (size_t i = 0; i < *count; i++) {
        uint32_t length;
        char c = 1;
        size_t name_len = 0;

        if (pread(fd, &length, 4, pos) != 4) {
            ret = -EIO;
            goto fail;
        }

        pos += 4;
        (*items)[i].size = ntohl(length);

        while (pos < st.st_size && name_len < 255) {
            if (pread(fd, &c, 1, pos++) != 1) {
                ret = -EIO;
                goto fail;
            }

            if (c == '\0') {
                break;
            }

            name_len++;
        }

        if (c != '\0') {
            ret = -EINVAL;
            goto fail;
        }

        (*items)[i].name = malloc(name_len + 1);

        if (!(*items)[i].name) {
            ret = -ENOMEM;
            goto fail;
        }

        if (pread(fd, (*items)[i].name, name_len,
                  pos - name_len - 1) != (ssize_t)name_len) {
            ret = -EIO;
            goto fail;
        }

        (*items)[i].name[name_len] = '\0';

        for (size_t previous = 0; previous < i; previous++) {
            if (strcmp((*items)[previous].name, (*items)[i].name) == 0) {
                ret = -EINVAL;
                goto fail;
            }
        }
    }

    close(fd);
    return 0;
fail:
    close(fd);
    free_ea_items(*items, *count);
    *items = NULL;
    *count = 0;
    return ret;
}

static int save_ea_items(const char *path, struct ea_item *items, size_t count)
{
    char header[PATH_MAX], unused[PATH_MAX], adpath[PATH_MAX];
    mode_t mode = 0;
    size_t total = EA_HEADER_SIZE;
    unsigned char *buf;
    size_t pos = EA_HEADER_SIZE;
    int ret = ea_paths(path, header, sizeof(header), NULL, unused, sizeof(unused));

    if (ret < 0) {
        return ret;
    }

    if (count == 0) {
        if (unlink(header) < 0 && errno != ENOENT) {
            return -errno;
        }

        return 0;
    }

    if ((ret = sidecar_path(path, 1, adpath, sizeof(adpath))) < 0
            || (ret = ensure_sidecar_dir(adpath)) < 0
            || (ret = sidecar_file_mode(path, &mode)) < 0) {
        return ret;
    }

    for (size_t i = 0; i < count; i++) {
        size_t name_len = strnlen(items[i].name, AFP_XATTR_NAME_MAX + 1U);

        if (name_len > AFP_XATTR_NAME_MAX) {
            return -EINVAL;
        }

        total += 4U + name_len + 1U;
    }

    buf = calloc(1, total);

    if (!buf) {
        return -ENOMEM;
    }

    put_u32(buf, EA_MAGIC);
    put_u16(buf + 4, EA_VERSION);
    put_u16(buf + 6, (uint16_t)count);

    for (size_t i = 0; i < count; i++) {
        size_t name_len = strnlen(items[i].name, AFP_XATTR_NAME_MAX + 1U);
        put_u32(buf + pos, items[i].size);
        pos += 4;
        memcpy(buf + pos, items[i].name, name_len + 1U);
        pos += name_len + 1U;
    }

    int fd = open(header, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (fd < 0) {
        ret = -errno;
    } else if (write(fd, buf, total) != (ssize_t)total) {
        ret = -EIO;
    } else {
        ret = 0;
    }

    if (fd >= 0) {
        close(fd);
    }

    free(buf);
    return ret;
}

static int netatalk_list(const char *path, char **list, size_t *size)
{
    struct ea_item *items = NULL;
    size_t count = 0, used = 0;
    int ret = load_ea_items(path, &items, &count);

    if (ret < 0 && metadata_absent(-ret)) {
        *list = NULL;
        *size = 0;
        return 0;
    }

    if (ret < 0) {
        return ret;
    }

    for (size_t i = 0; i < count; i++) {
        if (!metadata_name_filtered(items[i].name)) {
            ret = append_name(list, &used, items[i].name);

            if (ret < 0) {
                free(*list);
                *list = NULL;
                used = 0;
                break;
            }
        }
    }

    free_ea_items(items, count);
    *size = used;
    return ret;
}

static int netatalk_get(const char *path, const char *name, void **value,
                        size_t *size)
{
    struct ea_item *items = NULL;
    size_t count = 0;
    char header[PATH_MAX], file[PATH_MAX];
    int ret = load_ea_items(path, &items, &count);

    if (ret < 0) {
        return ret;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i].name, name) == 0) {
            ret = ea_paths(path, header, sizeof(header), name, file, sizeof(file));

            if (ret < 0) {
                break;
            }

            int fd = open(file, O_RDONLY);

            if (fd < 0) {
                break;
            }

            *value = malloc(items[i].size ? items[i].size : 1);
            *size = items[i].size;

            if (!*value) {
                ret = -ENOMEM;
            } else if (read(fd, *value, *size) != (ssize_t) * size) {
                free(*value);
                *value = NULL;
                ret = -EIO;
            } else {
                ret = 0;
            }

            close(fd);
            goto out;
        }
    }

    ret = -ENOATTR;
out:
    free_ea_items(items, count);
    return ret;
}

static int netatalk_set(const char *path, const char *name, const void *value,
                        size_t size)
{
    struct ea_item *items = NULL;
    size_t count = 0;
    char header[PATH_MAX], file[PATH_MAX];
    mode_t mode = 0;
    int ret;

    if (size > UINT32_MAX) {
        return -E2BIG;
    }

    ret = load_ea_items(path, &items, &count);

    if (ret < 0 && metadata_absent(-ret)) {
        ret = 0;
    }

    if (ret < 0) {
        return ret;
    }

    size_t index;

    for (index = 0; index < count;
            index++) if (strcmp(items[index].name, name) == 0) {
            break;
        }

    if (index == count) {
        struct ea_item *grown = realloc(items, (count + 1) * sizeof(*items));

        if (!grown) {
            free_ea_items(items, count);
            return -ENOMEM;
        }

        items = grown;
        items[count].name = strdup(name);
        items[count].size = (uint32_t)size;

        if (!items[count].name) {
            free_ea_items(items, count);
            return -ENOMEM;
        }

        count++;
    } else {
        items[index].size = (uint32_t)size;
    }

    ret = ea_paths(path, header, sizeof(header), name, file, sizeof(file));

    if (ret == 0) {
        ret = ensure_sidecar_dir(file);
    }

    if (ret == 0) {
        ret = sidecar_file_mode(path, &mode);
    }

    if (ret == 0) {
        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, mode);

        if (fd < 0) {
            ret = -errno;
        } else if (size > 0 && write(fd, value, size) != (ssize_t)size) {
            ret = -EIO;
        }

        if (fd >= 0) {
            close(fd);
        }
    }

    if (ret == 0) {
        ret = save_ea_items(path, items, count);
    }

    free_ea_items(items, count);
    return ret;
}

static int netatalk_remove(const char *path, const char *name)
{
    struct ea_item *items = NULL;
    size_t count = 0;
    char header[PATH_MAX], file[PATH_MAX];
    int ret = load_ea_items(path, &items, &count);

    if (ret < 0) {
        return ret;
    }

    size_t index;

    for (index = 0; index < count;
            index++) if (strcmp(items[index].name, name) == 0) {
            break;
        }

    if (index == count) {
        free_ea_items(items, count);
        return -ENOATTR;
    }

    ret = ea_paths(path, header, sizeof(header), name, file, sizeof(file));

    if (ret == 0 && unlink(file) < 0 && errno != ENOENT) {
        ret = -errno;
    }

    free(items[index].name);
    memmove(items + index, items + index + 1, (count - index - 1) * sizeof(*items));
    count--;

    if (ret == 0) {
        ret = save_ea_items(path, items, count);
    }

    free_ea_items(items, count);
    return ret;
}

int local_metadata_list(const char *path, enum afp_metadata_mode mode,
                        char **list,
                        size_t *size)
{
    int ret;
    enum afp_metadata_mode resolved;
    *list = NULL;
    *size = 0;
    ret = local_path_check(path);

    if (ret < 0) {
        return ret;
    }

    ret = resolve_generic_mode(path, mode, &resolved);

    if (ret < 0) {
        return ret;
    }

    if (resolved == AFP_METADATA_XATTR) {
        return sys_list(path, list, size);
    }

    if (resolved == AFP_METADATA_NETATALK) {
        return netatalk_list(path, list, size);
    }

    if (resolved == AFP_METADATA_MACOS || resolved == AFP_METADATA_NONE) {
        return 0;
    }

    return -EINVAL;
}

int local_metadata_get(const char *path, enum afp_metadata_mode mode,
                       const char *name, void **value, size_t *size)
{
    ssize_t needed;
    enum afp_metadata_mode resolved;
    int ret = local_path_check(path);

    if (ret < 0) {
        return ret;
    }

    if (!metadata_name_valid(name)) {
        return -EINVAL;
    }

    if (metadata_name_filtered(name)) {
        return -ENOTSUP;
    }

    ret = resolve_generic_mode(path, mode, &resolved);

    if (ret < 0) {
        return ret;
    }

    if (resolved == AFP_METADATA_XATTR) {
        needed = sys_get(path, name, NULL, 0);

        if (needed >= 0) {
            *value = malloc(needed ? (size_t)needed : 1);

            if (!*value) {
                return -ENOMEM;
            }

            if (needed) {
                ssize_t got = sys_get(path, name, *value, needed);

                if (got != needed) {
                    int error = got < 0 ? errno : EIO;
                    free(*value);
                    *value = NULL;
                    return -error;
                }
            }

            *size = needed;
            return 0;
        }

        return -errno;
    }

    if (resolved == AFP_METADATA_NETATALK) {
        return netatalk_get(path, name, value, size);
    }

    return resolved == AFP_METADATA_MACOS || resolved == AFP_METADATA_NONE
           ? -ENOTSUP : -EINVAL;
}

int local_metadata_set(const char *path, enum afp_metadata_mode mode,
                       const char *name, const void *value, size_t size)
{
    enum afp_metadata_mode resolved;
    int ret = local_path_check(path);

    if (ret < 0) {
        return ret;
    }

    if (!metadata_name_valid(name) || (!value && size > 0)) {
        return -EINVAL;
    }

    if (metadata_name_filtered(name)) {
        return -ENOTSUP;
    }

    ret = resolve_generic_mode(path, mode, &resolved);

    if (ret < 0) {
        return ret;
    }

    if (resolved == AFP_METADATA_XATTR) {
        return sys_set(path, name, value, size) == 0 ? 0 : -errno;
    }

    if (resolved == AFP_METADATA_NETATALK) {
        return netatalk_set(path, name, value, size);
    }

    return resolved == AFP_METADATA_MACOS || resolved == AFP_METADATA_NONE
           ? -ENOTSUP : -EINVAL;
}

int local_metadata_remove(const char *path, enum afp_metadata_mode mode,
                          const char *name)
{
    enum afp_metadata_mode resolved;
    int ret = local_path_check(path);

    if (ret < 0) {
        return ret;
    }

    if (!metadata_name_valid(name)) {
        return -EINVAL;
    }

    ret = resolve_generic_mode(path, mode, &resolved);

    if (ret < 0) {
        return ret;
    }

    if (resolved == AFP_METADATA_XATTR) {
        if (sys_remove(path, name) == 0) {
            return 0;
        }

        if (!metadata_absent(errno)) {
            return -errno;
        }

        return 0;
    }

    if (resolved == AFP_METADATA_NETATALK) {
        return netatalk_remove(path, name);
    }

    return resolved == AFP_METADATA_MACOS || resolved == AFP_METADATA_NONE
           ? -ENOTSUP : -EINVAL;
}

int local_finderinfo_get(const char *path, enum afp_metadata_mode mode,
                         unsigned char value[32])
{
    ssize_t ret;
    int path_ret = local_path_check(path);

    if (path_ret < 0) {
        return path_ret;
    }

    if (apple_metadata_uses_sys(mode)) {
        ret = sys_get(path, AFP_XATTR_FINDERINFO, value, 32);

        if (ret == 32) {
            return 0;
        }

        return ret < 0 ? -errno : -EIO;
    }

    if (mode == AFP_METADATA_NETATALK) {
        return ad_finder_get(path, 1, value);
    }

    if (mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR
            || mode == AFP_METADATA_MACOS) {
        return ad_finder_get(path, 0, value);
    }

    return mode == AFP_METADATA_NONE ? -ENOTSUP : -EINVAL;
}

int local_finderinfo_set(const char *path, enum afp_metadata_mode mode,
                         const unsigned char value[32])
{
    int ret = local_path_check(path);

    if (ret < 0) {
        return ret;
    }

    if (apple_metadata_uses_sys(mode)) {
        return sys_set(path, AFP_XATTR_FINDERINFO, value, 32) == 0
               ? 0 : -errno;
    }

    if (mode == AFP_METADATA_NETATALK) {
        return ad_finder_set(path, 1, value);
    }

    if (mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR
            || mode == AFP_METADATA_MACOS) {
        return ad_finder_set(path, 0, value);
    }

    return mode == AFP_METADATA_NONE ? -ENOTSUP : -EINVAL;
}

int local_finderinfo_remove(const char *path, enum afp_metadata_mode mode)
{
    unsigned char empty[32] = {0};
    int ret = local_path_check(path);

    if (ret < 0) {
        return ret;
    }

    if (mode == AFP_METADATA_NETATALK) {
        return ad_finder_set(path, 1, empty);
    }

    if (apple_metadata_uses_sys(mode)) {
        if (sys_remove(path, AFP_XATTR_FINDERINFO) == 0) {
            return 0;
        }

        if (!metadata_absent(errno)) {
            return -errno;
        }

        return 0;
    }

    if (mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR
            || mode == AFP_METADATA_MACOS) {
        return ad_finder_set(path, 0, empty);
    }

    return mode == AFP_METADATA_NONE ? -ENOTSUP : -EINVAL;
}

off_t local_resourcefork_size(const char *path, enum afp_metadata_mode mode)
{
    ssize_t ret;
    int path_ret = local_path_check(path);

    if (path_ret < 0) {
        return path_ret;
    }

    if (apple_metadata_uses_sys(mode)) {
        ret = sys_get(path, AFP_XATTR_RESOURCEFORK, NULL, 0);

        if (ret >= 0) {
            return ret;
        }

        return -errno;
    }

    if (mode == AFP_METADATA_NETATALK) {
        return ad_resource_size(path, 1);
    }

    if (mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR
            || mode == AFP_METADATA_MACOS) {
        return ad_resource_size(path, 0);
    }

    return mode == AFP_METADATA_NONE ? -ENOTSUP : -EINVAL;
}

ssize_t local_resourcefork_read(const char *path, enum afp_metadata_mode mode,
                                void *buf, size_t size, off_t offset)
{
    if (offset < 0 || (!buf && size > 0)) {
        return -EINVAL;
    }

    if (size > SSIZE_MAX) {
        return -EOVERFLOW;
    }

    int path_ret = local_path_check(path);

    if (path_ret < 0) {
        return path_ret;
    }

    if (apple_metadata_uses_sys(mode)) {
        off_t total = local_resourcefork_size(path, AFP_METADATA_XATTR);

        if (total >= 0) {
            if ((uintmax_t)total > SIZE_MAX) {
                return -EOVERFLOW;
            }

            size_t total_size = (size_t)total;
            void *all = malloc(total_size ? total_size : 1);

            if (!all) {
                return -ENOMEM;
            }

            ssize_t got = sys_get(path, AFP_XATTR_RESOURCEFORK, all, total_size);

            if (got < 0) {
                int e = errno;
                free(all);
                return -e;
            }

            if ((size_t)got > total_size) {
                free(all);
                return -EIO;
            }

            if (offset >= got) {
                free(all);
                return 0;
            }

            size_t position = (size_t)offset;
            size_t available = (size_t)got - position;

            if (size > available) {
                size = available;
            }

            if (size > 0) {
                memcpy(buf, (char *)all + position, size);
            }

            free(all);
            return size;
        }

        return total;
    }

    if (mode == AFP_METADATA_NETATALK) {
        return ad_resource_read(path, 1, buf, size, offset);
    }

    if (mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR
            || mode == AFP_METADATA_MACOS) {
        return ad_resource_read(path, 0, buf, size, offset);
    }

    return mode == AFP_METADATA_NONE ? -ENOTSUP : -EINVAL;
}

int local_resourcefork_write(const char *path, enum afp_metadata_mode mode,
                             const void *buf, size_t size, off_t offset)
{
    if (offset < 0 || (!buf && size > 0)) {
        return -EINVAL;
    }

    if ((uintmax_t)offset > (uintmax_t)SSIZE_MAX
            || size > (size_t)SSIZE_MAX - (size_t)offset) {
        return -EFBIG;
    }

    int path_ret = local_path_check(path);

    if (path_ret < 0) {
        return path_ret;
    }

    if (apple_metadata_uses_sys(mode)) {
        off_t old_size = offset == 0 ? 0 : local_resourcefork_size(path,
                         AFP_METADATA_XATTR);
        size_t position = (size_t)offset;
        size_t total = position + size;
        unsigned char *all;

        if (old_size > 0) {
            if ((uintmax_t)old_size > SIZE_MAX) {
                return -EOVERFLOW;
            }

            if ((size_t)old_size > total) {
                total = (size_t)old_size;
            }
        }

        all = calloc(1, total ? total : 1);

        if (!all) {
            return -ENOMEM;
        }

        if (position > 0 && old_size > 0) {
            ssize_t got = sys_get(path, AFP_XATTR_RESOURCEFORK, all,
                                  (size_t)old_size);

            if (got < 0) {
                int e = errno;
                free(all);
                return -e;
            }

            if (got != old_size) {
                free(all);
                return -EIO;
            }
        }

        if (size) {
            memcpy(all + position, buf, size);
        }

        if (sys_set(path, AFP_XATTR_RESOURCEFORK, all, total) == 0) {
            free(all);
            return 0;
        }

        int error = errno;
        free(all);
        return -error;
    }

    if (mode == AFP_METADATA_NETATALK) {
        return ad_resource_write(path, 1, buf, size, offset);
    }

    if (mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR
            || mode == AFP_METADATA_MACOS) {
        return ad_resource_write(path, 0, buf, size, offset);
    }

    return mode == AFP_METADATA_NONE ? -ENOTSUP : -EINVAL;
}

int local_resourcefork_remove(const char *path, enum afp_metadata_mode mode)
{
    int ret = local_path_check(path);

    if (ret < 0) {
        return ret;
    }

    if (mode == AFP_METADATA_NETATALK) {
        return ad_resource_write(path, 1, NULL, 0, 0);
    }

    if (apple_metadata_uses_sys(mode)) {
        if (sys_remove(path, AFP_XATTR_RESOURCEFORK) == 0) {
            return 0;
        }

        if (!metadata_absent(errno)) {
            return -errno;
        }

        return 0;
    }

    if (mode == AFP_METADATA_AUTO || mode == AFP_METADATA_XATTR
            || mode == AFP_METADATA_MACOS) {
        return ad_resource_write(path, 0, NULL, 0, 0);
    }

    return mode == AFP_METADATA_NONE ? -ENOTSUP : -EINVAL;
}

static int transfer_error_absent(int ret)
{
    return ret == -ENOATTR || ret == -ENODATA;
}

static int transfer_error_unsupported(int ret)
{
    return ret == -ENOTSUP || ret == -EOPNOTSUPP || ret == -ENOSYS;
}

static void transfer_warning(unsigned int *warnings, unsigned int warning)
{
    if (warnings) {
        *warnings |= warning;
    }
}

static int transfer_name_special(const char *name)
{
    const char *stripped = afp_xattr_strip_user_prefix(name);
    return stripped && (strcmp(stripped, AFP_XATTR_FINDERINFO) == 0
                        || strcmp(stripped, AFP_XATTR_RESOURCEFORK) == 0);
}

static int transfer_list_next(const char *list, size_t size, size_t *position,
                              const char **name)
{
    size_t length;

    if (*position >= size) {
        return 0;
    }

    length = strnlen(list + *position, size - *position);

    if (length == size - *position) {
        return -EIO;
    }

    *name = list + *position;
    *position += length + 1U;
    return 1;
}

static int transfer_remote_list(volumeid_t *volume, const char *path,
                                char **list, size_t *size)
{
    int ret = afp_sl_listxattr(volume, path, NULL, 0);
    *list = NULL;
    *size = 0;

    if (ret <= 0) {
        return ret;
    }

    if ((size_t)ret > AFP_SL_XATTR_LIST_MAX) {
        return -E2BIG;
    }

    *list = malloc((size_t)ret);

    if (!*list) {
        return -ENOMEM;
    }

    int got = afp_sl_listxattr(volume, path, *list, (size_t)ret);

    if (got < 0 || got > ret) {
        free(*list);
        *list = NULL;
        return got < 0 ? got : -EIO;
    }

    *size = (size_t)got;
    return 0;
}

static int transfer_remote_get(volumeid_t *volume, const char *path,
                               const char *name, void **value, size_t *size)
{
    int ret = afp_sl_getxattr(volume, path, name, NULL, 0);
    *value = NULL;
    *size = 0;

    if (ret < 0) {
        return ret;
    }

    if (ret > AFP_SL_METADATA_CHUNK) {
        return -E2BIG;
    }

    *value = malloc(ret ? (size_t)ret : 1U);

    if (!*value) {
        return -ENOMEM;
    }

    if (ret > 0) {
        int got = afp_sl_getxattr(volume, path, name, *value, (size_t)ret);

        if (got < 0 || got > ret) {
            free(*value);
            *value = NULL;
            return got < 0 ? got : -EIO;
        }

        ret = got;
    }

    *size = (size_t)ret;
    return 0;
}

static int transfer_metadata_result(int ret, unsigned int *warnings)
{
    if (ret >= 0 || transfer_error_absent(ret)) {
        return 0;
    }

    if (transfer_error_unsupported(ret)) {
        transfer_warning(warnings, AFP_METADATA_WARNING_UNSUPPORTED);
        return 0;
    }

    return ret;
}

int afp_metadata_clear_local(const char *path, enum afp_metadata_mode mode,
                             unsigned int *warnings)
{
    char *list = NULL;
    size_t list_size = 0;
    size_t position = 0;
    const char *name;
    int next;
    int ret;

    if (warnings) {
        *warnings = AFP_METADATA_WARNING_NONE;
    }

    if (!path || !metadata_mode_valid(mode)) {
        return -EINVAL;
    }

    if (mode == AFP_METADATA_NONE) {
        return 0;
    }

    ret = transfer_metadata_result(local_finderinfo_remove(path, mode), warnings);

    if (ret < 0) {
        return ret;
    }

    ret = transfer_metadata_result(local_resourcefork_remove(path, mode), warnings);

    if (ret < 0) {
        return ret;
    }

    ret = local_metadata_list(path, mode, &list, &list_size);

    if (ret < 0) {
        return transfer_metadata_result(ret, warnings);
    }

    while ((next = transfer_list_next(list, list_size, &position, &name)) > 0) {
        if (transfer_name_special(name) || metadata_name_filtered(name)) {
            continue;
        }

        ret = transfer_metadata_result(local_metadata_remove(path, mode, name),
                                       warnings);

        if (ret < 0) {
            free(list);
            return ret;
        }
    }

    free(list);
    return next < 0 ? next : 0;
}

int afp_sl_metadata_clear(volumeid_t *volume, const char *path,
                          unsigned int *warnings)
{
    char *list = NULL;
    size_t list_size = 0;
    size_t position = 0;
    const char *name;
    int next;
    int ret;

    if (warnings) {
        *warnings = AFP_METADATA_WARNING_NONE;
    }

    if (!volume || !*volume || !path) {
        return -EINVAL;
    }

    ret = transfer_metadata_result(afp_sl_removefinderinfo(volume, path), warnings);

    if (ret < 0) {
        return ret;
    }

    ret = transfer_metadata_result(afp_sl_removeresourcefork(volume, path),
                                   warnings);

    if (ret < 0) {
        return ret;
    }

    ret = transfer_remote_list(volume, path, &list, &list_size);

    if (ret == -E2BIG) {
        transfer_warning(warnings, AFP_METADATA_WARNING_LIST_TOO_LARGE);
        return 0;
    }

    if (ret < 0) {
        return transfer_metadata_result(ret, warnings);
    }

    while ((next = transfer_list_next(list, list_size, &position, &name)) > 0) {
        if (transfer_name_special(name) || metadata_name_filtered(name)) {
            continue;
        }

        ret = transfer_metadata_result(afp_sl_removexattr(volume, path, name),
                                       warnings);

        if (ret < 0) {
            free(list);
            return ret;
        }
    }

    free(list);
    return next < 0 ? next : 0;
}

static int transfer_local_xattrs_to_remote(const char *local_path,
        enum afp_metadata_mode mode, volumeid_t *volume, const char *remote_path,
        unsigned int *warnings)
{
    char *list = NULL;
    size_t list_size = 0;
    size_t position = 0;
    const char *name;
    int next;
    int ret = local_metadata_list(local_path, mode, &list, &list_size);

    if (ret < 0) {
        return transfer_metadata_result(ret, warnings);
    }

    while ((next = transfer_list_next(list, list_size, &position, &name)) > 0) {
        void *value = NULL;
        size_t value_size = 0;

        if (transfer_name_special(name) || metadata_name_filtered(name)) {
            continue;
        }

        ret = local_metadata_get(local_path, mode, name, &value, &value_size);

        if (ret == 0 && value_size > AFP_SL_METADATA_CHUNK) {
            transfer_warning(warnings, AFP_METADATA_WARNING_VALUE_TOO_LARGE);
            free(value);
            continue;
        }

        if (ret == 0) {
            ret = afp_sl_setxattr(volume, remote_path, name, value, value_size, 0);
        }

        free(value);
        ret = transfer_metadata_result(ret, warnings);

        if (ret < 0) {
            free(list);
            return ret;
        }
    }

    free(list);
    return next < 0 ? next : 0;
}

int afp_sl_metadata_copy_local_to_remote(
    const char *local_path, enum afp_metadata_mode mode,
    volumeid_t *destination_volume, const char *destination_path,
    unsigned int *warnings)
{
    unsigned char finderinfo[32];
    unsigned char buffer[AFP_SL_METADATA_CHUNK];
    off_t resource_size;
    off_t offset = 0;
    int finder_ret;
    int ret;

    if (warnings) {
        *warnings = AFP_METADATA_WARNING_NONE;
    }

    if (!local_path || !destination_volume || !*destination_volume
            || !destination_path || !metadata_mode_valid(mode)) {
        return -EINVAL;
    }

    if (mode == AFP_METADATA_NONE) {
        return 0;
    }

    finder_ret = local_finderinfo_get(local_path, mode, finderinfo);

    if (finder_ret > 0) {
        return -EIO;
    }

    if (finder_ret < 0 && !transfer_error_absent(finder_ret)
            && !transfer_error_unsupported(finder_ret)) {
        return finder_ret;
    }

    ret = afp_sl_metadata_clear(destination_volume, destination_path, warnings);

    if (ret < 0) {
        return ret;
    }

    if (finder_ret == 0) {
        ret = afp_sl_setfinderinfo(destination_volume, destination_path,
                                   finderinfo, sizeof(finderinfo));
    } else {
        ret = finder_ret;
    }

    ret = transfer_metadata_result(ret, warnings);

    if (ret < 0) {
        return ret;
    }

    resource_size = local_resourcefork_size(local_path, mode);

    while (resource_size > 0 && offset < resource_size) {
        size_t chunk = (size_t)(resource_size - offset);

        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }

        ssize_t amount = local_resourcefork_read(local_path, mode, buffer,
                         chunk, offset);

        if (amount <= 0 || (size_t)amount > chunk) {
            return amount < 0 ? (int)amount : -EIO;
        }

        ret = afp_sl_setresourcefork(destination_volume, destination_path,
                                     buffer, (size_t)amount,
                                     (unsigned long long)offset);

        if (transfer_error_unsupported(ret)) {
            transfer_warning(warnings, AFP_METADATA_WARNING_UNSUPPORTED);
            break;
        }

        if (ret < 0) {
            return ret;
        }

        offset += amount;
    }

    if (resource_size < 0) {
        ret = transfer_metadata_result((int)resource_size, warnings);

        if (ret < 0) {
            return ret;
        }
    }

    return transfer_local_xattrs_to_remote(local_path, mode,
                                           destination_volume,
                                           destination_path, warnings);
}

static int transfer_remote_xattrs_to_local(volumeid_t *volume,
        const char *remote_path, const char *local_path,
        enum afp_metadata_mode mode, unsigned int *warnings)
{
    char *list = NULL;
    size_t list_size = 0;
    size_t position = 0;
    const char *name;
    int next;
    int ret = transfer_remote_list(volume, remote_path, &list, &list_size);

    if (ret == -E2BIG) {
        transfer_warning(warnings, AFP_METADATA_WARNING_LIST_TOO_LARGE);
        return 0;
    }

    if (ret < 0) {
        return transfer_metadata_result(ret, warnings);
    }

    while ((next = transfer_list_next(list, list_size, &position, &name)) > 0) {
        void *value = NULL;
        size_t value_size = 0;

        if (transfer_name_special(name) || metadata_name_filtered(name)) {
            continue;
        }

        ret = transfer_remote_get(volume, remote_path, name, &value, &value_size);

        if (ret == -E2BIG) {
            transfer_warning(warnings, AFP_METADATA_WARNING_VALUE_TOO_LARGE);
            continue;
        }

        if (ret == 0) {
            ret = local_metadata_set(local_path, mode, name, value, value_size);
        }

        free(value);
        ret = transfer_metadata_result(ret, warnings);

        if (ret < 0) {
            free(list);
            return ret;
        }
    }

    free(list);
    return next < 0 ? next : 0;
}

int afp_sl_metadata_copy_remote_to_local(
    volumeid_t *source_volume, const char *source_path,
    const char *local_path, enum afp_metadata_mode mode,
    unsigned int *warnings)
{
    unsigned char finderinfo[32];
    unsigned char buffer[AFP_SL_METADATA_CHUNK];
    unsigned long long offset = 0;
    int finder_ret;
    int ret;

    if (warnings) {
        *warnings = AFP_METADATA_WARNING_NONE;
    }

    if (!source_volume || !*source_volume || !source_path || !local_path
            || !metadata_mode_valid(mode)) {
        return -EINVAL;
    }

    if (mode == AFP_METADATA_NONE) {
        return 0;
    }

    finder_ret = afp_sl_getfinderinfo(source_volume, source_path, finderinfo,
                                      sizeof(finderinfo));

    if (finder_ret >= 0 && finder_ret != (int)sizeof(finderinfo)) {
        return -EIO;
    }

    if (finder_ret < 0 && !transfer_error_absent(finder_ret)
            && !transfer_error_unsupported(finder_ret)) {
        return finder_ret;
    }

    ret = afp_metadata_clear_local(local_path, mode, warnings);

    if (ret < 0) {
        return ret;
    }

    if (finder_ret == (int)sizeof(finderinfo)) {
        ret = local_finderinfo_set(local_path, mode, finderinfo);
    } else {
        ret = finder_ret;
    }

    ret = transfer_metadata_result(ret, warnings);

    if (ret < 0) {
        return ret;
    }

    ret = afp_sl_getresourcefork(source_volume, source_path, NULL, 0, 0);

    if (ret > 0) {
        unsigned long long total = (unsigned int)ret;

        while (offset < total) {
            size_t chunk = (size_t)(total - offset);

            if (chunk > sizeof(buffer)) {
                chunk = sizeof(buffer);
            }

            ret = afp_sl_getresourcefork(source_volume, source_path, buffer,
                                         chunk, offset);

            if (ret <= 0 || (size_t)ret > chunk) {
                return ret < 0 ? ret : -EIO;
            }

            int write_ret = local_resourcefork_write(local_path, mode, buffer,
                            (size_t)ret, (off_t)offset);

            if (transfer_error_unsupported(write_ret)) {
                transfer_warning(warnings, AFP_METADATA_WARNING_UNSUPPORTED);
                break;
            }

            if (write_ret < 0) {
                return write_ret;
            }

            offset += (unsigned int)ret;
        }
    } else if (ret < 0) {
        ret = transfer_metadata_result(ret, warnings);

        if (ret < 0) {
            return ret;
        }
    }

    return transfer_remote_xattrs_to_local(source_volume, source_path,
                                           local_path, mode, warnings);
}

static int transfer_remote_xattrs_to_remote(volumeid_t *source_volume,
        const char *source_path, volumeid_t *destination_volume,
        const char *destination_path, unsigned int *warnings)
{
    char *list = NULL;
    size_t list_size = 0;
    size_t position = 0;
    const char *name;
    int next;
    int ret = transfer_remote_list(source_volume, source_path, &list, &list_size);

    if (ret == -E2BIG) {
        transfer_warning(warnings, AFP_METADATA_WARNING_LIST_TOO_LARGE);
        return 0;
    }

    if (ret < 0) {
        return transfer_metadata_result(ret, warnings);
    }

    while ((next = transfer_list_next(list, list_size, &position, &name)) > 0) {
        void *value = NULL;
        size_t value_size = 0;

        if (transfer_name_special(name) || metadata_name_filtered(name)) {
            continue;
        }

        ret = transfer_remote_get(source_volume, source_path, name, &value,
                                  &value_size);

        if (ret == -E2BIG) {
            transfer_warning(warnings, AFP_METADATA_WARNING_VALUE_TOO_LARGE);
            continue;
        }

        if (ret == 0) {
            ret = afp_sl_setxattr(destination_volume, destination_path, name,
                                  value, value_size, 0);
        }

        free(value);
        ret = transfer_metadata_result(ret, warnings);

        if (ret < 0) {
            free(list);
            return ret;
        }
    }

    free(list);
    return next < 0 ? next : 0;
}

int afp_sl_metadata_copy_remote_to_remote(
    volumeid_t *source_volume, const char *source_path,
    volumeid_t *destination_volume, const char *destination_path,
    unsigned int *warnings)
{
    unsigned char finderinfo[32];
    unsigned char buffer[AFP_SL_METADATA_CHUNK];
    unsigned long long offset = 0;
    int finder_ret;
    int ret;

    if (warnings) {
        *warnings = AFP_METADATA_WARNING_NONE;
    }

    if (!source_volume || !*source_volume || !source_path
            || !destination_volume || !*destination_volume
            || !destination_path) {
        return -EINVAL;
    }

    finder_ret = afp_sl_getfinderinfo(source_volume, source_path, finderinfo,
                                      sizeof(finderinfo));

    if (finder_ret >= 0 && finder_ret != (int)sizeof(finderinfo)) {
        return -EIO;
    }

    if (finder_ret < 0 && !transfer_error_absent(finder_ret)
            && !transfer_error_unsupported(finder_ret)) {
        return finder_ret;
    }

    ret = afp_sl_metadata_clear(destination_volume, destination_path, warnings);

    if (ret < 0) {
        return ret;
    }

    if (finder_ret == (int)sizeof(finderinfo)) {
        ret = afp_sl_setfinderinfo(destination_volume, destination_path,
                                   finderinfo, sizeof(finderinfo));
    } else {
        ret = finder_ret;
    }

    ret = transfer_metadata_result(ret, warnings);

    if (ret < 0) {
        return ret;
    }

    ret = afp_sl_getresourcefork(source_volume, source_path, NULL, 0, 0);

    if (ret > 0) {
        unsigned long long total = (unsigned int)ret;

        while (offset < total) {
            size_t chunk = (size_t)(total - offset);

            if (chunk > sizeof(buffer)) {
                chunk = sizeof(buffer);
            }

            ret = afp_sl_getresourcefork(source_volume, source_path, buffer,
                                         chunk, offset);

            if (ret <= 0 || (size_t)ret > chunk) {
                return ret < 0 ? ret : -EIO;
            }

            int write_ret = afp_sl_setresourcefork(destination_volume,
                                                   destination_path, buffer, (size_t)ret, offset);

            if (transfer_error_unsupported(write_ret)) {
                transfer_warning(warnings, AFP_METADATA_WARNING_UNSUPPORTED);
                break;
            }

            if (write_ret < 0) {
                return write_ret;
            }

            offset += (unsigned int)ret;
        }
    } else if (ret < 0) {
        ret = transfer_metadata_result(ret, warnings);

        if (ret < 0) {
            return ret;
        }
    }

    return transfer_remote_xattrs_to_remote(source_volume, source_path,
                                            destination_volume, destination_path, warnings);
}
