libafpclient_version_major = 1
libafpclient_version_minor = 0
libafpclient_version_patch = 0
libafpclient_version = '@0@.@1@.@2@'.format(
    libafpclient_version_major,
    libafpclient_version_minor,
    libafpclient_version_patch,
)

libafpclient_sources = files(
    'afp.c',
    'afp_url.c',
    'client.c',
    'codepage.c',
    'connect.c',
    'daemon_signals.c',
    'daemon_socket.c',
    'debug.c',
    'did.c',
    'dsi.c',
    'explicit_bzero.c',
    'forklist.c',
    'identify.c',
    'log.c',
    'loop.c',
    'lowlevel.c',
    'map_def.c',
    'midlevel.c',
    'proto_attr.c',
    'proto_desktop.c',
    'proto_directory.c',
    'proto_files.c',
    'proto_fork.c',
    'proto_login.c',
    'proto_map.c',
    'proto_replyblock.c',
    'proto_server.c',
    'proto_session.c',
    'proto_volume.c',
    'resource.c',
    'server.c',
    'status.c',
    'transport.c',
    'uams.c',
    'uams_clrtxt.c',
    'uams_def.c',
    'unicode.c',
    'users.c',
    'utils.c',
)

if gcrypt_dep.found()
    libafpclient_sources += files(
        'uams_dhx.c',
        'uams_dhx2.c',
        'uams_randnum.c',
        'uams_srp.c',
    )
endif

libafpclient = shared_library(
    'afpclient',
    sources: libafpclient_sources,
    c_args: cflags,
    dependencies: [root_dependencies, libiconv_dep, gcrypt_dep, pthread_dep],
    include_directories: incdir,
    install: true,
    install_rpath: install_runtime_path,
    version: libafpclient_version,
    soversion: libafpclient_version_major,
)

pkg.generate(
    libafpclient,
    description: 'Stateful AFP transport client library',
    name: 'libafpclient',
    version: libafpclient_version,
)
