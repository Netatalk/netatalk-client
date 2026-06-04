# NEWS

## afpfs-ng 0.9.4 (February 28, 2026)

### New Test Suite

- A new test suite for `afpcmd` and `afpgetstatus` has been added, with tests for both batch and interactive modes of
  `afpcmd`. The tests are designed to be run in a self-contained container environment, but can also be run on the host
  with the appropriate setup. They use Perl's `Test::More` framework and are run with the `prove` tool. This is now
  running in CI on every commit and pull request.

### AFP Client Library Improvements

- Turn `afpfsd.h` into a shim and create clean Stateless/FUSE separation, with shared IPC constants moved to
  `afp_ipc.h`, the stateless daemon structs moved to `afp_server.h`, and the FUSE-specific constants to `fuse_ipc.h`.
- MacRoman code page conversion support for better compatibility with AFP 2.x servers (Classic Mac OS).
- Separate the `mounted` and `attached` semantics, which allows for better error handling and more intuitive behavior in
  the stateless client and `afpcmd`.
- Serialize all AFP server operations to prevent concurrent DSI calls.
- Prevent `SIGPIPE` from propagating to clients, with cleaner error handling.

### Stateless Client Improvements

- Proper daemonization when `afpsld` is auto-spawned by the client library, which is required by the `kio-afp` client on
  KDE and any other client that launches in the background.
- Count client connections and shut down the stateless client daemon (`afpsld`) when no clients are connected.
- Support for "not empty directory" errors when trying to delete directories with the `rmdir` command in `afpcmd`.
- Fix numerous race conditions and other bugs that could cause the daemon to crash or misbehave under load.

### afpcmd Client Improvements

- Renamed `view` to `cat` and `pass` to `passwd` for better consistency with common command line tools.
- Graceful handling of server disconnects and other errors in interactive mode, with the ability to reconnect to the
  same volume or return to the volume selection menu.
- Do not tear down the server connection with the `exit` command in interactive mode, which allows other clients to
  continue using the same connection.
- Reintroduced the `disconnect` command in interactive mode, but with a new behavior that shuts down the AFP server
  connection and exits the client, rather than just detaching from the volume. This interrupts other clients using the
  same server connection.
- Less buggy readline support for interactive mode on *BSD and macOS, with better handling of escaped spaces.
- When no password is provided in the AFP URL, prompt the user for it.

### FUSE Client Improvements

- Correctly store time stamps internally and convey them to libfuse, notably to not break file creation times after
  copying files with xattr with macFUSE on macOS.
- Cleaner handling of server disconnects and other errors.
- Prepend `afp://` to URLs passed to `mount_afpfs`, if missing.
- When no password is provided in the AFP URL, prompt the user for it.

### afpgetstatus Improvements

- Attempt to authenticate with guest UAM and list visible volumes.
- Support for setting the log level, be sensitive to the log level coming from the client library.
- Handle naked IPv6 addresses in `afpgetstatus` arguments.
- Shut down on `SIGINT`.

### Other Fixes and Improvements

- Added a `-Denable-docs` option that allows opting out of installing documentation (enabled by default).
- Sundry buffer overflow and memory safety fixes in various places.

## afpfs-ng 0.9.3 (February 8, 2026)

### Stateless Client Library

- The stateless client library (`libafpsl.so`) together with the stateless client daemon (`afpsld`) have been
  (re)introduced and thoroughly reworked to provide a more robust and efficient way to interact with AFP servers without
  maintaining a persistent connection.
- The stateless client daemon has gotten a unique name (`afpsld`) to allow it to coexist with the FUSE client daemon
  (`afpfsd`), while removing all FUSE client-specific code and dependencies from the former.

### afpcmd Client Improvements

- The `afpcmd` client has been reworked to use the new stateless client library and daemon, rather than calling the
  midlevel API directly. This allows for better error handling and stability, and also turns `afpcmd` into a reference
  implementation and showcase for the stateless client library.
- Implemented password changing with all UAMs that support it, using the previously ineffective `pass` command.
- Two-way batch file transfers from the command line.
- Introduced support for password-protected volumes.
- Introduced verbose mode with `-V` or `--verbose` that prints detailed information about file transfers, while adding a
  summary of the transfer at the end.
- Removed the ineffectual `user` command for changing the user on the fly.
- Removed "synonyms" for commands (e.g. `del` for `rm`) to reduce the clutter and complexity of the command set.

### FUSE Client Improvements

- Added support for libfuse 2.9 with older versions of macFUSE / OSXFUSE on macOS (although not fully tested).
- Made the client sensitive to loglevels set by the `afpfsd` manager daemon.
- Remapped `afp_client` volpass option from `-V` to `-P` to avoid overlap with the new verbose mode in `afpcmd`.

### Library Improvements

- Fixed bug that prevented the use of 8-char passwords with the Randnum UAMs.
- Scrub all passwords from memory immediately after use.
- Fallback to guest (`nobody`) auth if no credentials are provided, and the server allows guest access.
- Full implementation of Precomposed UTF-8 to Decomposed UTF-8 conversion.
- Changed the UAM shorthand for 2-Way Randnum from `2wayrandnum` to `randnum2`.
- Removed unused GNU GMP and ncurses dependencies.
- Many memory safety and reliability improvements.

## afpfs-ng 0.9.2 (January 14, 2026)

### FUSE Client Improvements

- Rearchitected to a one-`afpfsd`-process-per-mount model for better stability and performance: launch the manager
  `afpfsd` process with `afpfsd --manager` and mount daemons with `afpfsd --socket-id <path>`. As before, `mount_afpfs`
  or `afp_client` will launch all daemons automatically. This also enables multiple mounts on macOS with macFUSE, which
  was not possible before.
- Server connection suspend and resume support in `afp_client`.
- Can now set log level and log type (syslog/stdout) for the FUSE client.
- macOS Finder xattr support for custom icons and other metadata (requires macFUSE v5.1.3 or later).

### afpcmd Client Improvements

- Added a new `exit` command that detaches from the current volume and returns to the list of volumes.
- When in volume selection mode, use the `ls` command to list available volumes on the server.
- Made the `chmod` command work properly.
- Can now set log level for the `afpcmd` client (always logs to syslog).

### AFP Protocol Compliance

- AFP 3.2 Extended Attributes (xattr) support.
- AFP 3.3 Replay Cache support.
- AFP 3.4 compliant xattr error handling.
- Implemented FPEnumerateExt (command 66) for AFP 3.0.

### General Improvements

- Removed obsolete workarounds for 20-year-old netatalk bugs.
- Proper support for long UTF-8 volume names in all clients.
- Introduced UAM shorthands for easier use: `clrtxt`, `randnum`, `dhx2`, `dhx`, etc.
- Converted man pages to mdoc format.
- We now install development files (headers, pkg-config) for libafpclient with Meson.

## afpfs-ng 0.9.1 (December 7, 2025)

### FUSE Client Improvements

- Support for FUSE 3.0+ on Linux and FreeBSD, and macFUSE on macOS, while maintaining compatibility with FUSE 2.9.9.
- Implemented FUSE commands for create, flush, chown, truncate, and rename.
- Enabled multi-threaded FUSE operation handling for better performance and stability, while introducing better thread
  safety across the board.
- Volume name labels and proper volume creation time support in macFUSE.
- On FreeBSD, `afpfsd` can now be started on the fly by the `mount_afpfs` tool.
- Numerous bug fixes and stability improvements.

### afpcmd Client Improvements

- Introduced a new copy (`cp`) command for file transfers on the AFP volume.
- Shored up error handling for the `cd`, `mv`, `put`, and `touch` commands.
- Now mapping error codes to human-readable strings for better user feedback.
- Fixed bug with 8-char passwords when using Cleartxt Password UAM.
- The `dir`/`ls` command now takes an optional dir to list instead of the current directory.

### Meson Build System Enhancements

- Support for libedit alongside readline for `afpcmd` builds, commonly used on Alpine Linux and elsewhere.
- Always build `afpgetstatus` tool even without ncurses/readline.
- Fail the build if neither `afpcmd` nor the FUSE client can be built.

### Miscellaneous Improvements

- Introduced stylistic coding standards for C, Meson, Markdown, YAML, and shell script code. The new `codefmt.sh` script
  can be used to automatically format code.
- Fix all security and reliability bugs reported by SonarQube static analysis.
- Now also compiling and running on NetBSD (FUSE only) and OpenBSD (`afpcmd` only).

## afpfs-ng 0.9.0 (February 9, 2025)

### FUSE Client Improvements

- Support for FUSE 2.99 on Linux, and macFUSE on macOS.
- Shore up error handling and memory synchronization to avoid deadlocks.
- Rename `mount_afp` to `mount_afpfs` to avoid name conflict with the native macOS AFP client.
- The ability to pass mount options to FUSE with `-O` is now supported, amongst other patches contributed by R.J.V.
  Bertin.

### afpgetstatus Improvements

- Support for IPv6 addresses. Print much more information on the AFP server.
- Introduce a `-i` flag to print the server's icon and icon mask.

### Meson Build System

- Autotools has been replaced by Meson. Configure and builds are much faster. Extra debug output is now controlled at
  compile time with `-Dbuildtype=debug`. If you want to build without debug output, use `-Dbuildtype=release`.

### Code Quality

- Consolidation and refactoring of the code. Numerous bug fixes.
- Code compiles cleanly without warnings on supported platforms now.

## afpfs-ng 0.8.2 (March 11, 2013)

### IPv6 Support

- The networking code has been updated to use `getaddrinfo()`. If a hostname is provided in the URL, libafpclient will
  try to connect to every address in the order in which they are returned, until a successful connection is made.
- The URL parser supports IPv6 literals (e.g. `afp://[2001:db8::cafe]:548/Movies`).

### A Ton of Bug Fixes

This release includes all the patches collected by XBMC (<http://xbmc.org/>) plus some more written by me. Thanks to
everyone who contributed to this project!

These patches have already been applied to the source and can be found in `afpfs-ng-0.8.2/patches/`.

## afpfs-ng 0.8.1 (March 8, 2008)

### Read-Only Support

You can mount volumes read-only with:

    mount_afp -o ro afp://username:password@hostname/volumename /mountpoint

Per request from various people.

### `@` and `:` in Passwords and Usernames

For one `@` in a password, use:

    p@@ssword

For one `:` in a username, use:

    user::name

Per request from Niclas Helbro.

### Fstab

You can now automatically mount volumes on boot with a line in fstab. See `docs/README`; there are some simple but
specific instructions.

## afpfs-ng 0.8 (February 18, 2008)

### New Command Line (Non-FUSE) Tools

#### Batch Mode of afpcmd

This lets you do simple transfers, e.g.:

    $ afpcmd afp://user:pass@server/alexdevries/linux-2.6.14.tar.bz2
    Connected to server Cubalibre using UAM "DHX2"
    Connected to volume alexdevries
        Getting file /linux-2.6.14.tar.bz2
    Transferred 39172170 bytes in 2.862 seconds. (13687 kB/s)

#### Interactive Mode of afpcmd

This is file transfer tool similar to an FTP client. Has (local) filename completion and command history.

#### Get Status Tool, afpgetstatus

A simple tool to get the status information of a server without logging in.

### FUSE Client Improvements

Continuation of FUSE client development, including the introduction of a new tool called `mount_afp`, which has the same
syntax as in Mac OS X. Better status and post-deployment debugging, proper forced or unforced exit and other bugs.

### Protocol Fixes

Many protocol enhancements and bug fixes, including support for AFP 2.x, multiple servers, session keys, signatures,
meta information, chmod and chown fixes. Tested against Mac OS X, OS9, Airport and netatalk.

File transfer performance is now similar or faster to Mac OS X.

### Development Library

The source code of afpfs-ng has now been changed to a library (`libafpclient`) and support for multiple clients
(examples are FUSE, `afpcmd`, `afpgetstatus`). With this library, more AFP clients (GIO, KIO) can be built with limited
pain. This API is not yet stabilized.

### Other

- FUSE client fully validated on Linux, builds on FreeBSD.
- Command line client builds and runs on Linux, runs but is weakly tested on FreeBSD and Mac OS X.
- There are manpages.

## afpfs-ng 0.4.3 (September 8, 2007)

New features in this release include:

- UTF-8 internationalization of filenames, volumes, and servers, mostly written by Michael Ulbrich.
- Proper uid/gid mapping to enable environments with a common or different user directory.
- DHX2 UAM from Derrik Pates.
- Fixes to deal with the Apple Airport Extreme quirks.
- Fixes to deal with netatalk quirks.
- Improved status output for debugging.
- Various small bug fixes (Paul Borman, Volker Grabsch).

This is the last release before a rework that introduces libafpclient to handle multiple clients.

You can get afpfs-ng from <http://afpfs-ng.sourceforge.net>.

## afpfs-ng 0.4 (February 11, 2007)

We're happy to release version 0.4 of afpfs-ng, the Apple Filing Protocol client for Linux with FUSE.

New features include:

- Stability: afpfs-ng is reliable to the point of being usable for all but the most strenuous IO loads.
- Encrypted authentication mechanisms: now also supporting Randnum, 2-Way Randnum, DHCAST128, and DHX2.
- Performance: DID caching, enhanced DSI packet processing, and other improvements make afpfs-ng about half as fast as
  the Mac OS X client.
- Ease of use: unmounting, SIGINT, easier logging, easier startup, better docs, session suspension.

For more information and to download, see <http://afpfs-ng.sf.net/>.

## afpfs-ng 0.3 (November 27, 2006)

- Many, many memory leaks fixed with some help from valgrind and close inspection.
- Fixed many segfaults from null pointer dereferencing.
- Heap corruption fixed.
- Server version matching works now (afpfs-ng speaks 3.2 by default).
- More testing against 10.2, 10.3, 10.4, and netatalk.
- Some documentation.
- Preliminary support for resource forks, although this is still incomplete and broken.

0.2 had a lot of problems with stability, mostly because of the complex and optimized DSI read code, which does
zero-copy reads and handles quantums properly. This appears to be fixed now.

Please, if you've tried afpfs-ng, let me know. You can subscribe to the mailing list at
<http://sourceforge.net/projects/afpfs-ng>, or mail me at <alexthepuffin@gmail.com>.

## afpfs-ng 0.2 (November 20, 2006)

In version 0.2, the project has matured considerably. You can mount a volume, see, read, and write files with reasonable
performance.

- Much better testing against 10.3 and 10.4 (speaking AFP 3.1, not 3.2).
- Better error handling.
- Restructured into client/server.
- Rewrote incoming reads to optimize for read performance. It works!
- Lots and lots of other changes:
  - Fixed rename, large DIDs, UTF-8 names.
- Dynamic quantum calculation.
- Status.
- New logging mechanism, forking.
- Autoconf (sigh).

## afpfs-ng 0.1 (October 16, 2006)

- It compiles.
- It has worked for me, but probably nobody else.
