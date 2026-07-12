#ifndef __UAM_DEFS_H_
#define __UAM_DEFS_H_

#define UAM_NEED_LIBGCRYPT_VERSION "1.4.0"

#define UAM_NOUSERAUTHENT 0x1
#define UAM_CLEARTXTPASSWRD 0x2
#define UAM_RANDNUMEXCHANGE 0x4
#define UAM_2WAYRANDNUM 0x8
#define UAM_DHCAST128 0x10
#define UAM_CLIENTKRB 0x20
#define UAM_DHX2 0x40
#define UAM_RECON1 0x80
#define UAM_SRP 0x100

const char *resolve_uam_shorthand(const char *name);
int uam_string_to_bitmap(char * name);
char *uam_bitmap_to_string(unsigned int bitmap);

#endif
