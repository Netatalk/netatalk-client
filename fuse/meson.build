mount_afpfs_sources = ['client.c']

executable(
    'mount_afpfs',
    sources: mount_afpfs_sources,
    include_directories: incdir,
    link_with: libafpclient,
    c_args: cflags,
    dependencies: [root_dependencies],
    install: true,
    install_rpath: install_runtime_path,
)

afpfsd_sources = ['commands.c', 'daemon.c', 'fuse_int.c', 'fuse_error.c']

executable(
    'afpfsd',
    sources: afpfsd_sources,
    include_directories: incdir,
    link_with: [libafpclient, daemon_common],
    c_args: cflags,
    dependencies: [root_dependencies, fuse_dep, pthread_dep],
    install: true,
    install_rpath: install_runtime_path,
)

install_symlink('afp_client', pointing_to: 'mount_afpfs', install_dir: bindir)
