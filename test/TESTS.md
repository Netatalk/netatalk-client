# Tests for Netatalk Client

## Test suite overview

Tests are organized into two directories:

- `unit/` contains the unit tests and their helpers, run by `meson test` against build-tree binaries
- `integration/` contains the installed-client tests, run in a container or on a host with a running AFP server

The integration tests use the `Test::More` library (part of Perl core) and are executed with the `prove` test runner.
These tests assume a netatalk AFP server is running locally with a share at `afp://localhost/afpfs_test`
that allows both guest and authenticated access.
The default authenticated user is `test_usr` with password `test_pwd`.
See Manual environment prep below for setup instructions.

---

## Run integration tests in a container

Build and run the integration tests inside a self-contained container image.
The image compiles Netatalk Client from source and includes a Netatalk AFP server.

### Build

```sh
podman build -f test/integration/Dockerfile -t netatalk-client-test .
```

### Run

```sh
podman run --rm netatalk-client-test
```

`prove` exits non-zero on any test failure, which causes the container to exit with a non-zero status.

### Include FUSE tests

The image includes libfuse 3 and `fusermount3`. To include `test_fuse.t`:

```sh
docker run --rm --device /dev/fuse --cap-add SYS_ADMIN \
    -e AFP_TEST_FUSE=1 netatalk-client-test
```

The same options can be used with a rootful Podman runtime. The Linux host
(or Linux VM on macOS) must provide `/dev/fuse`. The mount lives inside the
container; no host directory or host mount namespace is shared.

The entrypoint starts the AFP server as root, then runs the FUSE test and
client daemons as `test_usr`. It restricts their capability bounding set to
`SYS_ADMIN`; their effective capabilities are empty. The setuid `fusermount3`
helper temporarily acquires the mount capability. This does not require
`--privileged`, but is not a fully unprivileged container: the helper still
needs `SYS_ADMIN`. Do not add `no-new-privileges` to this mode, since it
prevents that helper from acquiring its privilege.

Docker's default seccomp profile is sufficient on the tested Docker Desktop
runtime. Other hosts may need a local AppArmor or SELinux policy that allows
FUSE mounts.

### Saving container logs

To save container logs locally, bind a directory into the container and set
`AFP_TEST_LOG_DIR`. Use Bash with `pipefail` so test failures remain visible
through `tee`:

```bash
set -o pipefail
mkdir -p /tmp/netatalk-client-test-logs
docker run --rm --device /dev/fuse --cap-add SYS_ADMIN \
    --volume /tmp/netatalk-client-test-logs:/test-logs \
    -e AFP_TEST_FUSE=1 -e AFP_TEST_LOG_DIR=/test-logs \
    netatalk-client-test 2>&1 | tee /tmp/netatalk-client-test-logs/integration-tests.log
```

Logs remain in the host directory after the container exits. An explicit
`AFP_FUSE_DEBUG_LOG` overrides the default FUSE log path; place it inside the
mounted directory to preserve it.

---

## Run integration tests on a host

### Prerequisites

- Netatalk Client built and installed (`afpc`, `afpcmd`, `afpfsd`, `mount_afpfs` on `PATH`)
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
cd test/integration
prove test_afpcmd_batch.t
pkill -x afpsld || true
while pgrep -x afpsld > /dev/null 2>&1; do sleep 0.1; done
prove test_afpcmd_interactive.t
```

The `afpsld` session daemon is killed between the two test runs to ensure a clean slate for the interactive suite.

### Running the FUSE test

`test_fuse.t` requires a real kernel FUSE mount. On a configured host:

```sh
cd test/integration
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
afpc fs unmount ./afpfs_mnt
```

On macOS with macFUSE, use:

```sh
/sbin/umount ./afpfs_mnt
```
