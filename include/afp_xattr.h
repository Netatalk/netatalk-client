#ifndef __AFP_XATTR_H_
#define __AFP_XATTR_H_

#include <errno.h>
#include <string.h>

/* ENOATTR is used by BSD and macOS, while Linux uses ENODATA for a missing
 * extended attribute. Keep both names available to shared xattr code. */
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

#define AFP_XATTR_RESOURCEFORK "com.apple.ResourceFork"
#define AFP_XATTR_FINDERINFO "com.apple.FinderInfo"

#define NETATALK_XATTR_META "org.netatalk.Metadata"
#define NETATALK_XATTR_META_LEN (sizeof(NETATALK_XATTR_META) - 1U)

#define AFP_EXTATTR_DATA_MAX 4096
#define AFP_XATTR_NAME_MAX 255U

#define AFP_XATTR_USER_PREFIX "user."
#define AFP_XATTR_USER_PREFIX_LEN 5

/* Utility for stripping the "user." prefix from xattr names. */
static inline const char *afp_xattr_strip_user_prefix(const char *name)
{
    if (name && strncmp(name, AFP_XATTR_USER_PREFIX,
                        AFP_XATTR_USER_PREFIX_LEN) == 0) {
        return name + AFP_XATTR_USER_PREFIX_LEN;
    }

    return name;
}

struct afp_extattr_info {
    unsigned int maxsize;
    unsigned int size;
    unsigned int copied;
    char data[AFP_EXTATTR_DATA_MAX];
};

/* Check if this is an internal server xattr that should be filtered */
#define IS_INTERNAL_SERVER_XATTR(name) \
    (strncmp((name), NETATALK_XATTR_META, NETATALK_XATTR_META_LEN) == 0)

#endif /* __AFP_XATTR_H_ */
