# Installation Instructions

This is a quick guide on how to install Netatalk Client.

Netatalk Client has been tested on many Linux distros, FreeBSD, NetBSD, OpenBSD, and macOS.

## Requirements

### Build tools

First off you need standard build tools: a C compiler (gcc/clang) and a build system.
This project uses the Meson build system with a Ninja backend.
Make sure *meson* and *ninja* (sometimes packaged as *ninja-build*) are installed.
You also want *pkg-config* that is used by the build system to find libraries.

### Libraries

| Dependency | Needed for                                                |
| ---------- | --------------------------------------------------------- |
| pthread    | mandatory on all platforms                                |
| libgcrypt  | encrypted UAMs                                            |
| readline   | afpcmd client                                             |
| libedit    | alternative to *readline* on f.e. Alpine Linux or FreeBSD |
| libfuse    | FUSE client; v3 recommended, compatible with v2.9         |
| libbsd     | mandatory on Linux when glibc < 2.38                      |

#### macOS

On macOS, macFUSE (5.1.3 or later) is recommended for the FUSE client.

macFUSE can be installed from [https://macfuse.github.io/](the macFUSE website) or via Homebrew.
Follow the instructions to install the macFUSE software and kernel extension.

Note that macFUSE 5.1.2 and earlier has a bug that prevents writing extended attributes,
which can lead to errors when copying files to the FUSE mount in the Finder.

## Compile and install

From the top level source directory, run:

    meson setup build -Dbuildtype=release

Build and install the software.

    meson compile -C build
    sudo meson install -C build

To see available options, run:

    meson configure

## Development files

The build system installs namespaced API headers under `netatalk-client/` and
pkg-config metadata for both supported library layers:

- `pkg-config --cflags --libs libafpsl` for daemon-backed filesystem operations
- `pkg-config --cflags --libs libafpclient` for the opaque stateful transport API

Concrete libafpclient implementation headers under `lib/` are not installed.
