#ifndef _AFP_PROTOCOL_H_
#define _AFP_PROTOCOL_H_

#include <sys/types.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>

#include "netatalk-client/types.h"

/* This file defines constants for the Apple Filing Protocol.
   Section references are to the AFP Programming Guide and
   AFP Reference, version 3.4.
 */

#define AFP_SERVER_NAME_LEN 33
#define AFP_SERVER_NAME_UTF8_LEN 255
#define AFP_VOLUME_NAME_LEN 33
#define AFP_VOLUME_NAME_UTF8_LEN 255
#define AFP_MOUNTPOINT_LEN 255
#define AFP_SIGNATURE_LEN 16
#define AFP_MACHINETYPE_LEN 16
#define AFP_LOGINMESG_LEN 200
#define AFP_VOLPASS_LEN 8
#define AFP_HOSTNAME_LEN 255
#define AFP_ZONE_LEN 32
#define AFP_SERVER_ICON_LEN 256
#define AFP_MAX_USERNAME_LEN 127
#define AFP_MAX_PASSWORD_LEN 127

/* This is the maximum length of any UAM string */
#define AFP_UAM_LENGTH 24

/* This is the maximum length of any path description */
#define AFP_MAX_PATH 768

#define AFP_VOL_FLAT 1
#define AFP_VOL_FIXED 2
#define AFP_VOL_VARIABLE 3

/* AFP Programming Guide: AFP Concepts > Directories and Files > Directory IDs */
#define AFP_ROOT_DID 2

/* AFP Reference: Constants > AFP Protocol Constants > Path Type Constants */
enum {
    kFPShortName = 1,
    kFPLongName = 2,
    kFPUTF8Name = 3
};

/* fork types */
#define AFP_FORKTYPE_DATA 0x0
#define AFP_FORKTYPE_RESOURCE 0x80

/* AFP Reference: FPOpenFork > Parameters > AccessMode */
#define AFP_OPENFORK_ALLOWREAD 1
#define AFP_OPENFORK_ALLOWWRITE 2
#define AFP_OPENFORK_DENYREAD 0x10
#define AFP_OPENFORK_DENYWRITE 0x20

/* AFP Reference: FPGetSrvrMsg > Parameters > MessageType */
typedef enum {
    AFPMESG_LOGIN = 0,
    AFPMESG_SERVER = 1
} afpmessage_t;

/* AFP Reference: Constants > AFP Protocol Constants > FPGetSrvrMsg Bitmap */
#define AFP_GETSRVRMSG_UTF8 0x2
#define AFP_GETSRVRMSG_GETMSG 0x1

/* AFP Programming Guide: AFP Concepts > File Server */
#define AFP_MAX_VERSION_LENGTH 16

/* Maximum length of a token, this is undocumented */
#define AFP_TOKEN_MAX_LEN 256

/* The maximum size of a file for AFP 2 */
#define AFP_MAX_AFP2_FILESIZE (4294967296)

/* AFP Reference: Constants > AFP Protocol Constants > Volume Attributes Bitmap */
enum {
    kReadOnly = 0x01,
    kHasVolumePassword = 0x02,
    kSupportsFileIDs = 0x04,
    kSupportsCatSearch = 0x08,
    kSupportsBlankAccessPrivs = 0x10,
    kSupportsUnixPrivs = 0x20,
    kSupportsUTF8Names = 0x40,
    kNoNetworkUserIDs = 0x80,
    kDefaultPrivsFromParent = 0x100,
    kNoExchangeFiles = 0x200,
    kSupportsExtAttrs = 0x400,
    kSupportsACLs = 0x800,
    kCaseSensitive = 0x1000,
    kSupportsTMLockSteal = 0x2000
};

/* AFP Reference: Constants > AFP Protocol Constants > File Creation Constants */
enum {
    kFPSoftCreate = 0,
    kFPHardCreate = 0x80
};

/* AFP Reference: Constants > AFP Protocol Constants > File and Directory Bitmap */
enum {
    kFPAttributeBit = 0x1,
    kFPParentDirIDBit = 0x2,
    kFPCreateDateBit = 0x4,
    kFPModDateBit = 0x8,
    kFPBackupDateBit = 0x10,
    kFPFinderInfoBit = 0x20,
    kFPLongNameBit = 0x40,
    kFPShortNameBit = 0x80,
    kFPNodeIDBit = 0x100,
    kFPOffspringCountBit = 0x0200,
    kFPOwnerIDBit = 0x0400,
    kFPGroupIDBit = 0x0800,
    kFPAccessRightsBit = 0x1000,
    kFPProDOSInfoBit = 0x2000, /* AFP version 2.2 and earlier */
    kFPUTF8NameBit = 0x2000, /* AFP version 3.0 and later */
    kFPUnixPrivsBit = 0x8000 /* AFP version 3.0 and later */
};

/* AFP Reference: Constants > AFP Protocol Constants > File and Directory Bitmap.
   These are the bits not in the directory bitmap above. */
enum {
    kFPDataForkLenBit = 0x0200,
    kFPRsrcForkLenBit = 0x0400,
    kFPExtDataForkLenBit = 0x0800, /* AFP version 3.0 and later */
    kFPLaunchLimitBit = 0x1000,
    kFPExtRsrcForkLenBit = 0x4000, /* AFP version 3.0 and later */
};

/* AFP Reference: Constants > AFP Protocol Constants > Extended Attributes Bitmap */
enum {
    kXAttrNoFollow = 0x1,
    kXAttrCreate = 0x2,
    kXAttrReplace = 0x4
};

/* AFP Reference: AFP Commands (CommandCode for each command) */
enum AFPFunction {
    afpByteRangeLock = 1, afpCloseVol, afpCloseDir, afpCloseFork,
    afpCopyFile, afpCreateDir, afpCreateFile, afpDelete,
    afpEnumerate, afpFlush, afpFlushFork,

    afpGetForkParms = 14, afpGetSrvrInfo, afpGetSrvrParms, afpGetVolParms,
    afpLogin, afpLoginCont, afpLogout, afpMapID,
    afpMapName, afpMoveAndRename, afpOpenVol, afpOpenDir,
    afpOpenFork, afpRead, afpRename, afpSetDirParms,
    afpSetFileParms, afpSetForkParms, afpSetVolParms, afpWrite,
    afpGetFileDirParms, afpSetFileDirParms, afpChangePassword, afpGetUserInfo,
    afpGetSrvrMsg, afpCreateID, afpDeleteID, afpResolveID,
    afpExchangeFiles, afpCatSearch,

    afpOpenDT = 48, afpCloseDT,

    afpGetIcon = 51, afpGetIconInfo, afpAddAPPL, afpRemoveAPPL,
    afpGetAPPL, afpAddComment, afpRemoveComment, afpGetComment,
    afpByteRangeLockExt, afpReadExt, afpWriteExt, afpGetAuthMethods,
    afpLoginExt, afpGetSessionToken, afpDisconnectOldSession, afpEnumerateExt,
    afpCatSearchExt, afpEnumerateExt2, afpGetExtAttr, afpSetExtAttr,
    afpRemoveExtAttr, afpListExtAttrs, afpGetACL, afpSetACL,
    afpAccess, afpSpotlightRPC,

    afpSyncDir = 78, afpSyncFork,

    afpZzzzz = 122,

    afpAddIcon = 192,
};

/* AFP Reference: Constants > AFP Protocol Constants > Volume Bitmap */
enum {
    kFPBadVolPre222Bitmap = 0xFe00,
    kFPBadVolBitmap = 0xF000,
    kFPVolAttributeBit = 0x1,
    kFPVolSignatureBit = 0x2,
    kFPVolCreateDateBit = 0x4,
    kFPVolModDateBit = 0x8,
    kFPVolBackupDateBit = 0x10,
    kFPVolIDBit = 0x20,
    kFPVolBytesFreeBit = 0x40,
    kFPVolBytesTotalBit = 0x80,
    kFPVolNameBit = 0x100,
    kFPVolExtBytesFreeBit = 0x200,
    kFPVolExtBytesTotalBit = 0x400,
    kFPVolBlockSizeBit = 0x800
};

/* AFP Attention Codes -- 4 bits */
#define AFPATTN_SHUTDOWN     (1 << 15)            /* shutdown/disconnect */
#define AFPATTN_CRASH        (1 << 14)            /* server crashed */
#define AFPATTN_MESG         (1 << 13)            /* server has message */
#define AFPATTN_NORECONNECT  (1 << 12)            /* don't reconnect */
/* server notification */
#define AFPATTN_NOTIFY       (AFPATTN_MESG | AFPATTN_NORECONNECT)

/* extended bitmap -- 12 bits. volchanged is only useful w/ a server
 * notification, and time is only useful for shutdown. */
#define AFPATTN_VOLCHANGED   (1 << 0)             /* volume has changed */
#define AFPATTN_TIME(x)      ((x) & 0xfff)        /* time in minutes */

#define kFPNoErr 0

/* AFP Reference: Result Codes */
#define kFPNoMoreSessions -1068
#define kASPSessClosed -1072
#define kFPAccessDenied -5000
#define kFPAuthContinue -5001
#define kFPBadUAM -5002
#define kFPBadVersNum -5003
#define kFPBitmapErr -5004
#define kFPCantMove -5005
#define kFPDenyConflict -5006
#define kFPDirNotEmpty -5007
#define kFPDiskFull -5008
#define kFPEOFErr -5009
#define kFPFileBusy -5010
#define kFPFlatVol -5011
#define kFPItemNotFound -5012
#define kFPLockErr -5013
#define kFPMiscErr -5014
#define kFPNoMoreLocks -5015
#define kFPNoServer -5016
#define kFPObjectExists -5017
#define kFPObjectNotFound -5018
#define kFPParamErr -5019
#define kFPRangeNotLocked -5020
#define kFPRangeOverlap -5021
#define kFPSessClosed -5022
#define kFPUserNotAuth -5023
#define kFPCallNotSupported -5024
#define kFPObjectTypeErr -5025
#define kFPTooManyFilesOpen -5026
#define kFPServerGoingDown -5027
#define kFPCantRename -5028
#define kFPDirNotFound -5029
#define kFPIconTypeError -5030
#define kFPVolLocked -5031
#define kFPObjectLocked -5032
#define kFPContainsSharedErr -5033
#define kFPIDNotFound -5034
#define kFPIDExists -5035
#define kFPDiffVolErr  -5036
#define kFPCatalogChanged -5037
#define kFPSameObjectErr -5038
#define kFPBadIDErr -5039
#define kFPPwdSameErr -5040
#define kFPPwdTooShortErr -5041
#define kFPPwdExpiredErr -5042
#define kFPInsideSharedErr -5043
#define kFPInsideTrashErr -5044
#define kFPPwdNeedsChangeErr -5045
#define kFPPwdPolicyErr -5046
#define kFPDiskQuotaExceeded -5047


/* These flags determine to lock or unlock in ByteRangeLock(Ext) */
enum {
    ByteRangeLock_Lock = 0,
    ByteRangeLock_Unlock = 1
};

/* AFP Reference: FPGetSrvrParms > Reply Block */
#define HasConfigInfo 0x1
#define HasPassword 0x80

/* AFP Reference: Constants > AFP Protocol Constants > FPMapID Constants */
enum {
    kUserIDToName = 1,
    kGroupIDToName = 2,
    kUserIDToUTF8Name = 3,
    kGroupIDToUTF8Name = 4,
    kUserUUIDToUTF8Name = 5,
    kGroupUUIDToUTF8Name = 6
};

/* AFP Reference: Constants > AFP Protocol Constants > FPMapName Constants */
enum {
    kNameToUserID = 1,
    kNameToGroupID = 2,
    kUTF8NameToUserID = 3,
    kUTF8NameToGroupID = 4,
    kUTF8NameToUserUUID = 5,
    kUTF8NameToGroupUUID = 6
};

/* AFP Reference: FPGetUserInfo > Parameters > Bitmap */
#define kFPGetUserInfo_USER_ID 1
#define kFPGetUserInfo_PRI_GROUPID 2

/* AFP Reference: Constants > AFP Protocol Constants > Server Flags Bitmap */
enum {
    kSupportsCopyfile = 0x01,
    kSupportsChgPwd = 0x02,
    kDontAllowSavePwd = 0x04,
    kSupportsSrvrMsg = 0x08,
    kSrvrSig = 0x10,
    kSupportsTCP = 0x20,
    kSupportsSrvrNotify = 0x40,
    kSupportsReconnect = 0x80,
    kSupportsDirServices = 0x100,
    kSupportsUTF8SrvrName = 0x200,
    kSupportsUUIDs = 0x400,
    kSupportsExtSleep = 0x800,
    kSupportsSuperClient = 0x8000
};

/* AFP Reference: Constants > AFP Protocol Constants > FPGetSessionToken Types */
enum {
    kLoginWithoutID = 0,
    kLoginWithID = 1,
    kReconnWithID = 2,
    kLoginWithTimeAndID = 3,
    kReconnWithTimeAndID = 4,
    kRecon1Login = 5,
    kRecon1ReconnectLogin = 6,
    kRecon1RefreshToken = 7,
    kGetKerberosSessionKey = 8
};

#endif
