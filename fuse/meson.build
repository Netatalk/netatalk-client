afpc_fs_sources = files('client.c')

afpfsd_sources = ['commands.c', 'daemon.c', 'fuse_fs.c', 'fuse_error.c']

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

install_symlink('mount_afpfs', pointing_to: 'afpc', install_dir: bindir)
