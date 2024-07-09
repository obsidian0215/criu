#ifndef _CRIU_LINUX_MOUNT_H
#define _CRIU_LINUX_MOUNT_H

#include "common/config.h"
#include "compel/plugins/std/syscall-codes.h"

/* Copied from /usr/include/sys/mount.h */

#ifndef FSCONFIG_CMD_CREATE
/* The type of fsconfig call made.   */
enum fsconfig_command {
	FSCONFIG_SET_FLAG = 0, /* Set parameter, supplying no value */
#define FSCONFIG_SET_FLAG FSCONFIG_SET_FLAG
	FSCONFIG_SET_STRING = 1, /* Set parameter, supplying a string value */
#define FSCONFIG_SET_STRING FSCONFIG_SET_STRING
	FSCONFIG_SET_BINARY = 2, /* Set parameter, supplying a binary blob value */
#define FSCONFIG_SET_BINARY FSCONFIG_SET_BINARY
	FSCONFIG_SET_PATH = 3, /* Set parameter, supplying an object by path */
#define FSCONFIG_SET_PATH FSCONFIG_SET_PATH
	FSCONFIG_SET_PATH_EMPTY = 4, /* Set parameter, supplying an object by (empty) path */
#define FSCONFIG_SET_PATH_EMPTY FSCONFIG_SET_PATH_EMPTY
	FSCONFIG_SET_FD = 5, /* Set parameter, supplying an object by fd */
#define FSCONFIG_SET_FD FSCONFIG_SET_FD
	FSCONFIG_CMD_CREATE = 6, /* Invoke superblock creation */
#define FSCONFIG_CMD_CREATE FSCONFIG_CMD_CREATE
	FSCONFIG_CMD_RECONFIGURE = 7, /* Invoke superblock reconfiguration */
#define FSCONFIG_CMD_RECONFIGURE FSCONFIG_CMD_RECONFIGURE
};
#endif // FSCONFIG_CMD_CREATE

#ifndef MS_MGC_VAL
/* Magic mount flag number. Has to be or-ed to the flag values.  */
#define MS_MGC_VAL 0xc0ed0000 /* Magic flag number to indicate "new" flags */
#define MS_MGC_MSK 0xffff0000 /* Magic flag number mask */
#endif

/* Possible value for FLAGS parameter of `umount2'.  */
enum
{
  MNT_FORCE = 1,		/* Force unmounting.  */
#define MNT_FORCE MNT_FORCE
  MNT_DETACH = 2,		/* Just detach from the tree.  */
#define MNT_DETACH MNT_DETACH
  MNT_EXPIRE = 4,		/* Mark for expiry.  */
#define MNT_EXPIRE MNT_EXPIRE
  UMOUNT_NOFOLLOW = 8		/* Don't follow symlink on umount.  */
#define UMOUNT_NOFOLLOW UMOUNT_NOFOLLOW
};

__BEGIN_DECLS

/* Mount a filesystem.  */
extern int mount (const char *__special_file, const char *__dir,
		  const char *__fstype, unsigned long int __rwflag,
		  const void *__data) __THROW;

/* Unmount a filesystem.  */
extern int umount (const char *__special_file) __THROW;

/* Unmount a filesystem.  Force unmounting if FLAGS is set to MNT_FORCE.  */
extern int umount2 (const char *__special_file, int __flags) __THROW;

__END_DECLS

#endif
