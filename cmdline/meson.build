executable(
    'afpgetstatus',
    sources: 'getstatus.c',
    include_directories: incdir,
    link_with: libafpclient,
    c_args: cflags,
    install: true,
)

if with_afpcmd
    executable(
        'afpcmd',
        sources: ['cmdline_afp.c', 'cmdline_main.c'],
        include_directories: incdir,
        link_with: [libafpclient, libafpsl],
        dependencies: [root_dependencies, cmdline_dependencies],
        c_args: cflags,
        install: true,
    )
endif
