afpc_sources = files(
    'afpc.c',
    'controller_sl.c',
)
afpc_links = [libafpsl]
afpc_args = cflags

if with_fuse
    afpc_sources += afpc_fs_sources
    afpc_links += libafpclient
    afpc_args += '-DHAVE_AFPC_FS'
endif

afpc_exe = executable(
    'afpc',
    sources: afpc_sources,
    include_directories: [incdir, include_directories('.')],
    link_with: afpc_links,
    dependencies: [root_dependencies, discovery_client_dep],
    c_args: afpc_args,
    install: true,
    install_rpath: install_runtime_path,
)
