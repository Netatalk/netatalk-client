# Integration tests for Netatalk Client

## Test suite overview

| File | Tests |
| ---- | ----- |
| `test_afpcmd_batch.t` | Uploads a file to an AFP share via `afpcmd` batch mode, downloads it back, and verifies the checksum matches. |
| `test_afpcmd_interactive.t` | Exercises `afpcmd` interactive mode by piping command sequences through stdin and asserting expected output patterns. |
| `test_afpgetstatus.t` | Verifies `afpgetstatus` can retrieve and parse AFP server status information. |
| `test_fuse.t` | Mounts an AFP share via `mount_afpfs` (FUSE) and performs various operations to test its functionality. |

`test_fuse.t` must be run stand-alone on a real host — FUSE kernel support is not reliable inside containers. See below.

All tests use the `Test::More` library (part of Perl core) and are executed with the `prove` test runner.

The tests assume a netatalk AFP server is running locally with a share at `afp://localhost/afpfs_test`
that allows both guest and authenticated access.
The default authenticated user is `test_usr` with password `test_pwd`.
See Manual environment prep below for setup instructions.

---

## Container tests (batch + interactive only)

Build and run the container friendly tests inside a self-contained container image.
The image compiles Netatalk Client from source and includes a Netatalk AFP server.

### Build

```sh
podman build -f test/Dockerfile -t netatalk-client-test .
```

### Run

```sh
podman run --rm netatalk-client-test
```

`prove` exits non-zero on any test failure, which causes the container to exit with a non-zero status.

---

## Stand-alone tests

### Prerequisites

- Netatalk Client built and installed (`afpcmd`, `afpfsd`, `mount_afpfs`, `afp_client` on `PATH`)
- netatalk installed and configured (see Manual environment prep below)
- Perl (any recent version; all modules used are in core)

### Manual environment prep

The stand-alone tests expect a local Netatalk server named `afpfs_testsrv`
with a volume named `afpfs_test`. The batch and interactive tests use the
default credentials `test_usr` / `test_pwd`.

Create the test user and shared directory:

```sh
sudo useradd --no-create-home test_usr || true
echo 'test_usr:test_pwd' | sudo chpasswd
sudo mkdir -p /mnt/afpfs
sudo chmod 2755 /mnt/afpfs
sudo chown test_usr:test_usr /mnt/afpfs
```

Configure Netatalk's *afp.conf* with guest and DHX2 authentication enabled:

```ini
[Global]
log file = /var/log/afpd.log
log level = default:debug
server name = afpfs_testsrv
uam list = uams_guest.so uams_dhx2.so

[afpfs_test]
path = /mnt/afpfs
volume name = afpfs_test
```

### Running batch and interactive tests

```sh
cd test
prove test_afpcmd_batch.t
pkill -x afpsld || true
while pgrep -x afpsld > /dev/null 2>&1; do sleep 0.1; done
prove test_afpcmd_interactive.t
```

The `afpsld` session daemon is killed between the two test runs to ensure a clean slate for the interactive suite.

### Running the FUSE test

`test_fuse.t` requires a real kernel FUSE mount and must be run on the host (not inside a container):

```sh
cd test
prove test_fuse.t
```

By default, `test_fuse.t` authenticates as `test_usr` with password `test_pwd`.
Override these credentials with environment variables:

```sh
AFP_TEST_USER=my_user AFP_TEST_PASSWORD=my_password prove test_fuse.t
```

or pass them as test arguments:

```sh
prove test_fuse.t :: --user my_user --password my_password
```

To capture detailed `afpfsd` debug logs for a failing FUSE run, set
`AFP_FUSE_DEBUG_LOG`. In this mode, `test_fuse.t` starts the manager in
foreground debug mode and redirects manager and per-mount daemon logs to the
given file:

```sh
AFP_FUSE_DEBUG_LOG=/tmp/test_fuse-afpfsd.log prove test_fuse.t
```

The test creates an `afpfs_mnt/` directory in the current working directory and removes it implicitly when unmounted.
If a run is interrupted mid-test, unmount manually before re-running:

```sh
afp_client unmount ./afpfs_mnt
```
