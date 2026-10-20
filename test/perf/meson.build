m_dep = cc.find_library('m', required: false)

vfs_common = static_library(
    'vfs_common',
    sources: ['vfs_common.c'],
    c_args: cflags,
)

vfs_lantest = executable(
    'vfs_lantest',
    sources: ['vfs_lantest.c'],
    c_args: cflags,
    dependencies: m_dep,
    link_with: vfs_common,
    install: true,
)

vfs_speedtest = executable(
    'vfs_speedtest',
    sources: ['vfs_speedtest.c'],
    c_args: cflags,
    dependencies: m_dep,
    link_with: vfs_common,
    install: true,
)

foreach tool : [vfs_lantest, vfs_speedtest]
    test(
        tool.name() + ' CSV layout',
        find_program('perl'),
        args: [files('test_' + tool.name() + '.t'), tool.full_path()],
        depends: tool,
        protocol: 'tap',
        verbose: true,
        timeout: 300,
    )
endforeach
