metadata_test = executable(
    'test_metadata',
    sources: ['test_metadata.c'],
    include_directories: [incdir, include_directories('../daemon')],
    link_with: libafpsl,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'local metadata storage',
    metadata_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

protocol_replies_test = executable(
    'test_protocol_replies',
    sources: ['test_protocol_replies.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'AFP protocol reply parsing',
    protocol_replies_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

log_test = executable(
    'test_log',
    sources: ['test_log.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'log message escaping',
    log_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

identify_test = executable(
    'test_identify',
    sources: ['test_identify.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'AFP server machine type identification',
    identify_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

stateless_log_test = executable(
    'test_stateless_log',
    sources: ['test_stateless_log.c'],
    include_directories: [incdir, include_directories('../daemon')],
    link_with: libafpsl,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'stateless log callback and framing',
    stateless_log_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)
