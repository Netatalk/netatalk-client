# The integration suites' libafpsl client, which their image copies from the
# build tree.
executable(
    'afpsl_probe',
    sources: ['afpsl_probe.c'],
    include_directories: incdir,
    link_with: libafpsl,
    dependencies: root_dependencies,
    c_args: cflags,
)
