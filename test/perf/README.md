# Mount benchmarks

`test/perf/` measures a filesystem client end to end: a server, a mount of
one of its volumes, and a benchmark issuing POSIX file operations on the
mount. The two tools know nothing about the protocol behind the mount, so the
same binaries measure the Netatalk FUSE client, a CIFS mount of a Samba share,
an NFS mount or a local directory. The performance dashboard runs them in one
privileged container on loopback, first against the Netatalk (AFP) FUSE mount
and then against the Samba (SMB) CIFS mount of the same directory.

## Pieces

- `vfs_lantest.c`: the `afp_lantest` test set as POSIX calls. Same test
  names, order, geometry, statistics and CSV layout, so the chart and
  history scripts read its output.
- `vfs_speedtest.c`: the `afp_speedtest` Read, Write, Copy and ServerCopy
  sweeps as POSIX calls, same runs, warmup, statistics and CSV layout.
- `vfs_common.[ch]`: checked file operations, aligned buffers, the timer
  and fatal-error cleanup, shared by both.
- `test_vfs_lantest.t`, `test_vfs_speedtest.t`: the `meson test` checks of
  the CSV layouts and of the cleanup, on a temporary directory with
  buffered I/O (`-D`).
- `base.Dockerfile`: the Netatalk testsuite image (server, entrypoint) plus
  Samba, the CIFS mount helper, libfuse and the flamegraph tools, built once
  per pinned netatalk commit and content of this file.
- `Dockerfile`: that base plus this tree's client and tools, built per PR.
- `entrypoint.sh`: starts the server, mounts over AFP and, when asked, over
  SMB, runs one tool per mount into `/results`, profiles `afpfsd` when
  asked.
- `.github/workflows/perf.yml`: the dashboard. One build job, three
  benchmark jobs (latency charts, throughput chart, flamegraph), one job
  that merges the metrics into `perf-history/history.yml` on `gh-pages` and
  posts the comment. The comment shows the AFP mount with its ratio to the
  SMB run of the same container, AFP/SMB for throughput and SMB/AFP for
  runtime, so above 1.00 the Netatalk client was the faster mount, and a
  chart of that ratio per lantest test across PRs; the SMB figures fold
  away under their own heading. `NETATALK_REF` in that file names the
  netatalk commit that supplies the server and the entrypoint.
- `contrib/scripts/plot_lantest.pl`, `plot_speedtest.pl`: the per-run
  charts from the CSVs, the latter with `--ratio` the AFP/SMB throughput
  ratio per file size; `plot_perf_history.pl`: the per-test trend across
  PRs from the history, and with `--ratio-prefix` the ratio of two mounts'
  series. Both ratio charts draw a parity line at 1.00.
- `.github/scripts/perf_history.pl`: merges a run's metrics into the
  history and writes the comment's tables and stats, with `--ratio` the
  ratio columns and figures; `.github/actions/publish-gh-pages`: publishes
  a chart as `<dir>/pr-<N>.<ext>` on `gh-pages`. The dashboard needs
  nothing from the netatalk repository beyond the server image.

The tools are built by meson on Linux with `-Dwith-testsuite=true`, the flag
the Netatalk server uses for its own test suite, and installed beside the other
netatalk-client programs, with the manual pages `vfs_lantest(1)` and
`vfs_speedtest(1)`.

## Data I/O

Every data read and write opens with `O_DIRECT`, so the kernel page cache
never answers for the filesystem behind the mount. `-D` keeps buffered I/O
for plain filesystems, which reject `O_DIRECT` for unaligned transfers.
`copy_file_range()` stands in for the server-side copies: an SMB3 server
runs it as a server-side copy, an NFS 4.2 server as a COPY, a FUSE daemon
through its `copy_file_range` operation when it has one; without an offload
the kernel moves the bytes itself.

## vfs_lantest

Count is the per-iteration file-operation total the `# Aggregate` block sums.
Reading a directory counts once per entry when each entry is also stat'ed,
and once per pass otherwise.

| afp_lantest test | POSIX calls in the timed region | Count | Netatalk FUSE client path |
| --- | --- | --- | --- |
| Writing one large file | `pwrite` per 1 MiB chunk, `close` | 101 | one `FPWriteExt` per chunk (`max_write` is the server quantum), and `FPCloseFork` at close |
| Reading one large file | `pread` per 1 MiB chunk, `close` | 101 | one `FPReadExt` per chunk (a read carries up to the server quantum) |
| Creating 2000 files | `mknod`, `stat` | 4,000 | `FPCreateFile`, `FPOpenFork`, `FPSetFileDirParms` for the mode, `FPCloseFork` and about three `FPGetFileDirParms`: libfuse turns `mknod` into create plus release; the `stat` is served by the kernel |
| Create 2000 dirs tree | `mkdir` | 2,000 | `FPCreateDir` and `FPSetFileDirParms` for the mode, plus a lookup |
| Open, write 1024 bytes, close | `open`, `pwrite`, `close` | 6,000 | `FPOpenFork`, `FPWriteExt`, `FPCloseFork` |
| Open, read 1024 bytes, close | `open`, `pread`, `close` | 6,000 | `FPOpenFork`, `FPReadExt`, `FPCloseFork` |
| Copying 1000 files client-side | read a file, create and write another | 6,000 | both fork lifecycles and `FPSetFileDirParms` for the copy's mode; the kernel's capability probe before each write is answered locally |
| Copying 2000 files server-side | `open` x2, `copy_file_range`, `close` x2 | 10,000 | thirteen requests per file: about five `FPGetFileDirParms`, two `FPOpenFork`, `FPCreateFile`, `FPSetFileDirParms` for the mode, `FPReadExt`, `FPWriteExt`, two `FPCloseFork`; the files are below 512 KiB, so the FUSE `copy_file_range` operation copies them through the client, and a whole file of 512 KiB and up, before Netatalk 4.5 with at most 1 GiB in its two forks, goes to a Netatalk server on AFP 3.4 as one `FPCopyFile` |
| Stat 2000 files | `stat` | 2,000 | one `FPGetFileDirParms` per file |
| Enumerate dir with 2000 files | `opendir`, `readdir` with `fstatat` per entry, `closedir` | 2,002 | `FPEnumerateExt2` pages; the per-entry `fstatat` is answered by the kernel's attribute cache, which the preceding stat test refreshed |
| Lock then unlock 2000 open forks | `fcntl` lock and unlock | 4,000 | kernel-local locks; no FUSE lock operation (an SMB mount sends them to the server) |
| Deleting 2000 files | `unlink` | 2,000 | `FPDelete` plus the type probe |
| Byte-range lock/unlock 2000 ranges | `fcntl` lock and unlock | 4,000 | kernel-local locks |
| Directory cache hits | `stat` per directory and file | 2,020 | `FPGetFileDirParms` once the kernel's one-second attribute cache expires |
| Mixed cache operations | `mknod`, `stat`, `stat`, `unlink`; `readdir` every tenth | 2,050 | create with `FPSetFileDirParms` for the mode, lookup, delete |
| Deep path traversal | `stat` per level per walk | 2,000 | kernel dentry cache after the first walk |
| Cache validation | four `stat` per file | 2,000 | kernel attribute cache |

```sh
vfs_lantest -c -n 20 -P /mnt/afpfs > lantest.csv
vfs_lantest -c -D -n 2 -s 4 -P /tmp/scratch
```

Options: `-c` CSV, `-D` buffered, `-f 1,3,9` test numbers, `-K` the cache
tests, `-n` iterations (above five the fastest and slowest sample of every
test are dropped), `-q` chunk KiB, `-s` large-file MiB.

## vfs_speedtest

| afp_speedtest operation | POSIX calls in the timed region |
| --- | --- |
| Read | `pread` per chunk of one file, filled during the first run |
| Write | `pwrite` per chunk, the same file rewritten every run |
| Copy | `pread` from Source then `pwrite` to Destination per chunk |
| ServerCopy | `copy_file_range` of Source into a fresh Destination until all of it is copied |

```sh
vfs_speedtest -c -n 20 -f Read,Write,Copy,ServerCopy \
    -z 0.00390625,0.0078125,0.015625,0.03125,0.0625,0.125,0.25,0.5,1,2,4,8,16,32,64,128,256 \
    -P /mnt/afpfs > speedtest.csv
```

Options: `-c` CSV, `-D` buffered, `-d` one size in MiB (default 64) or `-z` a
sweep in MiB, `-f` operations, `-n` measured runs per size and operation,
`-q` chunk KiB, `-W` warmup runs.

## Run it locally

Docker Desktop's Linux VM has `/dev/fuse`; a native Linux host works the
same. From the netatalk-client checkout, with a netatalk tree exported at
the pinned commit:

```sh
NETATALK_REF=$(sed -n 's/^  NETATALK_REF: //p' .github/workflows/perf.yml)
mkdir -p /tmp/netatalk-perf-base
git -C /path/to/netatalk archive "$NETATALK_REF" | tar -x -C /tmp/netatalk-perf-base
docker build -f /tmp/netatalk-perf-base/distrib/docker/testsuite_alp.Dockerfile \
    --build-arg BUILDTYPE=release -t netatalk-testsuite-perf /tmp/netatalk-perf-base
docker build -f test/perf/base.Dockerfile --build-arg BASE_IMAGE=netatalk-testsuite-perf \
    -t netatalk-client-perf-base .
docker build -f test/perf/Dockerfile --build-arg BASE_IMAGE=netatalk-client-perf-base \
    -t netatalk-client-perf .
mkdir -p results
docker run --privileged --rm -v "$PWD/results:/results" \
    -e AFP_USER=atalk1 -e AFP_PASS=afptest1 -e AFP_GROUP=afpusers \
    -e SHARE_NAME=test1 -e INSECURE_AUTH=1 -e AFP_CNID_TMPFS=1 \
    -e TESTSUITE=lan -e MOUNTS="afp smb" -e AFP_TEST_ITERATIONS=2 -e TEST_FLAGS=-c \
    netatalk-client-perf
perl contrib/scripts/plot_lantest.pl results/lantest-afp.csv -o lantest
perl contrib/scripts/plot_lantest.pl results/lantest-smb.csv -o lantest-smb
```

`TESTSUITE=speed` with `TEST_FLAGS="-c -z 0.00390625,1,16"` runs the
throughput sweep into `results/speedtest-afp.csv` and
`results/speedtest-smb.csv`; `plot_speedtest.pl` draws one chart per CSV,
each with the min–max band of its mount. `FLAMEGRAPH=1` writes
`results/flamegraph.svg`. The CIFS mount needs
the host kernel's `cifs` module (`modprobe cifs` on a Linux host). The
password is on the `afpc fs mount` command line inside the container, so use
a throwaway one. The chart scripts need `perl` and `gnuplot`.

## The Samba (SMB) mount

With `smb` in `MOUNTS` the container starts `smbd` on the directory the AFP
volume serves, with the Apple interoperability modules a Mac-serving Samba
uses (`vfs objects = catia fruit streams_xattr`, `ea support = yes`) and
Samba's default locking, oplocks and leases, gives the AFP user the same
password, mounts the share with the kernel CIFS client (`vers=3.1.1`, the
default one-second attribute cache) and runs the same tool with the same
arguments. `smbd` is pinned with `AFP_SERVER_CPUS` like `afpd`. On the CIFS
mount the lock tests and ServerCopy reach the server (SMB byte-range locks,
a server-side copy), where the FUSE client keeps locks in the kernel and
copies on a Netatalk server on AFP 3.4 only a whole file of 512 KiB and up,
before Netatalk 4.5 with at most 1 GiB in its two forks, into a file the copy
created.

Any other CIFS or NFS mount on a Linux host takes the same benchmark command
line; the mount line is the mount's own, a CIFS share here:

```sh
mount -t cifs //server/share /mnt/smb -o user=someone,pass=secret,vers=3.1.1
vfs_lantest -c -n 20 -P /mnt/smb > lantest-smb.csv
```

## Repository settings the dashboard needs

- A `gh-pages` branch and GitHub Pages serving it from `/`; the charts are
  linked as `https://netatalk.github.io/netatalk-client/<dir>/pr-<N>.png`.
- The `AFP_PASSWD` secret: the test user's password, eight characters or
  fewer, as the netatalk repository holds it.
- The packages `netatalk-client-perf` and `netatalk-client-perf-base` set
  to public visibility after the first run, as the netatalk images are.

## Bumping the server

Change `NETATALK_REF` in `.github/workflows/perf.yml` to the new netatalk
commit, any commit on `main`; the next run builds and pushes the base image
for it, with the server at that commit and the Alpine packages of that day.
Every other PR reuses the base, so the server side of both protocols stays
identical across PRs and the PR that moves the pin is the one whose
dashboard shows the server's effect. The base image tag carries the pin and
a hash of `base.Dockerfile`, so a change to that file builds a new base the
same way. The dashboard's 30-PR rolling average absorbs the step within
weeks and its run-baseline column (`Adj Δ%`) cancels a uniform shift.
