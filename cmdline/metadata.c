/*
 * Local metadata storage for afpcmd.
 *
 * Portions of the AppleDouble and extended attribute implementation are
 * adapted from Netatalk's libatalk/adouble and libatalk/vfs/ea_ad modules.
 *
 * Copyright (C) 1990,1991 Regents of The University of Michigan.
 * Copyright (C) 1999 Adrian Sun (asun@u.washington.edu)
 * Copyright (C) 2009-2010 Frank Lahm <franklahm@gmail.com>
 * Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "metadata.h"

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

#ifndef ENOATTR
#ifdef ENODATA
#define ENOATTR ENODATA
#else
#define ENOATTR ENOENT
#endif
#endif

#ifndef ENODATA
#define ENODATA ENOATTR
#endif

#define FI_NAME "com.apple.FinderInfo"
#define RF_NAME "com.apple.ResourceFork"
#define NETATALK_META "org.netatalk.Metadata"
#define COPY_SOURCE "com.apple.finder.copy.source"
#define COPY_CHECKPOINT "com.apple.finder.copy.checkpoint#"
#define RESUMABLE_COPY "com.apple.metadata:kMDItemResumableCopy"

#define METADATA_NAME_MAX 255U
#define SYS_NAME_MAX ((sizeof("user.") - 1U) + METADATA_NAME_MAX)
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
    return error == ENOATTR || error == ENODATA || error == ENOENT;
}

static int metadata_unsupported(int error)
{
    return error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS;
}

int metadata_mode_parse(const char *name, enum metadata_mode *mode)
{
    if (!name || !mode) {
        return -1;
    }

    if (strcmp(name, "auto") == 0) {
        *mode = METADATA_AUTO;
    } else if (strcmp(name, "sys") == 0) {
        *mode = METADATA_SYS;
    } else if (strcmp(name, "macos") == 0) {
        *mode = METADATA_MACOS;
    } else if (strcmp(name, "netatalk") == 0) {
        *mode = METADATA_NETATALK;
    } else if (strcmp(name, "none") == 0) {
        *mode = METADATA_NONE;
    } else {
        return -1;
    }

    return 0;
}

const char *metadata_mode_name(enum metadata_mode mode)
{
    switch (mode) {
    case METADATA_AUTO:
        return "auto";

    case METADATA_SYS:
        return "sys";

    case METADATA_MACOS:
        return "macos";

    case METADATA_NETATALK:
        return "netatalk";

    case METADATA_NONE:
        return "none";
    }

    return "unknown";
}

int metadata_name_filtered(const char *name)
{
    if (!name) {
        return 1;
    }

    if (strncmp(name, "user.", 5) == 0) {
        name += 5;
    }

    return strncmp(name, NETATALK_META, sizeof(NETATALK_META) - 1U) == 0
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

    length = strnlen(name, METADATA_NAME_MAX + 1U);
    return length > 0 && length <= METADATA_NAME_MAX
           && memchr(name, '/', length) == NULL;
}

static const char *sys_name(const char *name, char mapped[SYS_NAME_SIZE])
{
#if defined(__APPLE__)
    (void)mapped;
    return name;
#else

    if (strncmp(name, "user.", 5) == 0) {
        return name;
    }

    snprintf(mapped, SYS_NAME_SIZE, "user.%s", name);
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
    const char *bare = strncmp(actual, "user.", 5) == 0 ? actual + 5 : actual;
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
    const char *bare = strncmp(actual, "user.", 5) == 0 ? actual + 5 : actual;
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
    const char *bare = strncmp(actual, "user.", 5) == 0 ? actual + 5 : actual;
    return extattr_delete_file(path, EXTATTR_NAMESPACE_USER, bare);
#else
    (void)path;
    (void)actual;
    errno = ENOTSUP;
    return -1;
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

        snprintf(name, sizeof(name), "user.%.*s", length, raw + pos);

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

static int sidecar_exists(const char *path, int netatalk)
{
    char sidecar[PATH_MAX];
    struct stat st;
    int ret = sidecar_path(path, netatalk, sidecar, sizeof(sidecar));

    if (ret < 0) {
        return ret;
    }

    if (lstat(sidecar, &st) == 0) {
        return 1;
    }

    return errno == ENOENT ? 0 : -errno;
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
        return -errno;
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
        return -errno;
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
        size_t name_len = strnlen(items[i].name, METADATA_NAME_MAX + 1U);

        if (name_len > METADATA_NAME_MAX) {
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
        size_t name_len = strnlen(items[i].name, METADATA_NAME_MAX + 1U);
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

int local_metadata_list(const char *path, enum metadata_mode mode, char **list,
                        size_t *size)
{
    char *extra = NULL;
    size_t extra_size = 0;
    int ret;
    *list = NULL;
    *size = 0;

    if (mode == METADATA_NONE || mode == METADATA_MACOS) {
        return 0;
    }

    if (mode == METADATA_SYS) {
        return sys_list(path, list, size);
    }

    if (mode == METADATA_NETATALK) {
        return netatalk_list(path, list, size);
    }

    ret = sys_list(path, list, size);

    if (ret < 0 && !metadata_unsupported(-ret)) {
        return ret;
    }

    if (ret < 0) {
        *list = NULL;
        *size = 0;
    }

    ret = netatalk_list(path, &extra, &extra_size);

    if (ret == 0) {
        for (size_t pos = 0; pos < extra_size;) {
            size_t name_len = strnlen(extra + pos, extra_size - pos);

            if (name_len == extra_size - pos) {
                ret = -EIO;
                break;
            }

            ret = append_name(list, size, extra + pos);

            if (ret < 0) {
                break;
            }

            pos += name_len + 1U;
        }
    }

    free(extra);

    if (ret < 0 && !metadata_absent(-ret)) {
        free(*list);
        *list = NULL;
        *size = 0;
        return ret;
    }

    return 0;
}

int local_metadata_get(const char *path, enum metadata_mode mode,
                       const char *name, void **value, size_t *size)
{
    ssize_t needed;

    if (!metadata_name_valid(name)) {
        return -EINVAL;
    }

    if (mode == METADATA_NONE || mode == METADATA_MACOS
            || metadata_name_filtered(name)) {
        return -ENOTSUP;
    }

    if (mode != METADATA_NETATALK) {
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

        if (mode == METADATA_SYS || (!metadata_absent(errno)
                                     && !metadata_unsupported(errno))) {
            return -errno;
        }
    }

    return netatalk_get(path, name, value, size);
}

int local_metadata_set(const char *path, enum metadata_mode mode,
                       const char *name, const void *value, size_t size)
{
    if (!metadata_name_valid(name) || (!value && size > 0)) {
        return -EINVAL;
    }

    if (mode == METADATA_NONE || mode == METADATA_MACOS
            || metadata_name_filtered(name)) {
        return -ENOTSUP;
    }

    if (mode != METADATA_NETATALK) {
        if (sys_set(path, name, value, size) == 0) {
            return 0;
        }

        if (mode == METADATA_SYS || !metadata_unsupported(errno)) {
            return -errno;
        }
    }

    return netatalk_set(path, name, value, size);
}

int local_metadata_remove(const char *path, enum metadata_mode mode,
                          const char *name)
{
    if (!metadata_name_valid(name)) {
        return -EINVAL;
    }

    if (mode == METADATA_NONE || mode == METADATA_MACOS) {
        return -ENOTSUP;
    }

    if (mode == METADATA_AUTO) {
        int sys_ret = sys_remove(path, name) == 0 ? 0 : -errno;
        int ad_ret = netatalk_remove(path, name);

        if (sys_ret < 0 && !metadata_absent(-sys_ret)
                && !metadata_unsupported(-sys_ret)) {
            return sys_ret;
        }

        if (ad_ret < 0 && !metadata_absent(-ad_ret)) {
            return ad_ret;
        }

        return (sys_ret == 0 || ad_ret == 0) ? 0 : -ENOATTR;
    }

    if (mode != METADATA_NETATALK) {
        if (sys_remove(path, name) == 0) {
            return 0;
        }

        if (mode == METADATA_SYS || (!metadata_absent(errno)
                                     && !metadata_unsupported(errno))) {
            return -errno;
        }
    }

    return netatalk_remove(path, name);
}

int local_finderinfo_get(const char *path, enum metadata_mode mode,
                         unsigned char value[32])
{
    ssize_t ret;

    if (mode == METADATA_NONE) {
        return -ENOTSUP;
    }

    if (mode == METADATA_SYS || mode == METADATA_AUTO) {
        ret = sys_get(path, FI_NAME, value, 32);

        if (ret == 32) {
            return 0;
        }

        if (mode == METADATA_SYS || (ret < 0 && !metadata_absent(errno)
                                     && !metadata_unsupported(errno))) {
            return ret < 0 ? -errno : -EIO;
        }
    }

    if (mode == METADATA_MACOS || mode == METADATA_AUTO) {
        int r = ad_finder_get(path, 0, value);

        if (r == 0 || mode == METADATA_MACOS) {
            return r;
        }
    }

    return ad_finder_get(path, 1, value);
}

int local_finderinfo_set(const char *path, enum metadata_mode mode,
                         const unsigned char value[32])
{
    if (mode == METADATA_NONE) {
        return -ENOTSUP;
    }

    if (mode == METADATA_SYS || mode == METADATA_AUTO) {
        if (sys_set(path, FI_NAME, value, 32) == 0) {
            return 0;
        }

        if (mode == METADATA_SYS || !metadata_unsupported(errno)) {
            return -errno;
        }
    }

    return ad_finder_set(path, mode == METADATA_NETATALK, value);
}

int local_finderinfo_remove(const char *path, enum metadata_mode mode)
{
    unsigned char empty[32] = {0};

    if (mode == METADATA_NONE) {
        return -ENOTSUP;
    }

    if (mode == METADATA_AUTO) {
        int sys_ret = sys_remove(path, FI_NAME) == 0 ? 0 : -errno;
        int macos_exists = sidecar_exists(path, 0);
        int netatalk_exists = sidecar_exists(path, 1);
        int macos_ret;
        int netatalk_ret;

        if (macos_exists > 0) {
            macos_ret = ad_finder_set(path, 0, empty);
        } else if (macos_exists < 0) {
            macos_ret = macos_exists;
        } else {
            macos_ret = -ENOATTR;
        }

        if (netatalk_exists > 0) {
            netatalk_ret = ad_finder_set(path, 1, empty);
        } else if (netatalk_exists < 0) {
            netatalk_ret = netatalk_exists;
        } else {
            netatalk_ret = -ENOATTR;
        }

        if (sys_ret < 0 && !metadata_absent(-sys_ret)
                && !metadata_unsupported(-sys_ret)) {
            return sys_ret;
        }

        if (macos_ret < 0 && !metadata_absent(-macos_ret)) {
            return macos_ret;
        }

        if (netatalk_ret < 0 && !metadata_absent(-netatalk_ret)) {
            return netatalk_ret;
        }

        return 0;
    }

    if (mode == METADATA_SYS || mode == METADATA_AUTO) {
        if (sys_remove(path, FI_NAME) == 0) {
            return 0;
        }

        if (mode == METADATA_SYS || (!metadata_absent(errno)
                                     && !metadata_unsupported(errno))) {
            return -errno;
        }
    }

    return ad_finder_set(path, mode == METADATA_NETATALK, empty);
}

off_t local_resourcefork_size(const char *path, enum metadata_mode mode)
{
    ssize_t ret;

    if (mode == METADATA_NONE) {
        return -ENOTSUP;
    }

    if (mode == METADATA_SYS || mode == METADATA_AUTO) {
        ret = sys_get(path, RF_NAME, NULL, 0);

        if (ret >= 0) {
            return ret;
        }

        if (mode == METADATA_SYS || (!metadata_absent(errno)
                                     && !metadata_unsupported(errno))) {
            return -errno;
        }
    }

    if (mode == METADATA_MACOS || mode == METADATA_AUTO) {
        off_t r = ad_resource_size(path, 0);

        if (r >= 0 || mode == METADATA_MACOS) {
            return r;
        }
    }

    return ad_resource_size(path, 1);
}

ssize_t local_resourcefork_read(const char *path, enum metadata_mode mode,
                                void *buf, size_t size, off_t offset)
{
    if (mode == METADATA_SYS || mode == METADATA_AUTO) {
        off_t total = local_resourcefork_size(path, METADATA_SYS);

        if (total >= 0) {
            void *all = malloc(total ? (size_t)total : 1);

            if (!all) {
                return -ENOMEM;
            }

            ssize_t got = sys_get(path, RF_NAME, all, total);

            if (got < 0) {
                int e = errno;
                free(all);
                return -e;
            }

            if (offset >= got) {
                free(all);
                return 0;
            }

            if (size > (size_t)(got - offset)) {
                size = got - offset;
            }

            memcpy(buf, (char *)all + offset, size);
            free(all);
            return size;
        }

        if (mode == METADATA_SYS) {
            return total;
        }
    }

    if (mode == METADATA_MACOS || mode == METADATA_AUTO) {
        ssize_t r = ad_resource_read(path, 0, buf, size, offset);

        if (r >= 0 || mode == METADATA_MACOS) {
            return r;
        }
    }

    return ad_resource_read(path, 1, buf, size, offset);
}

int local_resourcefork_write(const char *path, enum metadata_mode mode,
                             const void *buf, size_t size, off_t offset)
{
    if (mode == METADATA_NONE) {
        return -ENOTSUP;
    }

    if (mode == METADATA_SYS || mode == METADATA_AUTO) {
        off_t old_size = offset == 0 ? 0 : local_resourcefork_size(path, METADATA_SYS);
        size_t total = offset + size;
        unsigned char *all;

        if (old_size > (off_t)total) {
            total = old_size;
        }

        all = calloc(1, total ? total : 1);

        if (!all) {
            return -ENOMEM;
        }

        if (offset > 0 && old_size > 0 && sys_get(path, RF_NAME, all, old_size) < 0) {
            int e = errno;
            free(all);
            return -e;
        }

        if (size) {
            memcpy(all + offset, buf, size);
        }

        if (sys_set(path, RF_NAME, all, total) == 0) {
            free(all);
            return 0;
        }

        int error = errno;
        free(all);

        if (mode == METADATA_SYS || !metadata_unsupported(error)) {
            return -error;
        }
    }

    return ad_resource_write(path, mode == METADATA_NETATALK, buf, size, offset);
}

int local_resourcefork_remove(const char *path, enum metadata_mode mode)
{
    if (mode == METADATA_NONE) {
        return -ENOTSUP;
    }

    if (mode == METADATA_AUTO) {
        int sys_ret = sys_remove(path, RF_NAME) == 0 ? 0 : -errno;
        int macos_exists = sidecar_exists(path, 0);
        int netatalk_exists = sidecar_exists(path, 1);
        int macos_ret;
        int netatalk_ret;

        if (macos_exists > 0) {
            macos_ret = ad_resource_write(path, 0, NULL, 0, 0);
        } else if (macos_exists < 0) {
            macos_ret = macos_exists;
        } else {
            macos_ret = -ENOATTR;
        }

        if (netatalk_exists > 0) {
            netatalk_ret = ad_resource_write(path, 1, NULL, 0, 0);
        } else if (netatalk_exists < 0) {
            netatalk_ret = netatalk_exists;
        } else {
            netatalk_ret = -ENOATTR;
        }

        if (sys_ret < 0 && !metadata_absent(-sys_ret)
                && !metadata_unsupported(-sys_ret)) {
            return sys_ret;
        }

        if (macos_ret < 0 && !metadata_absent(-macos_ret)) {
            return macos_ret;
        }

        if (netatalk_ret < 0 && !metadata_absent(-netatalk_ret)) {
            return netatalk_ret;
        }

        return 0;
    }

    if (mode == METADATA_SYS || mode == METADATA_AUTO) {
        if (sys_remove(path, RF_NAME) == 0) {
            return 0;
        }

        if (mode == METADATA_SYS || (!metadata_absent(errno)
                                     && !metadata_unsupported(errno))) {
            return -errno;
        }
    }

    return ad_resource_write(path, mode == METADATA_NETATALK, NULL, 0, 0);
}
