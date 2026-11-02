/*
    test_appledouble_paths.c: paths through the .AppleDouble directory

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/afp_internal.h"
#include "lib/resource.h"
#include "tap.h"

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/*!
 * @brief Set up a volume that shows AppleDouble files, on a server with no
 *        connection
 *
 * A path that reaches the server fails, so only a translation answers.
 *
 * @param[out] volume  receives the volume
 * @param[out] server  receives its server
 */
static void volume_open(struct afp_volume *volume, struct afp_server *server)
{
    memset(server, 0, sizeof(*server));
    memset(volume, 0, sizeof(*volume));
    server->fd = -1;
    server->connect_state = SERVER_STATE_DISCONNECTED;
    volume->server = server;
    volume->extra_flags = VOLUME_EXTRA_FLAGS_SHOW_APPLEDOUBLE;
}

/*!
 * @brief A name ending in .AppleDouble is a regular name
 *
 * Nothing is translated, even for a name that ends at the last byte of a page
 * whose next page cannot be read, where a look past the terminator would show.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_name_ending_in_appledouble_is_regular(void)
{
    struct afp_server server;
    struct afp_volume volume;
    struct stat st;
    static const char name[] = "/x.AppleDouble";
    long page = sysconf(_SC_PAGESIZE);
    char *map;
    char *path;
    int ret = 0;
    volume_open(&volume, &server);

    if (page <= 0) {
        return 1;
    }

    map = mmap(NULL, (size_t) page * 2, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (map == MAP_FAILED) {
        return 2;
    }

    if (mprotect(map + page, (size_t) page, PROT_NONE) != 0) {
        ret = 3;
    } else {
        path = map + page - sizeof(name);
        memcpy(path, name, sizeof(name));

        if (appledouble_getattr(&volume, path, &st) != 0) {
            ret = 4;
        } else if (appledouble_getattr(&volume, "/foo/x.AppleDouble", &st)
                   != 0) {
            ret = 5;
        }
    }

    munmap(map, (size_t) page * 2);
    return ret;
}

/*!
 * @brief The .AppleDouble directory itself translates
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_appledouble_directory_translates(void)
{
    struct afp_server server;
    struct afp_volume volume;
    struct stat st;
    volume_open(&volume, &server);
    memset(&st, 0, sizeof(st));

    if (appledouble_getattr(&volume, "/foo/.AppleDouble", &st) != 1) {
        return 1;
    }

    return S_ISDIR(st.st_mode) ? 0 : 2;
}

/*!
 * @brief A name that ends or starts with .AppleDouble is a regular name
 *
 * Inside a path, its mode, owner and time change through the setter.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_names_inside_a_path_are_regular(void)
{
    struct afp_server server;
    struct afp_volume volume;
    volume_open(&volume, &server);

    if (appledouble_setattr(&volume, "/x.AppleDouble/y") != 0) {
        return 1;
    }

    return appledouble_setattr(&volume, "/.AppleDoubleX/y") == 0 ? 0 : 2;
}

/*!
 * @brief A whole .AppleDouble component makes a pseudo-file
 *
 * After such a name too, the pseudo-file refuses a change of mode, owner
 * or time.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_appledouble_component_makes_a_pseudo_file(void)
{
    struct afp_server server;
    struct afp_volume volume;
    volume_open(&volume, &server);

    if (appledouble_setattr(&volume, "/foo/.AppleDouble/bar") != -EPERM) {
        return 1;
    }

    return appledouble_setattr(&volume, "/a.AppleDouble/.AppleDouble/x")
           == -EPERM ? 0 : 2;
}

/*!
 * @brief A link at a path of the .AppleDouble view is refused
 *
 * A link's target is any text, so a target there is no reason to refuse.
 *
 * @returns 0, or the number of the step that failed
 */
static int utest_link_in_the_view_is_refused(void)
{
    struct afp_server server;
    struct afp_volume volume;
    volume_open(&volume, &server);

    if (appledouble_symlink(&volume, "target", "/dir/.AppleDouble/link")
            != -EPERM) {
        return 1;
    }

    return appledouble_symlink(&volume, "/dir/.AppleDouble/x", "/dir/link")
           == 0 ? 0 : 2;
}

/*!
 * @brief Run the AppleDouble path translation checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    TEST_int(utest_name_ending_in_appledouble_is_regular(), 0,
             "a name ending in .AppleDouble is a regular name");
    TEST_int(utest_appledouble_directory_translates(), 0,
             "the .AppleDouble directory itself translates");
    TEST_int(utest_names_inside_a_path_are_regular(), 0,
             "names inside a path that end or start with it are regular");
    TEST_int(utest_appledouble_component_makes_a_pseudo_file(), 0,
             "a whole .AppleDouble component makes a pseudo-file");
    TEST_int(utest_link_in_the_view_is_refused(), 0,
             "a link at a path of the .AppleDouble view is refused");
    return test_tap_finish();
}
