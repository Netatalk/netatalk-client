# Installing Netatalk Client

This guide covers obtaining Netatalk Client and building, testing, installing, upgrading, and
uninstalling it from source. [Getting started](getting_started.html) covers using the installed programs.

With the requirements below in place, the procedure is:

    meson setup build -Dbuildtype=release
    meson compile -C build
    sudo meson install -C build

Netatalk Client builds on current versions of Linux, FreeBSD, NetBSD, OpenBSD, macOS, illumos, and Solaris.
The FUSE client is not available on illumos and Solaris.

## How to obtain Netatalk Client

### Release tarballs

The project publishes stable releases on the
[Netatalk Client releases page on GitHub](https://github.com/Netatalk/netatalk-client/releases).
The [Netatalk download page](https://netatalk.io/download.html) lists them as well.
Each release is a `netatalk-client-x.y.z.tar.xz` tarball with a `.sha256sum` checksum file
and an `.asc` GPG signature from the Netatalk project.

Verify the checksum and the signature, then unpack the sources:

    sha256sum -c netatalk-client-x.y.z.tar.xz.sha256sum
    gpg --keyserver keys.openpgp.org --recv-keys 7B13E1BFE4DDE8BD
    gpg --verify netatalk-client-x.y.z.tar.xz.asc netatalk-client-x.y.z.tar.xz
    tar xf netatalk-client-x.y.z.tar.xz
    cd netatalk-client-x.y.z

On macOS, use `shasum -a 256 -c` in place of `sha256sum -c`.
Compare the fingerprint gpg prints with the one on the download page.

### Git repository

To build the development version, clone the repository:

    git clone https://github.com/Netatalk/netatalk-client.git
    cd netatalk-client

The `main` branch is the development branch. A release tag carries the version number, for example `1.0.0`.

### Binary packages

Downstream packagers provide binary packages.
Look for a *netatalk-client* package in your operating system's package manager. MacPorts names it
*netatalk_client*. Without one, build from source.

## Requirements

### Build tools

The build needs these tools:

| Tool       | Notes                                                                              |
| ---------- | ---------------------------------------------------------------------------------- |
| C compiler | gcc or clang, for C11 sources                                                      |
| Meson      | 0.62 or later                                                                      |
| Ninja      | sometimes packaged as *ninja-build*                                                |
| pkg-config | pkg-config or pkgconf, used to find libraries                                      |
| Perl       | runs part of the test suite with its standard modules                              |

### Libraries

Each library below adds a component, and a build without it drops that component:

| Library                     | Needed for                                                   | Without it                              |
| --------------------------- | ------------------------------------------------------------ | --------------------------------------- |
| pthreads                    | all programs                                                 | the build fails                         |
| iconv                       | character set conversion, from the C library or *libiconv*   | the build fails                         |
| libgcrypt 1.2.3 or later    | the DHX, DHX2, Randnum, and SRP encrypted login methods      | only guest and cleartext logins         |
| libfuse 3.0+ or 2.9+        | the FUSE client: *afpfsd*, *mount_afpfs*, `afpc fs` commands | the FUSE client is not built            |
| readline or libedit         | the *afpcmd* command line client                             | *afpcmd* is not built                   |
| avahi-client or DNS-SD      | Zeroconf discovery: `afpc discover`, `afpcmd --browse`       | discovery reports "not supported"       |
| libbsd                      | Linux with glibc older than 2.38                             | the build fails on those systems        |

The build prefers FUSE 3 when both versions are present.

### Platform notes

Install commands and configure arguments by platform:

#### Alpine Linux

    apk add avahi-dev build-base fuse3-dev libedit-dev libgcrypt-dev meson ninja perl

Install *fuse-dev* in place of *fuse3-dev* for FUSE 2.

#### Debian and Ubuntu

    apt install gcc libavahi-client-dev libfuse3-dev libgcrypt-dev libreadline-dev meson ninja-build perl pkg-config

Install *libfuse-dev* in place of *libfuse3-dev* for FUSE 2.
Add *libbsd-dev* where the Libraries table requires libbsd, such as on Ubuntu 22.04.
Ubuntu 22.04 also ships Meson 0.61, older than the build requires. Install a current Meson with pipx and put
its directory, `~/.local/bin`, ahead of `/usr/bin` in your `PATH`:

    apt install pipx
    pipx install meson
    pipx ensurepath

sudo does not use your `PATH`, so install with `sudo ~/.local/bin/meson install -C build` in place of the
install command below.

#### Fedora

    dnf install avahi-devel fuse3-devel gcc libgcrypt-devel meson ninja-build perl-File-Temp perl-Test-Simple \
        perl-interpreter readline-devel

Install *fuse-devel* in place of *fuse3-devel* for FUSE 2.

#### FreeBSD

    pkg install avahi fusefs-libs3 libedit libgcrypt libiconv meson perl5 pkgconf

Install *fusefs-libs* in place of *fusefs-libs3* for FUSE 2.
Configure with `-Dpkg_config_path=/usr/local/libdata/pkgconfig`, the pkg-config directory of the ports tree.

#### NetBSD

    pkg_add fuse gcc13 libgcrypt meson perl pkg-config

#### OpenBSD

    pkg_add avahi libgcrypt meson

The base system provides the remaining tools.

#### macOS with Homebrew

    brew install libgcrypt macfuse meson pkgconf

The macfuse cask runs the official installer. Allow its kernel extension as
[the macFUSE website](https://macfuse.github.io/) describes. Install macFUSE 5.1.3 or later for the FUSE client.
macFUSE 5.1.2 and earlier cannot write extended attributes, so copying files to a FUSE mount in the Finder fails.

The FUSE library must be macFUSE. FUSE-T, the alternative without a kernel extension, reports version 1.2.9
in its `fuse3.pc`, below the 3.0 the build requires. The [configure summary](#configuring-the-build) then
shows *AFP FUSE client: NO*.

#### macOS with MacPorts

    sudo port install libedit libgcrypt libiconv macfuse +fs_link meson ninja pkgconfig

The `+fs_link` variant creates the `/Library/Filesystems` link that macFUSE needs to mount. The macFUSE notes
under macOS with Homebrew apply here as well. MacPorts installs under `/opt/local`, which
Apple's compiler and a non-MacPorts pkg-config do not search by default. Configure with the MacPorts
directories in the environment and install into the same prefix:

    PKG_CONFIG_PATH=/opt/local/lib/pkgconfig:/opt/local/share/pkgconfig \
    CFLAGS="-I/opt/local/include/fuse3 -I/opt/local/include" LDFLAGS=-L/opt/local/lib \
    meson setup build --prefix=/opt/local -Dbuildtype=release

The FUSE 3 include directory must come first, because the top-level `fuse.h` in `/opt/local/include`
is a FUSE 2 compatibility header. Leave the `fuse3` directory out when building against FUSE 2.

The official macFUSE installer uses `/usr/local` on every Mac, and MacPorts' pkg-config searches only
`/opt/local`. A build with that pkg-config therefore reports the FUSE client as *NO* with macFUSE installed
and working. Add `-Dpkg_config_path=/usr/local/lib/pkgconfig` to the configure command, or to a reconfigure
of an existing build directory:

    meson setup --reconfigure build -Dpkg_config_path=/usr/local/lib/pkgconfig

Homebrew's pkgconf searches `/usr/local/lib/pkgconfig` by default and needs no such option.

#### illumos and Solaris

The FUSE client is not available on OmniOS, OpenIndiana, and Solaris 11. Pass `-Denable-fuse=false` to leave
it out explicitly.

On OpenIndiana, install the build tools and libraries, then configure with
`-Denable-fuse=false -Dpkg_config_path=/usr/lib/amd64/pkgconfig`:

    pkg install developer/build/meson developer/build/ninja developer/build/pkg-config developer/gcc-14 \
        library/libedit system/library/security/libgcrypt

On Solaris 11, configure with `--prefix=/usr/local -Denable-fuse=false -Dpkg_config_path=/usr/lib/amd64/pkgconfig`:

    pkg install gcc library/libedit libgcrypt meson ninja pkg-config

On OmniOS, the libraries come from pkgsrc under `/opt/local`. Configure with
`--prefix=/opt/local -Denable-fuse=false -Dpkg_config_path=/opt/local/lib/pkgconfig`.

## Configuring the build

Meson builds out of tree, in a separate build directory. The commands below use a build directory called
`build` inside the source tree. Any directory works, and a separate directory per configuration keeps a
release build and a debug build apart. From the top level source directory, run:

    meson setup build -Dbuildtype=release

The default build type, `debug`, compiles with `-O0` and debug symbols. Add the arguments and environment
given for your platform above. Build options below covers an existing build directory.

The configure step ends with a summary whose *Options* section marks each component as YES or NO, so read it
before building. The build drops a component whose library it did not find, without an error.
[When a component is missing](#when-a-component-is-missing) covers a *NO* you did not expect.

### Build options

Pass options to `meson setup` as `-Doption=value`, and `--prefix=DIR` as the usual spelling of `-Dprefix=DIR`.
To change an option on an existing build directory, run `meson setup --reconfigure build -Doption=value` and
build again. On Linux, a prefix outside the dynamic linker's search path needs the ldconfig step under
Installing.

| Option                      | Default      | Effect                                                                  |
| --------------------------- | ------------ | ----------------------------------------------------------------------- |
| `prefix`                    | `/usr/local` | installation prefix                                                     |
| `buildtype`                 | `debug`      | `release` builds with `-O3` and without debug symbols                   |
| `enable-fuse`               | `true`       | build the FUSE client when a FUSE library exists                        |
| `force-fuse-v2`             | `false`      | build against FUSE 2 even with FUSE 3 present                           |
| `zeroconf`                  | `auto`       | `enabled` fails the configure step without a backend, `disabled` omits  |
| `zeroconf-backend`          | `auto`       | `avahi` or `dnssd`, `auto` picks Avahi, or DNS-SD on macOS              |
| `enable-docs`               | `true`       | install the manual pages and these guides                               |
| `with-testsuite`            | `false`      | build the *vfs_lantest* and *vfs_speedtest* benchmark tools, Linux only |
| `pkg_config_path`           | empty        | additional pkg-config search directories                                |
| `werror`                    | `true`       | treat compiler warnings as errors                                       |

Beyond this selection, `meson configure` run from the source directory prints every option the project offers
with its default. `meson configure build` prints the values set in an existing build directory.

### When a component is missing

The summary line *AFP FUSE client: NO* means the configure step found no usable FUSE library. An installed
*afpc* then prints `afpc: the 'fs' namespace is unavailable because FUSE support was not built`. The same
applies to *afpcmd* without readline or libedit, to the encrypted login methods without libgcrypt, and to
Zeroconf discovery without Avahi or DNS-SD. Passing tests prove nothing here, because the test suite covers
whatever the build produced.

First find out why. `build/meson-logs/meson-log.txt` records every dependency lookup with its outcome, for
example `Run-time dependency fuse3 found: NO found 1.2.9 but need: '>=3.0.0'`. `pkg-config --modversion fuse3`
and `pkg-config --modversion fuse` show what pkg-config offers against the versions in [Libraries](#libraries).

Then install the missing library with its development headers. [Libraries](#libraries) names the libraries.
[Platform notes](#platform-notes) gives the package names. A library whose `.pc` file sits in a directory
pkg-config does not search needs `-Dpkg_config_path=DIR` on the reconfigure below. Meson reads the
`PKG_CONFIG_PATH` environment variable only at the first configuration of a build directory.

`pkg-config --variable=pc_path pkg-config` prints the directories it searches by default. macFUSE with
MacPorts' pkg-config is the common case, covered under [macOS with MacPorts](#macos-with-macports).

Finally reconfigure. `meson compile` alone does not look for libraries again unless a build file changed, so
after installing a library, run:

    meson setup --reconfigure build

Configuring again repeats the lookups that failed and prints a new summary. Read it, then build and install.
Installing over the previous installation replaces the programs that lacked the component.
The [Meson documentation](https://mesonbuild.com/Commands.html) describes the commands used here.

## Building and testing

Compile, then run the unit tests:

    meson compile -C build
    meson test -C build

The unit tests need neither root privileges nor an AFP server. They write their log to
`build/meson-logs/testlog.txt`. [Benchmarking a mounted filesystem](benchmarks.html) describes the benchmark
tools that `-Dwith-testsuite=true` builds.

## Installing

Install with sudo, or as root on systems without it:

    sudo meson install -C build

Installation places the files below under the prefix and skips the components the build did not produce.

| Location                     | Files                                                                           |
| ---------------------------- | ------------------------------------------------------------------------------- |
| `bin/`                       | *afpc*, *afpcmd*, *afpfsd*, *afpgetstatus*, *afpsld*, *mount_afpfs*             |
| `bin/`                       | *vfs_lantest* and *vfs_speedtest* when built with `-Dwith-testsuite=true`       |
| `lib/`                       | the *libafpclient* and *libafpsl* shared libraries                              |
| `lib/pkgconfig/`             | `libafpclient.pc` and `libafpsl.pc`                                             |
| `include/netatalk-client/`   | the public headers                                                              |
| `share/man/man1/`            | the manual pages of the installed programs                                      |
| `share/doc/netatalk-client/` | AUTHORS, COPYING, the NEWS and README files, and these guides                   |

*mount_afpfs* is a symbolic link to *afpc*. On systems with a multiarch library layout, such as Debian, the
library directory is `lib/x86_64-linux-gnu/` or similar. Installation writes only under the prefix: no
configuration files, system services, or setuid programs.

To install into a staging directory, for example when building a package, pass `--destdir`. A root install
leaves the install log root-owned, so use a build directory without one:

    meson install -C build --destdir /tmp/netatalk-client-stage

Verify the installation by running `afpc version`, which every build installs. `afpcmd -h` and `afpc discover`
work when the configure summary listed the command line client and Zeroconf discovery. Mounting with the
FUSE client requires access to `/dev/fuse`. [Getting started](getting_started.html) covers the FUSE
configuration.

On Linux, a program that fails to start because `libafpsl.so.1` or `libafpclient.so.1` is missing has its
libraries in a directory the dynamic linker does not search. Add the library directory from the configure
summary to `/etc/ld.so.conf.d/` and run `sudo ldconfig`, or configure with `-Dprefix=/usr`.

## Rebuilding and upgrading

Build and install again after you pull new sources or edit files:

    meson compile -C build
    sudo meson install -C build

Ninja rebuilds only what changed and re-runs the configure step when a build file changed. A library
installed after the first configure run needs the reconfigure under
[When a component is missing](#when-a-component-is-missing).

Meson caches successful lookups and the results of its compiler probes across reconfigurations. A removed,
upgraded, or moved library therefore still counts as found, and a header that appeared later still counts as
missing. Wipe the build directory in that case. Meson configures it again with the same options:

    meson setup --wipe build

Removing the directory and running `meson setup` again with the same options gives the same result. Both
discard `build/meson-logs/install-log.txt`, so uninstall first when you installed from that directory.

To clean and rebuild everything while keeping the configuration:

    meson compile -C build --clean
    meson compile -C build

Before installing a new version over a running one, every user with AFP mounts or running daemons unmounts
and stops them. The replaced programs and libraries must not be in use:

    afpc fs unmount /path/to/mountpoint
    afpc fs exit
    afpc sl exit

Then install. Installing over a previous version replaces its files in place and removes nothing. Uninstall
the previous version first when it came from a different build directory or prefix. Do the same when a
component drops out of the build, for example when you disable the FUSE client.

## Uninstalling

Unmount every AFP mount and stop the daemons with the commands in the previous section. Then run the
uninstall target from the build directory that installed the files:

    sudo ninja -C build uninstall

This removes every file the installation created, including the *mount_afpfs* link. Uninstalling leaves your
own files alone, such as entries in `/etc/fstab` or `/etc/fuse.conf`. It also leaves the AppleDouble metadata
files that *afpcmd* writes next to transferred files in some metadata modes.

`build/meson-logs/install-log.txt` lists the installed files, and the uninstall target removes exactly those.
Every install rewrites the log. An install into a staging directory with `--destdir` therefore lists the
staged paths instead. Run `sudo meson install -C build` again to restore the list before uninstalling. Without the
build directory, remove the files under the locations in the installation table by hand. Or configure and
install again with the same options and uninstall from the new build directory.

The package manager removes an installation made from a binary package.

## Development files

The headers and pkg-config files listed under Installing serve both library layers:

- `pkg-config --cflags --libs libafpsl` for daemon-backed filesystem operations
- `pkg-config --cflags --libs libafpclient` for the opaque stateful transport API

The build does not install the libafpclient implementation headers from the source tree's `lib/` directory.
[Developer documentation](developer.html) describes the APIs.

## Next steps

[Getting started](getting_started.html) explains how to mount a volume with the FUSE client and how to use the
command line client. [Features and compatibility](features.html) lists what works with which servers.
[Reporting bugs](reporting_bugs.html) describes what to include in a bug report.
