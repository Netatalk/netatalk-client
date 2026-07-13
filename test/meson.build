public_headers_test = executable(
    'test_public_headers',
    sources: ['test_public_headers.c'],
    include_directories: incdir,
    link_with: libafpsl,
)

test('public headers compile standalone', public_headers_test)

transport_api_test = executable(
    'test_transport_api',
    sources: ['test_transport_api.c'],
    include_directories: incdir,
    link_with: libafpclient,
)

test('stateful transport public API', transport_api_test)

daemon_socket_test = executable(
    'test_daemon_socket',
    sources: ['test_daemon_socket.c'],
    include_directories: incdir,
    link_with: daemon_common,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'daemon socket lifecycle',
    daemon_socket_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

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

versions_test = executable(
    'test_versions',
    sources: ['test_versions.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'AFP version selection',
    versions_test,
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
