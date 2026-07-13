#ifndef NETATALK_CLIENT_FUSE_FS_H
#define NETATALK_CLIENT_FUSE_FS_H

struct afp_volume;

int afp_fuse_main(int argc, char *argv[], struct afp_volume *volume);

#endif
