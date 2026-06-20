metadata_test = executable(
    'test_metadata',
    sources: ['test_metadata.c'],
    include_directories: [incdir, include_directories('../daemon')],
    link_with: libafpsl,
    dependencies: root_dependencies,
    c_args: cflags,
)

test('local metadata storage', metadata_test)

protocol_replies_test = executable(
    'test_protocol_replies',
    sources: ['test_protocol_replies.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test('AFP protocol reply parsing', protocol_replies_test)

log_test = executable(
    'test_log',
    sources: ['test_log.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test('log message escaping', log_test)

stateless_log_test = executable(
    'test_stateless_log',
    sources: ['test_stateless_log.c'],
    include_directories: [incdir, include_directories('../daemon')],
    link_with: libafpsl,
    dependencies: root_dependencies,
    c_args: cflags,
)

test('stateless log callback and framing', stateless_log_test)
