/*
    test_listing.c: directory listings, their records and the stat form

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "lib/afp_internal.h"
#include "lib/afp_protocol.h"
#include "lib/afp_replies.h"
#include "lib/dsi_protocol.h"
#include "lib/mapping.h"
#include "lib/resource.h"
#include "lib/utils.h"
#include "tap.h"

/*! @brief An FPEnumerateExt2 reply: a file with a decomposed name, a folder */
struct ext2_page {
    struct dsi_header header __attribute__((__packed__));
    uint16_t filebitmap;
    uint16_t dirbitmap;
    uint16_t reqcount;
    uint16_t file_size;
    uint8_t file_isdir;
    uint8_t file_pad;
    uint32_t file_parent;
    uint32_t file_mod;
    uint32_t file_node;
    uint64_t file_len;
    uint16_t file_name_offset;
    uint32_t file_name_pad;
    uint32_t file_name_hint;
    uint16_t file_name_len;
    char file_name[3];
    uint8_t file_align;
    uint16_t dir_size;
    uint8_t dir_isdir;
    uint8_t dir_pad;
    uint32_t dir_parent;
    uint32_t dir_mod;
    uint32_t dir_node;
    uint16_t dir_offspring;
    uint16_t dir_name_offset;
    uint32_t dir_name_pad;
    uint32_t dir_name_hint;
    uint16_t dir_name_len;
    char dir_name[3];
    uint8_t dir_align;
} __attribute__((__packed__));

/*! @brief An FPEnumerateExt2 reply whose one UTF-8 name no name buffer holds */
struct long_page {
    struct dsi_header header __attribute__((__packed__));
    uint16_t filebitmap;
    uint16_t dirbitmap;
    uint16_t reqcount;
    uint16_t file_size;
    uint8_t file_isdir;
    uint8_t file_pad;
    uint16_t file_name_offset;
    uint32_t file_name_pad;
    uint32_t file_name_hint;
    uint16_t file_name_len;
    char file_name[1021];
    uint8_t file_align;
} __attribute__((__packed__));

/*! @brief An FPEnumerate reply: a file named in Mac Roman, with its date */
struct afp2_page {
    struct dsi_header header __attribute__((__packed__));
    uint16_t filebitmap;
    uint16_t dirbitmap;
    uint16_t reqcount;
    uint8_t file_size;
    uint8_t file_isdir;
    uint32_t file_mod;
    uint16_t file_name_offset;
    uint32_t file_len;
    uint8_t file_name_len;
    char file_name[4];
    uint8_t file_align;
} __attribute__((__packed__));

/*!
 * @brief Run the listing checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    struct afp_versions afp34 = { "AFP3.4", 34 };
    struct afp_server server;
    struct afp_volume volume;
    struct afp_listing listing, other;
    struct ext2_page page;
    struct long_page longname;
    struct afp2_page afp2;
    struct stat st;
    struct timespec ts;
    const char *grow = ".comment.comment.comment.comment";
    uint32_t capacity, pool_cap;
    const struct afp_dirent *file;
    const struct afp_dirent *dir;
    test_tap_init(argc, argv);
    memset(&server, 0, sizeof(server));
    memset(&volume, 0, sizeof(volume));
    memset(&listing, 0, sizeof(listing));
    memset(&other, 0, sizeof(other));
    server.using_version = &afp34;
    server.connect_time = 1234567890;
    volume.server = &server;
    volume.mapping = AFP_MAPPING_COMMON;
    memset(&page, 0, sizeof(page));
    page.filebitmap = htons(kFPParentDirIDBit | kFPModDateBit | kFPNodeIDBit
                            | kFPExtDataForkLenBit | kFPUTF8NameBit);
    page.dirbitmap = htons(kFPParentDirIDBit | kFPModDateBit | kFPNodeIDBit
                           | kFPOffspringCountBit | kFPUTF8NameBit);
    page.reqcount = htons(2);
    page.file_size = htons(40);
    page.file_parent = htonl(2);
    page.file_mod = afp_date_from_unix(&server, (time_t)1700000000);
    page.file_node = htonl(17);
    page.file_len = hton64(UINT64_C(12345));
    page.file_name_offset = htons(26);
    page.file_name_len = htons(3);
    memcpy(page.file_name, "e\xcc\x81", 3);
    page.dir_size = htons(34);
    page.dir_isdir = 0x80;
    page.dir_parent = htonl(2);
    page.dir_mod = htonl(UINT32_C(0x80000000));
    page.dir_node = htonl(77);
    page.dir_offspring = htons(3);
    page.dir_name_offset = htons(20);
    page.dir_name_len = htons(3);
    memcpy(page.dir_name, "dir", 3);
    /* The page's two entries, their names converted into the pool */
    CHECK(afp_enumerateext2_reply(&server, (char *)&page, sizeof(page),
                                  &listing) == 0);
    CHECK(listing.count == 2 && listing.capacity >= 2);
    file = &listing.entries[0];
    dir = &listing.entries[1];
    CHECK(!file->isdir && file->parent_did == 2 && file->cnid == 17
          && file->size == 12345);
    CHECK((file->valid & kFPModDateBit)
          && file->mod_ns == INT64_C(1700000000) * AFP_NS_PER_SEC);
    CHECK(strcmp(afp_dirent_name(&listing, file), "\xc3\xa9") == 0);
    CHECK(dir->isdir && dir->cnid == 77 && dir->offspring == 3
          && !(dir->valid & kFPModDateBit));
    CHECK(strcmp(afp_dirent_name(&listing, dir), "dir") == 0);
    CHECK(dir->name_off == file->name_off + 3);
    /* A page whose entry runs past the reply is dropped whole. */
    page.dir_size = htons(200);
    CHECK(afp_enumerateext2_reply(&server, (char *)&page, sizeof(page),
                                  &listing) == -1);
    CHECK(listing.count == 2 && listing.pool_used == 7);
    /* A name no name buffer holds fails its page, as a malformed reply does. */
    memset(&longname, 0, sizeof(longname));
    longname.filebitmap = htons(kFPUTF8NameBit);
    longname.reqcount = htons(1);
    longname.file_size = htons(1038);
    longname.file_name_offset = htons(6);
    longname.file_name_len = htons(1021);
    memset(longname.file_name, 'a', sizeof(longname.file_name));
    CHECK(afp_enumerateext2_reply(&server, (char *)&longname,
                                  sizeof(longname), &listing) == -1);
    CHECK(listing.count == 2 && listing.pool_used == 7);
    /* A malformed name lists as it decodes: a cut sequence shows as one '*',
     * and one starting with a continuation byte decodes to nothing and goes. */
    page.dir_size = htons(34);
    memcpy(page.file_name, "a\xc3x", 3);
    CHECK(afp_enumerateext2_reply(&server, (char *)&page, sizeof(page),
                                  &other) == 0
          && strcmp(afp_dirent_name(&other, &other.entries[0]), "a*") == 0);
    memcpy(page.file_name, "\x80" "ab", 3);
    CHECK(afp_enumerateext2_reply(&server, (char *)&page, sizeof(page),
                                  &other) == 0
          && other.count == 4
          && afp_dirent_name(&other, &other.entries[2])[0] == '\0');
    afp_listing_drop_unnamed(&other);
    CHECK(other.count == 3
          && strcmp(afp_dirent_name(&other, &other.entries[2]), "dir") == 0);
    afp_listing_free(&other);
    /* An FPEnumerate page, its Mac Roman name converted and its date read
     * back */
    memset(&afp2, 0, sizeof(afp2));
    afp2.filebitmap = htons(kFPModDateBit | kFPLongNameBit
                            | kFPDataForkLenBit);
    afp2.reqcount = htons(1);
    afp2.file_size = 18;
    afp2.file_mod = afp_date_from_unix(&server, (time_t)1600000000);
    afp2.file_name_offset = htons(10);
    afp2.file_len = htonl(4096);
    afp2.file_name_len = 4;
    memcpy(afp2.file_name, "caf\x8e", 4);
    CHECK(afp_enumerate_reply(&server, (char *)&afp2, sizeof(afp2),
                              &listing) == 0);
    CHECK(listing.count == 3 && listing.entries[2].size == 4096
          && strcmp(afp_dirent_name(&listing, &listing.entries[2]),
                    "caf\xc3\xa9") == 0);
    CHECK((listing.entries[2].valid & kFPModDateBit)
          && listing.entries[2].mod_ns
          == INT64_C(1600000000) * AFP_NS_PER_SEC);
    /* A pseudo-entry copies its entry under the name with a suffix. */
    CHECK(afp_listing_append(&listing, &listing.entries[0], ".finderinfo",
                             AFP_META_FINDERINFO, 32) == 0);
    CHECK(listing.count == 4 && listing.entries[3].kind == AFP_META_FINDERINFO
          && listing.entries[3].size == 32 && listing.entries[3].cnid == 17
          && strcmp(afp_dirent_name(&listing, &listing.entries[3]),
                    "\xc3\xa9.finderinfo") == 0);
    /* The stat form: a file without UNIX privileges is 0600, its node ID
     * the inode, its dates as sent. */
    file = &listing.entries[0];
    CHECK(afp_dirent_to_stat(&volume, file, &st) == 0);
    CHECK(S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0600
          && st.st_ino == 17 && st.st_size == 12345 && st.st_nlink == 1);
    CHECK(st.st_mtime == (time_t)1700000000);
    /* A directory has offspring + 2 links and the size Mac OS X showed; a
     * date the server reports unset is the connection's time. */
    dir = &listing.entries[1];
    CHECK(afp_dirent_to_stat(&volume, dir, &st) == 0);
    CHECK(S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 0700
          && st.st_nlink == 5 && st.st_size == 3 * 34 + 24);
    CHECK(st.st_mtime == server.connect_time
          && st.st_ctime == server.connect_time);
    /* A pseudo-file is a regular file of its own size, a folder's too. */
    CHECK(afp_dirent_to_stat(&volume, &listing.entries[3], &st) == 0);
    CHECK(S_ISREG(st.st_mode) && st.st_size == 32);
    CHECK(afp_listing_append(&listing, &listing.entries[1], ".comment",
                             AFP_META_COMMENT, 5) == 0);
    CHECK(afp_dirent_mode(&listing.entries[4]) == (S_IFREG | 0700)
          && listing.entries[4].isdir);
    listing.entries[1].valid |= kFPUnixPrivsBit;
    listing.entries[1].unixprivs.permissions = S_IFDIR | 0751;
    CHECK(afp_listing_append(&listing, &listing.entries[1], ".finderinfo",
                             AFP_META_FINDERINFO, 32) == 0);
    CHECK(afp_dirent_mode(&listing.entries[5]) == (S_IFREG | 0751));
    /* A pseudo-file has one link and the blocks of its own size. */
    CHECK(afp_dirent_to_stat(&volume, &listing.entries[5], &st) == 0
          && st.st_nlink == 1 && st.st_blocks == 1);
    /* Appends from the listing's own array grow both arrays: each copy
     * takes its entry as the entry stood. */
    capacity = listing.capacity;
    pool_cap = listing.pool_cap;

    for (uint32_t i = listing.count; i <= capacity; i++) {
        if (afp_listing_append(&listing, &listing.entries[0], grow,
                               AFP_META_COMMENT, 5) != 0) {
            break;
        }
    }

    CHECK(listing.capacity > capacity && listing.pool_cap > pool_cap);
    file = &listing.entries[listing.count - 1];
    CHECK(listing.count == capacity + 1 && file->cnid == 17
          && strcmp(afp_dirent_name(&listing, file),
                    "\xc3\xa9.comment.comment.comment.comment") == 0);
    file = &listing.entries[0];
    /* An owner the id mapping cannot express fails the presentation. */
    volume.mapping = AFP_MAPPING_UNKNOWN;
    CHECK(afp_dirent_to_stat(&volume, file, &st) == -EIO);
    /* A fork length past off_t's range clamps; a date before 1970 borrows
     * a second for its fraction, and a whole second keeps its own. */
    volume.mapping = AFP_MAPPING_COMMON;
    listing.entries[0].size = UINT64_MAX;
    CHECK(afp_dirent_to_stat(&volume, &listing.entries[0], &st) == 0
          && st.st_size == INT64_MAX);
    ts = afp_dirent_time(&volume, &listing.entries[0], kFPModDateBit,
                         INT64_C(-1500000000));
    CHECK(ts.tv_sec == -2 && ts.tv_nsec == 500000000);
    ts = afp_dirent_time(&volume, &listing.entries[0], kFPModDateBit,
                         INT64_C(-2000000000));
    CHECK(ts.tv_sec == -2 && ts.tv_nsec == 0);
    afp_listing_free(&listing);
    CHECK(listing.entries == NULL && listing.pool == NULL
          && listing.count == 0 && listing.capacity == 0);
    /* A time past the record's range, the year 2262, saturates; the last
     * whole second within it does not. */
    CHECK(sizeof(time_t) < sizeof(int64_t)
          || afp_ns_from_sec((time_t)(INT64_MAX / AFP_NS_PER_SEC) + 1)
          == INT64_MAX);
    CHECK(sizeof(time_t) < sizeof(int64_t)
          || afp_ns_from_sec((time_t)(INT64_MAX / AFP_NS_PER_SEC))
          == INT64_MAX / AFP_NS_PER_SEC * AFP_NS_PER_SEC);
    return test_tap_finish();
}
