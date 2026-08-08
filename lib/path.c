/*
 *  path.c
 *
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "netatalk-client/path.h"

int afpc_path_validate(const char *data, size_t max_len, size_t *len)
{
    size_t measured;

    if (!data) {
        return -EINVAL;
    }

    if (max_len == AFPC_PATH_UNBOUNDED) {
        measured = strlen(data);
    } else {
        measured = strnlen(data, max_len + 1U);

        if (measured > max_len) {
            return -ENAMETOOLONG;
        }
    }

    if (len) {
        *len = measured;
    }

    return 0;
}

void afpc_path_clear(struct afpc_path *path)
{
    if (!path) {
        return;
    }

    free(path->data);
    path->data = NULL;
    path->len = 0;
}

int afpc_path_set(struct afpc_path *path, const char *data, size_t max_len)
{
    char *copy;
    size_t len;
    int ret;

    if (!path) {
        return -EINVAL;
    }

    if (!data) {
        afpc_path_clear(path);
        return 0;
    }

    ret = afpc_path_validate(data, max_len, &len);

    if (ret) {
        return ret;
    }

    copy = malloc(len + 1U);

    if (!copy) {
        return -ENOMEM;
    }

    memcpy(copy, data, len + 1U);
    afpc_path_clear(path);
    path->data = copy;
    path->len = len;
    return 0;
}

int afpc_path_copy(struct afpc_path *dest, const struct afpc_path *src,
                   size_t max_len)
{
    if (!dest || !src) {
        return -EINVAL;
    }

    return afpc_path_set(dest, src->data, max_len);
}

int afpc_path_join(struct afpc_path *dest, const char *parent,
                   const char *name, size_t max_len)
{
    char *joined;
    size_t parent_len;
    size_t name_len;
    size_t separator_len;
    size_t total_len;
    int ret;

    if (!dest || !parent || !name) {
        return -EINVAL;
    }

    ret = afpc_path_validate(parent, max_len, &parent_len);

    if (ret) {
        return ret;
    }

    ret = afpc_path_validate(name, max_len, &name_len);

    if (ret) {
        return ret;
    }

    separator_len = parent_len > 0 && parent[parent_len - 1U] != '/'
                    && name_len > 0;

    if (parent_len > SIZE_MAX - separator_len) {
        return -ENAMETOOLONG;
    }

    total_len = parent_len + separator_len;

    if (total_len > SIZE_MAX - name_len) {
        return -ENAMETOOLONG;
    }

    total_len += name_len;

    if (total_len == SIZE_MAX) {
        return -ENAMETOOLONG;
    }

    if (max_len != AFPC_PATH_UNBOUNDED && total_len > max_len) {
        return -ENAMETOOLONG;
    }

    joined = malloc(total_len + 1U);

    if (!joined) {
        return -ENOMEM;
    }

    memcpy(joined, parent, parent_len);

    if (separator_len) {
        joined[parent_len] = '/';
    }

    memcpy(joined + parent_len + separator_len, name, name_len);
    joined[total_len] = '\0';
    afpc_path_clear(dest);
    dest->data = joined;
    dest->len = total_len;
    return 0;
}
