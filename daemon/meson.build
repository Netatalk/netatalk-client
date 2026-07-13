libafpsl_version_major = 1
libafpsl_version_minor = 0
libafpsl_version_patch = 0
libafpsl_version = '@0@.@1@.@2@'.format(
    libafpsl_version_major,
    libafpsl_version_minor,
    libafpsl_version_patch,
)

afpsld_sources = [
    'daemon.c',
    'commands.c',
    'daemon_client.c',
]

executable(
    'afpsld',
    sources: afpsld_sources,
    dependencies: [
        root_dependencies,
        pthread_dep,
    ],
    link_with: [libafpclient, daemon_common],
    include_directories: incdir,
    c_args: cflags,
    install: true,
    install_rpath: install_runtime_path,
)

libafpsl = shared_library(
    'afpsl',
    sources: ['stateless.c', 'metadata.c'],
    dependencies: root_dependencies,
    include_directories: incdir,
    link_with: libafpclient,
    c_args: cflags,
    install: true,
    install_rpath: install_runtime_path,
    version: libafpsl_version,
    soversion: libafpsl_version_major,
)

pkg.generate(
    libafpsl,
    description: 'AFP stateless client library',
    name: 'libafpsl',
    version: libafpsl_version,
)

afpsl_dep = declare_dependency(
    link_with: libafpsl,
    include_directories: incdir,
    version: libafpsl_version,
)
