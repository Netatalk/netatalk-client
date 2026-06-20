project(
    'netatalk-client',
    'c',
    version: '0.9.5',
    license: 'GPLv2',
    default_options: ['warning_level=3', 'c_std=c11'],
    meson_version: '>=0.62',
)

cc = meson.get_compiler('c')
host_os = host_machine.system()
build_type = get_option('buildtype')
afpfsng_version = meson.project_version()
pkg = import('pkgconfig')

install_prefix = get_option('prefix')
bindir = install_prefix / get_option('bindir')
libdir = install_prefix / get_option('libdir')
mandir = install_prefix / get_option('mandir')
datadir = install_prefix / get_option('datadir')
includedir = install_prefix / get_option('includedir')

libsearch_dirs = []
include_dirs = []
cflags = [
    '-DAFPFS_VERSION="' + afpfsng_version + '"',
    '-DBINDIR="' + bindir + '"',
    '-D_FILE_OFFSET_BITS=64',
]

if 'afp' in get_option('enable-debugging')
    cflags += '-DDEBUG_AFP_LOOP'
endif

if 'dsi' in get_option('enable-debugging')
    cflags += '-DDEBUG_DSI'
endif

if host_os == 'linux'
    cflags += '-D_GNU_SOURCE'
elif host_os in ['dragonfly', 'freebsd', 'openbsd']
    libsearch_dirs += '/usr/local/lib'
    include_dirs += ['/usr/local/include']
    cflags += '-DLIBICONV_PLUG'
elif host_os == 'netbsd'
    libsearch_dirs += '/usr/pkg/lib'
    include_dirs += ['/usr/pkg/include']
endif

readline_dep = cc.find_library('readline', dirs: libsearch_dirs, required: false)
editline_dep = dependency('libedit', required: false)
pthread_dep = cc.find_library('pthread', dirs: libsearch_dirs, required: true)
libiconv_dep = cc.find_library('iconv', dirs: libsearch_dirs, required: false)
libbsd_dep = cc.find_library('bsd', dirs: libsearch_dirs, required: false)

root_dependencies = []
cmdline_dependencies = [readline_dep]

if editline_dep.found()
    cmdline_dependencies += editline_dep
    cflags += '-DHAVE_LIBEDIT'
elif readline_dep.found()
    cmdline_dependencies += readline_dep
    cflags += '-DHAVE_LIBREADLINE'
endif

if libbsd_dep.found()
    root_dependencies += libbsd_dep
    cflags += '-DHAVE_LIBBSD'
endif

if cc.has_function('explicit_bzero')
    cflags += '-DHAVE_EXPLICIT_BZERO'
elif cc.has_function('memset_explicit')
    cflags += '-DHAVE_MEMSET_EXPLICIT'
endif

gcrypt_dep = dependency('libgcrypt', version: '>=1.2.3', required: false)
fuse_dep = dependency('fuse3', version: '>=3.0.0', required: false)
if not fuse_dep.found() or get_option('force-fuse-v2')
    fuse_dep = dependency('fuse', version: '>=2.9.0', required: false)
endif

with_afpcmd = readline_dep.found() or editline_dep.found()
with_fuse = get_option('enable-fuse') and fuse_dep.found()
with_crypt = gcrypt_dep.found()

# Check for extended attribute headers (platform-dependent)
if cc.has_header('sys/xattr.h')
    cflags += '-DHAVE_SYS_XATTR_H'
endif
if cc.has_header('sys/extattr.h')
    cflags += '-DHAVE_SYS_EXTATTR_H'
endif
if cc.has_header('attr/xattr.h')
    cflags += '-DHAVE_ATTR_XATTR_H'
endif

if with_fuse
    fuse_version = fuse_dep.version()
    if fuse_version.version_compare('>=3.0.0')
        cflags += [
            '-DHAVE_LIBFUSE',
            '-DFUSE_USE_VERSION=35',
        ]
    else
        cflags += [
            '-DHAVE_LIBFUSE',
            '-DFUSE_USE_VERSION=29',
        ]
    endif

    # Detect FUSE capability: new readdir API with fuse_readdir_flags
    # This tests if enum fuse_readdir_flags type exists (FUSE 3.3+)
    fuse_readdir_flags_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>
        enum fuse_readdir_flags flags;
    ''',
        dependencies: fuse_dep,
        name: 'FUSE enum fuse_readdir_flags',
    )

    # Detect FUSE capability: new function signatures with struct fuse_file_info *fi parameter
    # This tests if a function with the new signature can be defined and used
    fuse_file_info_ops_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>

        int chown_func(const char *path, uid_t uid, gid_t gid,
                       struct fuse_file_info *fi) { return 0; }
        int chmod_func(const char *path, mode_t mode,
                       struct fuse_file_info *fi) { return 0; }
        int truncate_func(const char *path, off_t offset,
                          struct fuse_file_info *fi) { return 0; }
    ''',
        dependencies: fuse_dep,
        name: 'FUSE new function signatures with file_info',
    )

    # Detect FUSE capability: new rename API with flags parameter
    fuse_rename_flags_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>
        int rename_func(const char *from, const char *to,
                        unsigned int flags) { return 0; }
    ''',
        dependencies: fuse_dep,
        name: 'FUSE rename with flags parameter',
    )

    # Detect FUSE capability: new utimens API with file_info parameter
    fuse_utimens_fi_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>
        int utimens_func(const char *path, const struct timespec tv[2],
                         struct fuse_file_info *fi) { return 0; }
    ''',
        dependencies: fuse_dep,
        name: 'FUSE utimens with file_info parameter',
    )

    # Define FUSE_NEW_API if we have the new API capabilities
    # We consider it "new API" if all the new signatures are supported
    if fuse_readdir_flags_check and fuse_file_info_ops_check and fuse_rename_flags_check and fuse_utimens_fi_check
        cflags += '-DFUSE_NEW_API=1'
    else
        cflags += '-DFUSE_NEW_API=0'
    endif
endif

incdir = include_directories(['.', 'include', include_dirs])

if with_crypt
    add_project_arguments('-DHAVE_LIBGCRYPT', language: 'c')
endif

if not with_afpcmd and not with_fuse
    error(
        'At least one of AFP command line client or FUSE client must be buildable.',
    )
endif

subdir('lib')
subdir('daemon')
subdir('cmdline')
subdir('test')

if with_fuse
    subdir('fuse')
endif

subdir('include')

if get_option('enable-docs')
    subdir('docs')

    install_data(
        ['AUTHORS', 'COPYING', 'NEWS.md', 'README.md'],
        install_dir: datadir / 'doc' / 'netatalk-client',
    )
endif

summary_info = {
    'Build directory': meson.current_build_dir(),
    'Source path': meson.current_source_dir(),
}
summary(summary_info, bool_yn: true, section: 'Build environment:')

summary_info = {
    'Installation prefix': install_prefix,
    'Executable directory': bindir,
    'Library directory': libdir,
    'Manual page directory': mandir,
}
summary(summary_info, bool_yn: true, section: 'Directories:')

summary_info = {
    'Host CPU': host_machine.cpu_family(),
    'Host endianness': build_machine.endian(),
    'C compiler': cc.get_id(),
    'Build stype': build_type,
    'Shared or static libraries': get_option('default_library'),
}
summary(summary_info, bool_yn: true, section: 'Compilation:')

summary_info = {
    'AFP command line client': with_afpcmd,
    'AFP FUSE client': with_fuse,
    'Encrypted UAMs': with_crypt,
}
summary(summary_info, bool_yn: true, section: 'Options:')
