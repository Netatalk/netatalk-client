# Benchmarking a mounted filesystem

`vfs_lantest` and `vfs_speedtest` measure a filesystem through the POSIX file
API, so they benchmark anything the kernel can mount: an AFP volume through
the Netatalk FUSE client, an SMB share through the kernel CIFS client, an NFS
export, or a local directory. The two tools know nothing about the protocol
behind the mount. What they measure is the whole path from the system call to
the server and back: the kernel, the filesystem client and the server.

They run the workloads of the Netatalk server benchmarks `afp_lantest` and
`afp_speedtest`, with the same test set, geometry, statistics and CSV layout.
The server tools issue AFP requests and need an AFP server; these two issue
file operations and need a directory. The same Netatalk scripts chart both,
so a server measured directly and a server measured through a mount can be
compared.

## Building

The tools are built with the `with-testsuite` meson option, the flag the
Netatalk server uses for its own test suite. It is off by default and takes
effect on Linux only, because the tools use `O_DIRECT` and
`copy_file_range(2)`. They install beside the other Netatalk Client programs
with the manual pages `vfs_lantest(1)` and `vfs_speedtest(1)`:

```sh
meson setup build -Dwith-testsuite=true
meson compile -C build
```

## vfs_lantest: latency of file operations

`vfs_lantest` times seventeen tests, each a burst of small operations, and
reports the time of every test per pass:

1. Writing one large file, one write per chunk
2. Reading it back
3. Creating 2000 files and a stat of each
4. Creating a 20x9x10 directory tree
5. Open, write 1024 bytes, close, on each of the 2000 files
6. Open, read 1024 bytes, close, on each
7. Copying 1000 files through user space
8. Copying 2000 files with `copy_file_range(2)`
9. Stat of each file
10. Listing the directory with a stat of every entry
11. Lock then unlock one byte in each of the 2000 open files
12. Deleting the 2000 files
13. Lock then unlock 2000 byte ranges of one file
14. Stat of 20 directories and their 100 files each
15. Create, stat, stat, delete on 500 files, listing every tenth round
16. 100 walks of a 20-level directory chain, one stat per level
17. Four stats of each of 500 files

Options: `-P directory` (required), `-n iterations` (default 2; above five the
fastest and slowest sample of every test are dropped), `-f tests` (test
numbers; tests on the shared 2000 files pull in their creation and deletion),
`-K` (the four cache tests only), `-s MiB` (large-file size, default 100),
`-q KiB` (chunk size, default 1024), `-c` (CSV output), `-D` (buffered I/O).

```sh
vfs_lantest -c -n 20 -P /mnt/afpfs > lantest.csv
```

## vfs_speedtest: throughput of file transfers

`vfs_speedtest` measures `Read`, `Write`, `Copy` and `ServerCopy` in MB/s, at
one file size or across a sweep of sizes, with one warmup run and N measured
runs per size and operation. `Read`, `Write` and `Copy` move the data through
user space one chunk at a time; `ServerCopy` hands the whole file to
`copy_file_range(2)`.

Options: `-P directory` (required), `-f operations` (default Write),
`-n runs` (default 1), `-W warmup` (default 1), `-d MiB` (one size, default 64)
or `-z sizes` (a sweep in MiB; fractions name KiB sizes, 4 KiB is
0.00390625), `-q KiB` (chunk size, default 1024), `-c` (CSV output), `-D`
(buffered I/O).

```sh
vfs_speedtest -c -n 20 -f Read,Write,Copy,ServerCopy \
    -z 0.00390625,0.0078125,0.015625,0.03125,0.0625,0.125,0.25,0.5,1,2,4,8,16,32,64,128,256 \
    -P /mnt/afpfs > speedtest.csv
```

## What the data path does

Every data read and write opens the file with `O_DIRECT`, so the kernel page
cache never answers for the filesystem behind the mount. A plain local
filesystem refuses `O_DIRECT` for the 1024-byte transfers of the small-file
tests; pass `-D` for buffered I/O there.

`copy_file_range(2)` stands in for a server-side copy. An SMB3 server runs it
as a server-side copy and an NFS 4.2 server as a `COPY`; a FUSE filesystem
runs it in its own `copy_file_range` operation when it has one; without an
offload the kernel reads and writes the data itself. On 64-bit Linux the
Netatalk FUSE client copies a whole file of 512 KiB and up, before Netatalk
4.5 at most 1 GiB with its resource fork, into a file the copy created on a
Netatalk server on
AFP 3.4 with `FPCopyFile`, and any other range of a data or resource fork
through the client in quantum-sized reads and writes; elsewhere the kernel
copies.

Locks and directory listings go where the filesystem sends them. The kernel
CIFS client sends byte-range locks to the server; a FUSE filesystem without a
lock operation keeps them in the kernel. Repeated `stat` calls within the
attribute cache timeout of a mount, one second for FUSE and CIFS by default,
never reach the server.

## Mounts

Any mount works. Mount it, then point the tools at a directory in it:

```sh
mount -t cifs //server/share /mnt/smb -o user=someone,pass=secret,vers=3.1.1
vfs_lantest -c -n 20 -P /mnt/smb > lantest-smb.csv

mount -t nfs server:/export /mnt/nfs
vfs_speedtest -c -n 20 -f Read,Write -d 64 -P /mnt/nfs > speedtest-nfs.csv

vfs_lantest -c -D -n 5 -P /tmp/scratch > lantest-local.csv
```

The tools create everything inside a directory of their own under `-P` and
delete it on exit, also after an error.

## Charts

The CSV layouts are those of `afp_lantest -c` and `afp_speedtest -c`, so the
chart scripts in the Netatalk repository read them:

```sh
perl contrib/scripts/plot_lantest.pl lantest.csv -o lantest
perl contrib/scripts/plot_speedtest.pl speedtest.csv -o speedtest
```

The Netatalk Client performance dashboard runs both tools on every pull
request, on a FUSE mount of a Netatalk volume and on a CIFS mount of a Samba
share of the same directory; the container is described in
[test/perf/README.md](https://github.com/Netatalk/netatalk-client/blob/main/test/perf/README.md).
