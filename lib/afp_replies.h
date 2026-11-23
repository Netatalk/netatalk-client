/*!
 * @file
 * @brief The parsers of AFP replies, and the call that picks one
 *
 * Part of the protocol layer: the reply parsers, and the call that picks one.
 * Lanes: data, metadata, directory, session.
 * Southbound: the DSI layer, which hands a reply up through that call.
 */

#ifndef __AFP_REPLIES_H_
#define __AFP_REPLIES_H_

int parse_reply_block(struct afp_server *server, const char *buf,
                      size_t size, unsigned char isdir,
                      unsigned int filebitmap, unsigned int dirbitmap,
                      struct afp_dirent *e, char *name, size_t name_len);

int afp_reply(unsigned short subcommand, struct afp_server * server,
              void *other);

int afp_opendt_reply(struct afp_server *server, char *buf, unsigned int size,
                     void *other);

int afp_getcomment_reply(struct afp_server *server, char *buf,
                         unsigned int size, void *other);

int afp_geticon_reply(struct afp_server *server, char *buf, unsigned int size,
                      void *other);
int afp_geticoninfo_reply(struct afp_server *server, char *buf,
                          unsigned int size, void *other);
int afp_getappl_reply(struct afp_server *server, char *buf,
                      unsigned int size, void *other);

int afp_login_reply(struct afp_server *server, char *buf, unsigned int size,
                    void *other);

int afp_changepassword_reply(struct afp_server *server, char *buf,
                             unsigned int size, void *other);

int afp_getsessiontoken_reply(struct afp_server *server, char *buf,
                              unsigned int size, void *other);

int afp_getsrvrparms_reply(struct afp_server *server, char *msg,
                           unsigned int size, void *other);

int afp_getsrvrmsg_reply(struct afp_server *server, char *buf,
                         unsigned int size, void *other);

int afp_mapname_reply(struct afp_server *server, char *buf, unsigned int size,
                      void *other);

int afp_mapid_reply(struct afp_server *server, char *buf, unsigned int size,
                    void *other);

int afp_getuserinfo_reply(struct afp_server *server, char *buf,
                          unsigned int size, void *other);

int afp_volopen_reply(struct afp_server *server, char *buf, unsigned int size,
                      void *ignored);

int afp_getfiledirparms_reply(struct afp_server *server, char *buf,
                              unsigned int size, void *other);

int afp_enumerate_reply(struct afp_server *server, char *buf,
                        unsigned int size, void *other);

int afp_enumerateext2_reply(struct afp_server *server, char *buf,
                            unsigned int size, void *other);

int afp_getvolparms_reply(struct afp_server *server, char *buf,
                          unsigned int size, void *other);

int afp_openfork_reply(struct afp_server *server, char *buf, unsigned int size,
                       void *x);

int afp_createdir_reply(struct afp_server * server, char *buf,
                        unsigned int len, void *dir_p);

int afp_read_reply(struct afp_server *server, char *buf, unsigned int size,
                   void *other);

int afp_readext_reply(struct afp_server *server, char *buf, unsigned int size,
                      void *other);

int afp_write_reply(struct afp_server *server, char *buf, unsigned int size,
                    void *other);

int afp_writeext_reply(struct afp_server *server, char *buf, unsigned int size,
                       void *other);

int afp_byterangelock_reply(struct afp_server *server, char *buf,
                            unsigned int size, void *x);

int afp_byterangelockext_reply(struct afp_server *server, char *buf,
                               unsigned int size, void *x);

int afp_listextattrs_reply(struct afp_server *server, char *buf,
                           unsigned int size, void *x);

int afp_getextattr_reply(struct afp_server *server, char *buf,
                         unsigned int size, void *other);

#endif
