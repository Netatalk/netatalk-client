# Developer Documentation for Netatalk Client

The Apple Filing Protocol is a network filesystem that is commonly used
to share files between Apple Macintosh computers.

A network connection must be established to a server and maintained.  
Netatalk Client provides a basic library on which to build full clients
(called libafpclient), and a sample of clients (FUSE and a simple
command line).

## Architectural Diagram

    ┌──────────────────────────────────────────────────────────────┐
    │                    Client Applications                       │
    ├─────────────────────────────┬────────────────────────────────┤
    │   afpcmd, GUI clients       │   afp_client, mount_afpfs      │
    └───────┬─────────────────────┴─────────┬──────────────────────┘
            │                               │
            │ libafpsl.so (afpsl.h)         │ (direct spawn/mount)
            │ Stateless API                 │
            ↓                               ↓
        ┌──────────────────┐          ┌─────────────────────┐
        │   afpsld         │          │   afpfsd            │
        │   (Stateless)    │          │   (FUSE)            │
        ├──────────────────┤          ├─────────────────────┤
        │ • CONNECT        │          │ • MOUNT/UNMOUNT     │
        │ • File I/O       │          │ • FUSE operations   │
        │ • Dir ops        │          │ • Multi-mount mgmt  │
        │                  │          │                     │
        │ NO dependencies  │          │ Requires libfuse    │
        └───────┬──────────┘          └───────┬─────────────┘
                │                             │
                │  Calls midlevel API         │  Calls midlevel API
                └─────────────┬───────────────┘
                              ↓
                ┌──────────────────────────────────┐
                │      libafpclient.so             │
                ├──────────────────────────────────┤
                │  • midlevel (afp.h, midlevel.h)  │
                │  • lowlevel                      │
                │  • proto_* (AFP protocol)        │
                │  • Engine (DSI, event loop)      │
                └──────────────┬───────────────────┘
                               ↓
                       ┌───────────────┐
                       │  AFP Server   │
                       └───────────────┘

## libafpclient

This is a shared library (libafpclient.so) that implements the basic DSI
and AFP communication requirements for connecting to AFP servers.  An
AFP client uses this library through several APIs, defined later.

You should use libafpclient in a situation where you have a stateful process.
This means that the process that's handling your client lives for the duration
of the transactions required.

A key point to know when building libafpclients is that libafpclient will
spawn threads and override signals.  Asynchronous events need to be
hooked into a loop provided by libafpclient.  You cannot write your own
select() loop!

The major subcomponents of libafpclient are all in the lib/ directory.

They are:

### Midlevel

This is an API that simplifies the AFP functions that does some simplification
of the protocol, such as calling multiple AFP functions to perform a basic
task.  This is the most likely API set to use when using libafpclient
directly.

Typically, a midlevel function will:

- translate filenames for you
- handle metainformation (resource forks, special files)
- call the lowlevel function

### Lowlevel

This is an API that handles many AFP functions, while taking care of some
AFP details, such as behaviour differences between AFP versions and
situations where servers don't adhere to the exact protocol.

An example of this is when listing a directory; ll_readdir() will
figure out what AFP version is being used, and either call protocols
afp_enumerateext() for AFP 2.x or afp_enumerateext2 for 3.x (which can
handle larger file lists).

These are implemented in lib/midlevel.c.  The API is exposed in midlevel.h.

You should generally not use these functions.

### Protocol

This is the raw API that exposes individual AFP functions, this
includes things like afp_listextattr().

These are implemented in lib/proto_* files and exposed in afp.h.

You should almost never use this set of functions.

Other topics

- startup
- metainformation
- scheduling

## AFP Protocol Compliance

### AFP 3.3 (OS X 10.6)

#### Replay Cache Support

AFP 3.3 **mandates** support for the AFP Replay Cache mechanism, which ensures reliable operation across
network interruptions and reconnections.

1. **Persistent Request IDs**: Request IDs are no longer reset to 0 on reconnection when the server
   supports replay cache. They wrap around from 65535 to 1 (avoiding 0).

2. **Server Capability Detection**: During `DSIOpenSession`, the client now parses the
   `kServerReplayCacheSize` option from the server's reply to detect replay cache support.

3. **Dynamic Behavior**:
   - If server advertises replay cache support → persistent request IDs enabled
   - If server doesn't support replay cache → legacy behavior (reset to 0 on reconnect)

----

## Two-Daemon Architecture

The project uses two separate daemon binaries with distinct purposes:

### afpsld - AFP Stateless Daemon

**Location**: `daemon/`

**Purpose**: Handles remote AFP file operations via the stateless API

**Dependencies**: libafpclient.so only (NO FUSE required)

**Socket**: `/tmp/afp_sl-<uid>`

**Operations**:

- Connection management (CONNECT, ATTACH, DETACH)
- File operations (OPEN, READ, WRITE, CLOSE, STAT)
- Directory operations (READDIR, MKDIR, RMDIR)
- File manipulation (CREAT, UNLINK, RENAME, CHMOD, TRUNCATE)
- Volume information (STATFS)

**Architecture**:

- Listens on a user-specific Unix domain socket
- Spawns a thread for each incoming request
- Maintains global state for all connected servers and attached volumes
- Threads are detached and clean up automatically after responding

### afpfsd - AFP FUSE Daemon

**Location**: `fuse/`

**Purpose**: Mounts AFP volumes as local filesystems using FUSE

**Dependencies**: libafpclient.so + libfuse

**Socket**: `/tmp/afp_server-<uid>-<mountpoint-hash>`

**Operations**:

- FUSE mount management (MOUNT, UNMOUNT, STATUS)
- Local FUSE filesystem operations
- Manager daemon coordinates multiple mounts

This daemon uses a multi-daemon model where a manager process spawns individual mount-specific daemon processes
for fault isolation (see Multi-Mount Architecture section).

## Multi-Mount Architecture for FUSE

**Design Choice**: The Netatalk Client FUSE client uses a manager daemon architecture
where each mount gets its own isolated daemon process,
providing fault isolation and simpler state management for multiple mounts.

Compared to a single shared daemon with per-mount multi threading or multiplexing, this design offers:

**Benefits**:

- **Fault Isolation**: One mount crashing doesn't affect others
- **Resource Isolation**: Independent memory spaces and CPU usage
- **Security Compartmentalization**: Credential isolation per mount

**Trade-offs**:

- ~2-3 MB memory overhead per additional mount
- Slightly slower multi-mount startup (each daemon forks independently)
- More socket files in /tmp

### Multi-Daemon Model Overview

    ┌─────────────────────────────────────────────────────────────┐
    │ mount_afpfs afp://server/vol1 /mnt/vol1                     │
    │ mount_afpfs afp://server/vol2 /mnt/vol2                     │
    │ mount_afpfs afp://server/vol3 /mnt/vol3                     │
    └─────────────────────────────────────────────────────────────┘
                ↓ All requests go through manager
    ┌─────────────────────────────────────────────────────────────┐
    │ afpfsd --manager (PID: 1000) [socket: afpfsd-501]           │
    │   ├── Tracks child PIDs: [2001, 2002, 2003]                 │
    │   ├── Spawns mount-specific daemons on demand               │
    │   └── Handles coordinated shutdown (exit command)           │
    └─────────────────────────────────────────────────────────────┘
            ↓ Spawns independent mount daemons
    ┌──────────────────────────────┬──────────────────────────────┬──────────────────────────────┐
    │ afpfsd --socket-id ... (2001)│ afpfsd --socket-id ... (2002)│ afpfsd --socket-id ... (2003)│
    │ [socket: afpfsd-501-bdb4...] │ [socket: afpfsd-501-8f3e...] │ [socket: afpfsd-501-c7d2...] │
    │   /mnt/vol1 FUSE mount       │   /mnt/vol2 FUSE mount       │   /mnt/vol3 FUSE mount       │
    └──────────────────────────────┴──────────────────────────────┴──────────────────────────────┘

### Mount Flow

1. `mount_afpfs afp://server/vol1 /mnt/vol1`
2. Client computes mount socket ID via hash of `/mnt/vol1` → `afpfsd-501-bdb4a5c2`
3. Client tries to connect to mount socket → doesn't exist
4. Client connects to manager socket `afpfsd-501`
5. If manager doesn't exist, client spawns it: `afpfsd --manager`
6. Client sends `AFP_SERVER_COMMAND_SPAWN_MOUNT` with socket ID and mountpoint
7. Manager forks child process: `afpfsd --socket-id afpfsd-501-bdb4a5c2`
8. Mount daemon listens on its unique socket and performs FUSE mount
9. Client receives success, sends actual mount request to mount daemon socket

### Coordinated Shutdown

    afp_client exit

1. Client connects to manager socket `afpfsd-501` (NULL mountpoint)
2. Sends `AFP_SERVER_COMMAND_EXIT`
3. Manager daemon:
   - Sends SIGTERM to all tracked child PIDs
   - Waits 1 second for graceful shutdown
   - Sends SIGKILL to any remaining children
   - Waits for all children with `waitpid()`
   - Exits manager daemon

Result: All mounts unmounted cleanly, no orphaned processes

**Key Functions**:

- `get_daemon_filename(char *name, size_t size, const char *mountpoint)` in `fuse/client.c`
  - mountpoint != NULL: hashes path → unique socket per mount (e.g., `afpfsd-501-bdb4a5c2`)
  - mountpoint == NULL: returns manager socket (e.g., `afpfsd-501`)
  - Used on all platforms (no `#ifdef` branching)

- `start_manager_daemon()` in `fuse/client.c`
  - Forks and execs: `afpfsd --manager`
  - Only runs if manager socket doesn't exist

- `start_afpfsd(const char *mountpoint)` in `fuse/client.c`
  - Connects to manager socket (starts manager if needed)
  - Sends `AFP_SERVER_COMMAND_SPAWN_MOUNT` request
  - Manager spawns mount daemon with unique socket ID

- `run_manager_daemon()` in `fuse/daemon.c`
  - Listens on shared socket (e.g., `afpfsd-501`)
  - Handles `SPAWN_MOUNT`, `EXIT`, `PING` commands
  - Tracks child daemon PIDs in linked list
  - Reaps dead children periodically via `waitpid(..., WNOHANG)`

- `main()` in `fuse/daemon.c`
  - Detects `--manager` flag → calls `run_manager_daemon()`
  - Detects `--socket-id` flag → runs mount daemon on that socket
  - No flag: backward compatible mode (shared socket, deprecated)

### Socket Naming

All socket files are created in `/tmp/`:

- **Manager socket**: `/tmp/afp_server-<uid>` (e.g., `/tmp/afp_server-501`)
- **Mount sockets**: `/tmp/afp_server-<uid>-<hash>` (e.g., `/tmp/afp_server-501-bdb4a5c2`)
  - Hash is computed via djb2 algorithm on mountpoint absolute path
  - Ensures unique socket per mountpoint

### Management Commands (status, unmount, exit)

Use NULL mountpoint in `daemon_connect()`, which causes:

- `get_daemon_filename(NULL)` → returns manager socket name (e.g., `afpfsd-501`)
- Commands connect to manager daemon

----

## Stateless Client Library and Daemon Architecture

### Stateless Client Library (libafpsl)

Netatalk Client provides a stateless client library (`libafpsl.so`) for applications that need to perform AFP operations
without managing persistent connections or event loops.
Unlike libafpclient which requires a long-lived stateful process with its own event loop,
the stateless library delegates connection management to a daemon process.

**Key characteristics:**

- **No event loops**: Applications don't need to integrate with libafpclient's signal handlers or select() loops
- **Daemon-managed state**: Server and volume connections persist in the daemon, not the client process
- **One-shot operations**: Each API call sends a request to the daemon via Unix socket and receives a response
- **Process isolation**: Client process crashes don't affect active AFP connections managed by the daemon
- **Consumer-owned logging**: Applications can route library and daemon messages into their own logging framework

Register a logger before making stateless calls:

    static void app_log(void *context, int level, const char *message)
    {
        /* level is one of the syslog LOG_* values. */
    }

    afp_sl_set_log_callback(app_log, application_context);

The callback runs synchronously on the calling thread. Passing a null callback disables log delivery. Registration is
process-global, matching the stateless library's process-global connection state.

The library also provides metadata-only replacement helpers for local-to-AFP, AFP-to-local, and AFP-to-AFP copies.
Callers select the local representation on each operation with `enum afp_metadata_mode`; supported modes are filesystem
xattrs, macOS AppleDouble, Netatalk AppleDouble, automatic detection, and none. The destination must already exist.
These helpers clear represented destination metadata before copying Finder Info, the resource fork, and eligible generic
xattrs. They deliberately do not copy the data fork, POSIX mode, or timestamps.

The stateless API returns zero on success and negative errno values on failure. Metadata read and list calls instead
return a nonnegative byte count on success. Generic xattr values are limited to 4096 bytes,
xattr name lists to `AFP_SL_XATTR_LIST_MAX`, and resource forks to `INT_MAX`. Resource-fork data is read and written in
4096-byte chunks. Positioned writes do not shorten an existing fork, except that a zero-length write at offset
zero clears it; call `afp_sl_truncateresourcefork()` to set its final length explicitly. `afp_sl_setxattr()` accepts
`AFP_SL_XATTR_CREATE` or `AFP_SL_XATTR_REPLACE` (the portable equivalents of
the system `XATTR_CREATE` and `XATTR_REPLACE` flags), but not both. `afp_sl_attach()` reports a volume-password challenge
through its optional status output while returning `-EACCES`. Consumers can classify session recovery with
`afp_sl_recovery_for_error()`. `afp_sl_changepw()` likewise uses a typed status output for password-policy details while
keeping its return value in the errno domain.

Metadata replacement is not atomic. A failure can leave partially copied destination metadata. Unsupported metadata and
values or lists above current protocol limits are reported through the optional `enum afp_metadata_warning` bitmask so
non-interactive consumers can apply their own policy without parsing library output.

**Use cases:**

- Command-line utilities (afpcmd)
- GUI applications that need asynchronous AFP operations
- Scripts and automation tools
- Applications on systems without FUSE support

### Stateless Protocol Communication

The stateless library communicates with afpsld using a request/response protocol over Unix sockets:

Every daemon response ends with a structured log trailer. Each record preserves its syslog severity, and libafpsl
delivers the records through the registered callback after validating the complete response. This applies to fixed and
streaming operations, including reads, writes, metadata calls, and directory listings.

**Connection model:**

1. **Short-lived Unix socket connections**: Most operations open a new connection to afpsld,
   send one request, receive one response, and close
2. **Persistent server/volume state**: Even though socket connections are ephemeral,
   the server and volume state persists in afpsld's memory
3. **Volume ID handles**: When a volume is attached, afpsld returns a `volumeid_t` (opaque pointer)
   that remains valid across separate socket connections as long as the daemon runs
4. **Connection reuse for CONNECT/ATTACH**: The CONNECT operation keeps the socket open (`close=0` flag)
   to allow the subsequent ATTACH to use the same connection

**Request flow:**

    Client Process           afpsld Daemon              AFP Server
        |                        |                          |
        |--CONNECT (close=0)---->|                          |
        |                        |----TCP connect---------->|
        |<---server_id-----------|                          |
        |                        |                          |
        |--ATTACH (close=1)----->|                          |
        |                        |----FPOpenVol------------>|
        |<---volumeid------------|                          |
        [connection closes]      |                          |
        |                        |                          |
        |--READDIR (close=1)---->|                          |
        |  (new socket)          |----FPEnumerate---------->|
        |<---file list-----------|                          |
        [connection closes]      |                          |

**Threading model:**

- afpsld accepts connections on the main thread
- Each accepted connection spawns a new detached thread
- Thread processes one command and sends response
- Thread exits and cleans up automatically
- Server/volume state persists in global data structures protected by locks

### Benefits of the Two-Daemon Approach

**Separation of concerns:**

- Stateless operations (afpsld) completely independent of FUSE
- FUSE mounting (afpfsd) can evolve independently
- Each daemon optimized for its specific use case

**Build flexibility:**

- Can build afpsld + libafpsl without FUSE dependency
- Systems without FUSE can still use AFP file operations
- Minimal dependencies for embedded or constrained environments

**Process isolation:**

- Stateless operations don't affect FUSE mounts
- FUSE mount crashes don't affect stateless operations
- Each daemon can be restarted independently

**Clear naming:**

- afpsld = AFP stateless daemon (file operations)
- afpfsd = AFP FUSE daemon (filesystem mounting)
- No conflict with Netatalk's `afpd` server

----

## afpcmd Implementation Using Stateless Library

### Overview

The afpcmd command-line utility has been refactored to use the stateless client library (libafpsl)
instead of directly calling the midlevel API. This change provides several benefits:

- **No event loop management**: afpcmd no longer needs to integrate with libafpclient's event loop
- **Simpler connection handling**: Connection state managed by afpsld daemon
- **Daemon-based architecture**: Aligns with modern client design patterns
- **Better fault isolation**: AFP connections survive afpcmd crashes

### Architecture Transition

**Before (stateful direct API):**

    afpcmd process
    ├── Calls midlevel API (ml_*) directly
    ├── Manages struct afp_server and afp_volume locally
    ├── Runs afp_main_loop() in background thread
    ├── Integrates with signal handlers
    └── Must live for duration of all operations

**After (stateless daemon API):**

    afpcmd process                afpsld daemon
    ├── Calls stateless API       ├── Manages global server/volume state
    │   (afp_sl_*)                ├── Runs afp_main_loop()
    ├── Opens/closes Unix socket  ├── Calls midlevel API (ml_*)
    ├── No event loop             └── Spawns threads per request
    ├── No signal handlers
    └── Can exit/restart freely

### Connection Management

afpcmd maintains minimal connection state:

- `volumeid_t vol_id` - Opaque handle returned by afpsld after ATTACH
- `int connected` - Boolean flag indicating whether a volume is attached
- `char curdir[]` - Current working directory path (client-side tracking)

**Connection flow:**

1. User runs: `afpcmd afp://user:pass@server/volume`
2. afpcmd calls `afp_sl_connect()` → afpsld connects to AFP server
3. afpcmd calls `afp_sl_attach()` → afpsld opens volume
4. afpcmd receives `volumeid_t` handle and sets `connected = 1`
5. All subsequent commands pass this volumeid to afp_sl_* functions
6. afpsld uses volumeid to look up the volume in its global state

**Disconnect flow:**

1. User runs: `disconnect` or `quit`
2. afpcmd calls `afp_sl_detach()` → afpsld closes volume
3. afpcmd sets `vol_id = NULL` and `connected = 0`

### Command Implementation

afpcmd commands map to stateless library operations:

| Command | Stateless API | Description |
| ------- | ------------- | ----------- |
| connect | afp_sl_connect() + afp_sl_attach() | Authenticate and attach to volume |
| disconnect | afp_sl_detach() | Detach from volume |
| ls/dir | afp_sl_readdir() | List directory contents |
| get | afp_sl_stat() + afp_sl_open() + afp_sl_read() + afp_sl_close() | Download files |
| put | afp_sl_creat() + afp_sl_open() + afp_sl_write() + afp_sl_close() | Upload files |
| rm | afp_sl_unlink() | Delete files |
| mkdir | afp_sl_mkdir() | Create directories |
| rmdir | afp_sl_rmdir() | Remove directories |
| mv | afp_sl_rename() | Rename/move files |
| chmod | afp_sl_chmod() | Change permissions |
| stat | afp_sl_stat() | Get file attributes |
| df | afp_sl_statfs() | Volume statistics |

### Example: File Download (get command)

**High-level flow:**

1. User runs: `get remote_file.txt local_file.txt`
2. afpcmd calls `afp_sl_stat(vol_id, path, basename, &stat)` to get file size
3. afpcmd calls `afp_sl_open(vol_id, path, basename, &fileid, O_RDONLY)`
4. afpcmd loops calling `afp_sl_read(vol_id, fileid, fork=0, offset, size, &received, &eof, buffer)`
5. Each chunk is written to local file
6. afpcmd calls `afp_sl_close(vol_id, fileid)` when complete

**What happens in afpsld:**

- Each afp_sl_* call creates a new Unix socket connection to afpsld
- afpsld finds the volume using volumeid (pointer lookup)
- afpsld calls corresponding midlevel API (ml_stat, ml_open, ml_read, ml_close)
- Response is sent back to afpcmd
- Connection closes (except for CONNECT which stays open for ATTACH)
