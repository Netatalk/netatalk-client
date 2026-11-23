/*!
 * @file
 * @brief The entry to a mount's FUSE loop
 *
 * The interface of fuse_fs.c.
 * Lanes: data, metadata, directory.
 * Northbound: the mount thread of commands.c.
 */

#ifndef NETATALK_CLIENT_FUSE_FS_H
#define NETATALK_CLIENT_FUSE_FS_H

struct afp_volume;

int afp_fuse_main(int argc, char *argv[], struct afp_volume *volume);

#endif
