fork_locking_test = executable(
    'test_fork_locking',
    sources: files(
        'afp_fake_server.c',
        'test_fork_locking.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'AFP fork locking',
    fork_locking_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

connect_test = executable(
    'test_connect',
    sources: files(
        'afp_fake_server.c',
        'test_connect.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'server connect and status',
    connect_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

reconnect_test = executable(
    'test_reconnect',
    sources: files(
        'afp_fake_server.c',
        'test_reconnect.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'reconnect into the session',
    reconnect_test,
    args: ['--tap'],
    protocol: 'tap',
    timeout: 90,
    verbose: true,
)

copy_range_test = executable(
    'test_copy_range',
    sources: files(
        'afp_fake_server.c',
        'test_copy_range.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'copy limit',
    copy_range_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

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
    'path': 'netatalk-client/path.h',
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

afpc_parser_test = files('check_afpc.t')
test(
    'afpc parser and invocation dispatch',
    find_program('perl'),
    args: [afpc_parser_test, afpc_exe.full_path(), with_fuse ? 'fuse' : 'no-fuse'],
    depends: afpc_exe,
    protocol: 'tap',
    verbose: true,
)

transport_api_test = executable(
    'test_transport_api',
    sources: ['test_transport_api.c'],
    include_directories: incdir,
    link_with: libafpclient,
)

test('stateful transport public API', transport_api_test)

transport_wait_test = executable(
    'test_transport_wait',
    sources: files(
        'afp_fake_server.c',
        'test_transport_wait.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'a raw command waits as its timeout says',
    transport_wait_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

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
    'afpc discover output',
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
    'afpc service resolution',
    resolve_service_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

cmdline_discover_test = executable(
    'test_cmdline_discover',
    sources: discovery_core_sources
    + files(
        '../../cmdline/discover.c',
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
    include_directories: [incdir, include_directories('../../daemon')],
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

# A misaligned load or store, or a null a declaration forbids, traps with no
# runtime to link; the tests check more, so a compiler without the flags
# builds them without.
trap_args = [
    '-fsanitize=alignment,nonnull-attribute',
    '-fsanitize-undefined-trap-on-error',
]
traps = cc.has_multi_arguments(trap_args)

protocol_replies_test = executable(
    'test_protocol_replies',
    sources: files(
        '../../lib/proto_volume.c',
        'test_protocol_replies.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: traps ? cflags + trap_args : cflags,
)

test(
    'AFP protocol reply parsing',
    protocol_replies_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

filename_validation_test = executable(
    'test_filename_validation',
    sources: ['test_filename_validation.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'AFP filename validation',
    filename_validation_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

write_frames_test = executable(
    'test_write_frames',
    sources: files(
        '../../lib/proto_attr.c',
        '../../lib/proto_files.c',
        '../../lib/proto_login.c',
        '../../lib/proto_server.c',
        'test_write_frames.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: traps ? cflags + trap_args : cflags,
)

test(
    'a command hands the send its frame',
    write_frames_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

unaligned_lengths_test = executable(
    'test_unaligned_lengths',
    sources: files(
        '../../lib/dsi.c',
        '../../lib/utils.c',
        'test_unaligned_lengths.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: traps ? cflags + trap_args : cflags,
)

test(
    'a length is read and written at any alignment',
    unaligned_lengths_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

path_conversion_allocs_test = executable(
    'test_path_conversion_allocs',
    sources: files(
        '../../lib/codepage.c',
        '../../lib/unicode.c',
        'test_path_conversion_allocs.c',
    ),
    include_directories: incdir,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'path conversion allocations',
    path_conversion_allocs_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

fork_io_test = executable(
    'test_fork_io',
    sources: ['test_fork_io.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'AFP fork I/O',
    fork_io_test,
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

fuse_error_test = executable(
    'test_fuse_error',
    sources: ['test_fuse_error.c', '../../fuse/fuse_error.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'libfuse start errors',
    fuse_error_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

fuse_ipc_test = executable(
    'test_fuse_ipc',
    sources: ['test_fuse_ipc.c', '../../fuse/fuse_ipc.c'],
    include_directories: incdir,
    c_args: cflags,
)

test(
    'mount daemon command checks',
    fuse_ipc_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

test(
    'log message escaping',
    log_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

afp_version_test = executable(
    'test_afp_version',
    sources: ['test_afp_version.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'AFP version parsing',
    afp_version_test,
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
    include_directories: [incdir, include_directories('../../daemon')],
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

dsi_recv_test = executable(
    'test_dsi_recv',
    sources: ['test_dsi_recv.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'DSI receive path',
    dsi_recv_test,
    args: ['--tap'],
    protocol: 'tap',
    timeout: 60,
    verbose: true,
)

status_text_test = executable(
    'test_status_text',
    sources: ['test_status_text.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'server status text',
    status_text_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

appledouble_paths_test = executable(
    'test_appledouble_paths',
    sources: ['test_appledouble_paths.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'AppleDouble path translation',
    appledouble_paths_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

send_failures_test = executable(
    'test_send_failures',
    sources: ['test_send_failures.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'a failed send is a failure',
    send_failures_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

fork_records_test = executable(
    'test_fork_records',
    sources: files(
        'afp_fake_server.c',
        'test_fork_records.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'open-fork records',
    fork_records_test,
    args: ['--tap'],
    # Bound at load: lazy binding would run on the stack a case dirties.
    env: ['LD_BIND_NOW=1', 'DYLD_BIND_AT_LAUNCH=1'],
    protocol: 'tap',
    verbose: true,
)

listing_test = executable(
    'test_listing',
    sources: ['test_listing.c'],
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: root_dependencies,
    c_args: cflags,
)

test(
    'directory listings',
    listing_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

dir_cache_test = executable(
    'test_dir_cache',
    sources: files(
        'afp_fake_server.c',
        'test_dir_cache.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'directory cache',
    dir_cache_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

dir_cache_requests_test = executable(
    'test_dir_cache_requests',
    sources: files(
        'afp_fake_server.c',
        'test_dir_cache_requests.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'directory cache requests',
    dir_cache_requests_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)

metadata_requests_test = executable(
    'test_metadata_requests',
    sources: files(
        'afp_fake_server.c',
        'test_metadata_requests.c',
    ),
    include_directories: incdir,
    link_with: libafpclient,
    dependencies: [root_dependencies, pthread_dep],
    c_args: cflags,
)

test(
    'create, truncate and metadata requests',
    metadata_requests_test,
    args: ['--tap'],
    protocol: 'tap',
    verbose: true,
)
