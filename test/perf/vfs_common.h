/* Helpers shared by vfs_lantest and vfs_speedtest */

#ifndef VFS_COMMON_H
#define VFS_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define KIB 1024
#define MIB (KIB * KIB)
#define FILE_MODE 0640
#define DIR_MODE 0750
#define EXIT_USAGE 1
#define EXIT_TESTFAIL 2

/* Name printed in front of every diagnostic */
extern const char *vfs_progname;
/* O_DIRECT, or 0 after -D */
extern int vfs_data_flags;
/* Runs before a fatal exit; removes what the tool created */
extern void (*vfs_cleanup)(void);

_Noreturn void vfs_fatal(const char *what, const char *path);
_Noreturn void vfs_fatal_msg(const char *msg);
long vfs_opt_number(const char *arg, long lo, long hi, void (*usage)(void));
char *vfs_aligned_zeroed(size_t len);
void vfs_join(char *dst, const char *dir, const char *name);
int vfs_open(const char *path, int flags);
void vfs_close(int fd, const char *path);
void vfs_mkdir(const char *path);
void vfs_rmdir(const char *path);
void vfs_unlink(const char *path);
void vfs_mknod(const char *path);
void vfs_stat(const char *path);
void vfs_pwrite(int fd, const char *buf, size_t len, off_t off,
                const char *path);
void vfs_pread(int fd, char *buf, size_t len, off_t off, const char *path);
void vfs_copy_range(int in, int out, size_t len, const char *path);
void vfs_timer_start(void);
uint64_t vfs_timer_us(void);

#endif /* VFS_COMMON_H */
