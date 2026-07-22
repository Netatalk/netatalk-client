public_headers_test = executable(
    'test_public_headers',
    sources: ['test_public_headers.c'],
    include_directories: incdir,
    link_with: libafpsl,
)

test('public headers compile standalone', public_headers_test)

public_header_checks = {
    'afpsl': 'netatalk-client/afpsl.h',
    'transport': 'netatalk-client/transport.h',
    'types': 'netatalk-client/types.h',
    'url': 'netatalk-client/url.h',
}

private_header_checks = {
    'discovery': 'discovery/discovery.h',
    'discovery_backend': 'discovery/backend.h',
    'discovery_client': 'discovery/client/discover.h',
    'fuse_ipc': 'fuse/fuse_ipc.h',
    'stateless_ipc': 'daemon/stateless_ipc.h',
}

# Each target compiles one header in a fresh translation unit, preventing an
# earlier include from accidentally supplying a declaration needed later.
foreach name, header : public_header_checks + private_header_checks
    executable(
        'check_c_header_' + name,
        sources: ['header_compile.c'],
        include_directories: incdir,
        c_args: cflags + ['-DTEST_HEADER=<' + header + '>'],
    )
endforeach

if add_languages('cpp', native: false, required: false)
    foreach name, header : public_header_checks
        executable(
            'check_cpp_header_' + name,
            sources: ['header_compile.cpp'],
            include_directories: incdir,
            cpp_args: ['-DTEST_HEADER=<' + header + '>'],
            override_options: ['cpp_std=c++11'],
        )
    endforeach
endif

ipc_namespaces_test = executable(
    'test_ipc_namespaces',
    sources: ['test_ipc_namespaces.c'],
    include_directories: incdir,
    c_args: cflags,
)

test('IPC namespaces and wire compatibility', ipc_namespaces_test)

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

discovery_test = executable(
    'test_discovery',
    sources: discovery_core_sources
    + files(
        'discovery_fake_backend.c',
        'test_discovery.c',
    ),
    include_directories: incdir,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'Zeroconf discovery core',
    discovery_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

discover_command_test = executable(
    'test_discover_command',
    sources: discovery_core_sources
    + files(
        'discovery_command_fake_backend.c',
        'test_discover_command.c',
    ),
    include_directories: incdir,
    link_with: discovery_client_lib,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'afp_client discover output',
    discover_command_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

resolve_service_test = executable(
    'test_resolve_service',
    sources: discovery_core_sources
    + files(
        'discovery_service_fake_backend.c',
        'test_resolve_service.c',
    ),
    include_directories: incdir,
    link_with: discovery_client_lib,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'afp_client service resolution',
    resolve_service_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

cmdline_discover_test = executable(
    'test_cmdline_discover',
    sources: discovery_core_sources
    + files(
        '../cmdline/discover.c',
        'discovery_command_fake_backend.c',
        'test_cmdline_discover.c',
    ),
    include_directories: incdir,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'afpcmd service picker',
    cmdline_discover_test,
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
