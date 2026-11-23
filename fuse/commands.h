/*!
 * @file
 * @brief afpfsd's registration with the library, and its log settings
 *
 * The interface of fuse/commands.c.
 * Lane: control.
 * Northbound: afpfsd's daemon.c.
 */

#ifndef __COMMANDS_H_
#define __COMMANDS_H_

int fuse_register_afpclient(void);
void fuse_set_log_method(int new_method);
void fuse_set_log_level(int loglevel);

#endif
