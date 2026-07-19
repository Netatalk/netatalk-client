discovery_backend_source = 'backend_' + zeroconf_backend + '.c'

discovery_core_sources = files('discovery.c')

discovery_lib = static_library(
    'afpc-discovery',
    sources: discovery_core_sources + files(discovery_backend_source),
    dependencies: [root_dependencies, zeroconf_dependencies],
    include_directories: incdir,
    c_args: cflags + zeroconf_cargs,
    install: false,
)

discovery_dep = declare_dependency(
    link_with: discovery_lib,
    dependencies: zeroconf_dependencies,
    include_directories: incdir,
)

# Higher-level discovery behavior shared by the standalone and FUSE clients.
# Keep the provider implementation separate so tests can link this archive
# against their fake discovery backends.
discovery_client_lib = static_library(
    'afpc-client-discovery',
    sources: files('client/discover.c'),
    dependencies: root_dependencies,
    include_directories: incdir,
    c_args: cflags,
    install: false,
)

discovery_client_dep = declare_dependency(
    link_with: discovery_client_lib,
    dependencies: discovery_dep,
    include_directories: incdir,
)
