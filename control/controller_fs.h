#ifndef NETATALK_CLIENT_CONTROL_CONTROLLER_FS_H
#define NETATALK_CLIENT_CONTROL_CONTROLLER_FS_H

/* argv contains the command and its arguments, without the "fs" namespace. */
int afpc_fs_command(char *program_path, int argc, char **argv);
int afpc_mount_url_command(int argc, char **argv);

#endif
