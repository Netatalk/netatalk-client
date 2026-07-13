executable(
    'afpgetstatus',
    sources: 'getstatus.c',
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
    install: true,
    install_rpath: install_runtime_path,
)

if with_afpcmd
    afpcmd_incdir = include_directories(['.', '../include', include_dirs])

    executable(
        'afpcmd',
        sources: [
            'cmdline_afp.c',
            'cmdline_main.c',
        ],
        include_directories: afpcmd_incdir,
        link_with: libafpsl,
        dependencies: [root_dependencies, cmdline_dependencies],
        c_args: cflags,
        install: true,
        install_rpath: install_runtime_path,
    )
endif
