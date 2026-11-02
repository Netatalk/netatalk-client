#!/usr/bin/perl

# Netatalk Client FUSE client tests
# Based on the original test/Makefile by Simon Vetter
# Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

use strict;
use warnings;

use Test::More;
use Config;
use Cwd qw(getcwd);
use File::Spec;
use Errno        ();
use Fcntl        qw(O_RDONLY O_WRONLY O_RDWR O_CREAT O_EXCL O_TRUNC O_SYNC O_DSYNC);
use Getopt::Long qw(GetOptions);
use IO::Handle;
use Time::HiRes ();

# O_DIRECT keeps the kernel's page cache out of a transfer, so every byte
# crosses into afpfsd. $O_DIRECT is 0 where Fcntl does not define O_DIRECT.
my $O_DIRECT = eval { Fcntl::O_DIRECT() } // 0;
use POSIX qw(WNOHANG setsid);

my $AFP_USER           = $ENV{AFP_TEST_USER}     // 'test_usr';
my $AFP_PASS           = $ENV{AFP_TEST_PASSWORD} // 'test_pwd';
my $AFP_HOST           = 'localhost';
my $AFP_VOL            = 'afpfs_test';
my $AFP_FUSE_DEBUG_LOG = $ENV{AFP_FUSE_DEBUG_LOG};
my $mnt_dir            = getcwd() . '/afpfs_mnt';
my $afpfsd_pid;

GetOptions(
           'user=s'     => \$AFP_USER,
           'password=s' => \$AFP_PASS,
) or BAIL_OUT('Invalid arguments. Usage: prove test_fuse.t :: --user USER --password PASSWORD');

sub url_escape_component {
    my ($value) = @_;
    $value =~ s/([^A-Za-z0-9._~-])/sprintf('%%%02X', ord($1))/eg;
    return $value;
}

my $AFP_AUTH_URL = sprintf(
                           'afp://%s:%s@%s/%s',
                           url_escape_component($AFP_USER),
                           url_escape_component($AFP_PASS),
                           $AFP_HOST,
                           $AFP_VOL,
);
my $AFP_GUEST_URL = "afp://$AFP_HOST/$AFP_VOL";

sub start_afpfsd_manager {
    # Own the manager's lifetime and keep daemon output out of prove's TAP
    # pipe. An orphaned manager holding that pipe open prevents prove exiting.
    my $log_path = $AFP_FUSE_DEBUG_LOG // File::Spec->devnull();
    open(my $log_fh, '>>', $log_path)
      or BAIL_OUT("Cannot open afpfsd log '$log_path': $!");
    $log_fh->autoflush(1);
    print $log_fh "\n==> Starting afpfsd manager for test_fuse.t pid $$\n";

    my $pid = fork();
    BAIL_OUT("Cannot fork afpfsd manager: $!") unless defined $pid;

    if ($pid == 0) {
        setsid() or die "setsid: $!";
        open(STDOUT, '>&', $log_fh) or die "dup stdout: $!";
        open(STDERR, '>&', $log_fh) or die "dup stderr: $!";
        close $log_fh;
        exec('afpfsd', $AFP_FUSE_DEBUG_LOG ? '--debug' : '--foreground', '--manager')
          or die "exec afpfsd: $!";
    }

    close $log_fh;
    $afpfsd_pid = $pid;
    sleep 1;

    my $exited = waitpid($pid, WNOHANG);
    if ($exited == $pid) {
        undef $afpfsd_pid;
        BAIL_OUT("afpfsd manager exited early; log: $log_path");
    }

    ok($pid > 0, "prepare: afpfsd manager started, logging to $log_path");
}

END {
    if ($afpfsd_pid) {
        my $exit_status = $?;
        kill 'TERM', -$afpfsd_pid;
        waitpid($afpfsd_pid, 0);
        $? = $exit_status;
    }
}

sub mount_or_bail {
    my ($description, @cmd) = @_;
    my $rc = system(@cmd);

    is($rc, 0, $description);
    BAIL_OUT("$description failed: @cmd exited with " . ($rc >> 8))
      if $rc != 0;
}

# Run a mount command on $mnt_dir that must refuse its arguments, and unmount
# what it mounted if it did not, so that the next mount finds $mnt_dir free.
sub refused_mount {
    my ($description, @cmd) = @_;
    my $rc = system(@cmd, $mnt_dir);

    is($rc, 2 << 8, $description);
    unmount_or_bail("$description: the mount it made unmounts") if $rc == 0;
}

# Return the status text of the mounted server.
sub status_text {
    open(my $status_fh, '-|', 'afpc', 'fs', 'status', $mnt_dir)
      or BAIL_OUT("Cannot run afpc fs status: $!");
    my $text = do { local $/; <$status_fh> };
    close $status_fh
      or BAIL_OUT('afpc fs status exited with ' . ($? == -1 ? "close error: $!" : 'status ' . ($? >> 8)));
    return $text;
}

# Return the per-command request counts of the mounted server, from the
# status text: "    afpReadExt: 12" lines.
sub request_counts {
    my %counts = status_text() =~ /^\s+(afp\w+): (\d+)$/mg;
    BAIL_OUT('afpc fs status listed no request counts') unless %counts;
    return \%counts;
}

# Return the requests of one command between two counts.
sub request_delta {
    my ($before, $after, $command) = @_;
    return ($after->{$command} // 0) - ($before->{$command} // 0);
}

# Return the flushes between two counts, by FPFlushFork or FPSyncFork.
sub flush_delta {
    my ($before, $after) = @_;
    return request_delta($before, $after, 'afpFlushFork') + request_delta($before, $after, 'afpSyncFork');
}

# Return the counts once the kernel's pending releases finish, which is when
# the FPCloseFork count settles.
sub settled_counts {
    my $last = request_counts();

    for (1 .. 100) {
        select(undef, undef, undef, 0.1);
        my $now = request_counts();
        return $now if request_delta($last, $now, 'afpCloseFork') == 0;
        $last = $now;
    }

    BAIL_OUT('the releases did not settle');
}

# Return the counts once $closes more forks close. The kernel releases a file
# after close(2) returns. A flush at release goes before the fork's
# FPCloseFork.
sub counts_after_release {
    my ($before, $closes) = @_;

    for (1 .. 100) {
        my $after = request_counts();
        return $after if request_delta($before, $after, 'afpCloseFork') >= $closes;
        select(undef, undef, undef, 0.05);
    }

    BAIL_OUT("$closes FPCloseFork did not arrive");
}

# Write $bytes to $path in system calls of at most 1 MiB. Each four-byte word
# holds, big-endian, the count of four-byte words before it, so data at a
# wrong offset fails cmp.
sub write_bytes {
    my ($path, $bytes) = @_;
    sysopen(my $fh, $path, O_WRONLY | O_CREAT | O_TRUNC)
      or BAIL_OUT("Cannot create $path: $!");
    my $offset = 0;

    while ($offset < $bytes) {
        my $len   = $bytes - $offset < (1 << 20) ? $bytes - $offset : (1 << 20);
        my $chunk = substr(pack('N*', $offset / 4 .. $offset / 4 + (($len + 3) >> 2) - 1), 0, $len);
        syswrite($fh, $chunk) == $len
          or BAIL_OUT("Short write to $path: $!");
        $offset += $len;
    }

    close $fh;
}

# Return a system call's number on x86_64 or aarch64 Linux with a 64-bit
# Perl, or undef elsewhere. The tables here cover those platforms.
sub syscall_nr {
    my (%nr) = @_;
    return $^O eq 'linux' && $Config{ptrsize} == 8 ? $nr{(POSIX::uname())[4]} : undef;
}

my $copy_file_range_nr = syscall_nr(x86_64 => 326, aarch64 => 285);
my $setxattr_nr        = syscall_nr(x86_64 => 188, aarch64 => 5);
my $getxattr_nr        = syscall_nr(x86_64 => 191, aarch64 => 8);
my $listxattr_nr       = syscall_nr(x86_64 => 194, aarch64 => 11);
my $removexattr_nr     = syscall_nr(x86_64 => 197, aarch64 => 14);
my $mknodat_nr         = syscall_nr(x86_64 => 259, aarch64 => 33);
my $SYSCALL_SKIP       = 'system calls by number need a 64-bit Perl on x86_64 or aarch64 Linux';
my $DEBUG_LOG_SKIP     = 'the daemon keeps no debug log without AFP_FUSE_DEBUG_LOG';
my $SETATTR_SKIP       = "macOS's combined setattr reports a refused change as done";

# Run copy_file_range(2) of $len bytes between two handles, at their file
# offsets or at the given ones. Perl has no builtin, so the call goes by its
# number. A packed offset passes as a pointer, a 0 as NULL.
sub copy_range {
    my ($in, $out, $len, $off_in, $off_out) = @_;
    my $in_ptr  = defined $off_in  ? pack('q', $off_in)  : 0;
    my $out_ptr = defined $off_out ? pack('q', $off_out) : 0;
    return syscall($copy_file_range_nr, fileno($in), $in_ptr, fileno($out), $out_ptr, $len, 0);
}

# Run copy_range() and take the request counts before and after it.
sub counted_copy {
    my $before = request_counts();
    my $copied = copy_range(@_);
    return ($copied, $before, request_counts());
}

# Return the requests a copy on the server sends and one through the client
# does not.
sub offload_delta {
    my ($before, $after) = @_;
    return request_delta($before, $after, 'afpDelete') + request_delta($before, $after, 'afpCopyFile');
}

# Run setxattr(2) on the resource fork of $path. The call ends with the
# fork's close.
sub set_resource_fork {
    my ($path, $value) = @_;
    my $name = 'user.com.apple.ResourceFork';
    syscall($setxattr_nr, $path, $name, $value, length $value, 0) == 0
      or BAIL_OUT("Cannot set the resource fork of $path: $!");
}

# Run removexattr(2) on the resource fork of $path. The call ends with the
# fork's close.
sub remove_resource_fork {
    my ($path) = @_;
    my $name = 'user.com.apple.ResourceFork';
    syscall($removexattr_nr, $path, $name) == 0
      or BAIL_OUT("Cannot remove the resource fork of $path: $!");
}

# Return the errno of a system call that returned $result, or 0 for a success.
sub errno_of {
    my ($result) = @_;
    return $result == -1 ? $! + 0 : 0;
}

# Run getxattr(2), setxattr(2) and removexattr(2) of $name on $path. Return
# each one's errno_of() and the extended attribute requests the three sent.
sub xattr_calls {
    my ($path, $name) = @_;
    my $value  = 'x';
    my $before = request_counts();
    my @errnos = (
                  errno_of(syscall($getxattr_nr,    $path, $name, 0, 0)),
                  errno_of(syscall($setxattr_nr,    $path, $name, $value, length $value, 0)),
                  errno_of(syscall($removexattr_nr, $path, $name)),
    );
    my $after    = request_counts();
    my $requests = 0;
    $requests += request_delta($before, $after, $_) for qw(afpGetExtAttr afpSetExtAttr afpRemoveExtAttr);
    return (@errnos, $requests);
}

# Return what the daemon's debug log holds from byte $offset on.
sub daemon_log_since {
    my ($offset) = @_;
    open(my $fh, '<', $AFP_FUSE_DEBUG_LOG) or BAIL_OUT("Cannot read $AFP_FUSE_DEBUG_LOG: $!");
    seek($fh, $offset, 0)                  or BAIL_OUT("Cannot seek in $AFP_FUSE_DEBUG_LOG: $!");
    my $text = do { local $/; <$fh> };
    close $fh;
    return $text // '';
}

# Return the timeouts libfuse parsed for the mount on $mnt_dir, from the
# "fuse init" line logged after byte $offset, or undef where none is, as from
# a FUSE 2 build. A stat(2) first waits out the mount's init.
sub fuse_init_timeouts {
    my ($offset) = @_;
    stat $mnt_dir;
    my ($timeouts) =
      daemon_log_since($offset) =~ /\*\*\* fuse init (entry_timeout=\S+ negative_timeout=\S+ attr_timeout=\S+)/;
    return $timeouts;
}

# Test whether $len bytes of $first from $first_off equal those of $second
# from $second_off.
sub same_bytes {
    my ($first, $second, $len, $first_off, $second_off) = @_;
    return system('cmp', '-s', '-n', $len, $first, $second, $first_off // 0, $second_off // 0) == 0;
}

# Return the request quantum the mounted server advertised, from the status
# text.
sub server_quantum {
    my ($quantum) = status_text() =~ /^\s+quantum: (\d+)$/m;
    BAIL_OUT('afpc fs status listed no quantum') unless $quantum;
    return $quantum;
}

sub unmount_or_bail {
    my ($description) = @_;
    my @cmd =
      $^O eq 'darwin'
      ? ('/sbin/umount', $mnt_dir)
      : ('afpc', 'fs', 'unmount', $mnt_dir);
    my $rc = system(@cmd);

    is($rc, 0, $description);
    BAIL_OUT("$description failed: @cmd exited with " . ($rc >> 8))
      if $rc != 0;
}

# -----------------------------------------------------------------------
# prepare test environment: ensure mount directory exists, start afpfsd
# -----------------------------------------------------------------------
mkdir $mnt_dir unless -d $mnt_dir;
ok(-d $mnt_dir, 'prepare: mount directory exists');

start_afpfsd_manager();

# -----------------------------------------------------------------------
# fuse_auth: authenticated mount
# -----------------------------------------------------------------------
sleep 1;
mount_or_bail(
              'fuse_auth: authenticated mount succeeds',
              'mount_afpfs', $AFP_AUTH_URL, $mnt_dir
);

open(my $wfh, '>', "$mnt_dir/sample.txt")
  or BAIL_OUT("Cannot write to mounted share: $!");
print $wfh "You should read this back\n";
close $wfh;

open(my $rfh, '<', "$mnt_dir/sample.txt")
  or BAIL_OUT("Cannot read from mounted share: $!");
my $content = do { local $/; <$rfh> };
close $rfh;
like( $content, qr/^You should read this back$/m,
      'fuse_auth: file content readable after write');

# -----------------------------------------------------------------------
# fuse_resume: authenticated suspend/resume keeps mounted volume usable
# -----------------------------------------------------------------------
is(system('afpc', 'fs', 'suspend', $mnt_dir), 0,
   'fuse_resume: authenticated suspend succeeds');

open(my $status_fh, '-|', 'afpc', 'fs', 'status', $mnt_dir)
  or BAIL_OUT("Cannot run afpc fs status: $!");
my $status          = do { local $/; <$status_fh> };
my $status_close_ok = close $status_fh;
my $status_rc       = $?;
ok($status_close_ok, 'fuse_resume: status command exits successfully')
  or diag('afpc fs status exited with ' . ($status_rc == -1 ? "close error: $!" : 'status ' . ($status_rc >> 8)));
like( $status, qr/connection: .*disconnected/,
      'fuse_resume: status reports disconnected server after suspend');

is(system('afpc', 'fs', 'resume', $mnt_dir), 0,
   'fuse_resume: authenticated resume succeeds without new password');

open(my $resume_rfh, '<', "$mnt_dir/sample.txt")
  or BAIL_OUT("Cannot read file after resume: $!");
my $resume_content = do { local $/; <$resume_rfh> };
close $resume_rfh;
like( $resume_content, qr/^You should read this back$/m,
      'fuse_resume: existing file readable after resume');

open(my $resume_wfh, '>', "$mnt_dir/resume.txt")
  or BAIL_OUT("Cannot write file after resume: $!");
print $resume_wfh "Resume write survived reconnect\n";
close $resume_wfh;

open(my $resume_check_fh, '<', "$mnt_dir/resume.txt")
  or BAIL_OUT("Cannot read resume check file: $!");
my $resume_check = do { local $/; <$resume_check_fh> };
close $resume_check_fh;
like( $resume_check, qr/^Resume write survived reconnect$/m,
      'fuse_resume: new file writable after resume');

unlink "$mnt_dir/resume.txt";
ok(!-e "$mnt_dir/resume.txt", 'fuse_resume: resume check file removed');

unmount_or_bail('fuse_auth: authenticated unmount succeeds');

# -----------------------------------------------------------------------
# fuse_auth: guest mount immediately after authenticated unmount
# -----------------------------------------------------------------------
mount_or_bail(
              'fuse_auth: guest mount succeeds',
              'mount_afpfs', $AFP_GUEST_URL, $mnt_dir
);

ok(
   -f "$mnt_dir/sample.txt",
   'fuse_auth: guest mount shows previously written file'
);

open(my $gfh, '<', "$mnt_dir/sample.txt")
  or BAIL_OUT("Cannot read file on guest mount: $!");
my $guest_content = do { local $/; <$gfh> };
close $gfh;
like( $guest_content, qr/^You should read this back$/m,
      'fuse_auth: guest mount file content matches');

unmount_or_bail('fuse_auth: guest unmount succeeds');

# -----------------------------------------------------------------------
# fuse_auth: authenticated mount cleanup immediately after guest unmount
# -----------------------------------------------------------------------
mount_or_bail(
              'fuse_auth: cleanup mount succeeds',
              'mount_afpfs', $AFP_AUTH_URL, $mnt_dir
);

open(my $cfh, '<', "$mnt_dir/sample.txt")
  or BAIL_OUT("Cannot read file on cleanup mount: $!");
my @lines = <$cfh>;
close $cfh;
like( $lines[0], qr/^You should read this back$/,
      'fuse_auth: cleanup mount file content matches');
is(scalar @lines, 1, 'fuse_auth: file has exactly one line');

# -----------------------------------------------------------------------
# fuse_requests: the status text counts the session's requests by command.
# -----------------------------------------------------------------------
my $counts = request_counts();
is($counts->{afpOpenVol}, 1, 'fuse_requests: status counts one FPOpenVol');
ok(($counts->{afpReadExt} // 0) >= 1,
   'fuse_requests: status counts the reads of this mount');

# -----------------------------------------------------------------------
# fuse_quantum: a direct read is one FPReadExt per server quantum. dd's
# buffer sits on a page boundary, so each 1 MiB read is one FUSE request. A
# Perl buffer does not, and the kernel would split it in two.
# -----------------------------------------------------------------------
SKIP: {
    skip 'direct reads with dd are Linux', 2 unless $^O eq 'linux';
    write_bytes("$mnt_dir/quantum.bin", 4 << 20);
    my @dd     = ('dd', "if=$mnt_dir/quantum.bin", 'of=/dev/null', 'bs=1M', 'iflag=direct', 'status=none');
    my $before = request_counts();
    my $rc     = system(@dd);
    my $after  = request_counts();
    is($rc, 0, 'fuse_quantum: dd reads 4 MiB directly');
    is(request_delta($before, $after, 'afpReadExt'), 4,
       'fuse_quantum: a 4 MiB direct read is four FPReadExt');
    unlink "$mnt_dir/quantum.bin";
}

# -----------------------------------------------------------------------
# fuse_listing: 800 entries list in two FPEnumerateExt2 requests, a page and
# the end page. The server's 64 KiB reply buffer holds about a thousand of
# them per page.
# -----------------------------------------------------------------------
{
    my $dir = "$mnt_dir/listing";
    mkdir $dir or BAIL_OUT("Cannot create $dir: $!");

    for my $i (0 .. 799) {
        my $name = sprintf('%s/f%04d', $dir, $i);
        open(my $fh, '>', $name) or BAIL_OUT("Cannot create $name: $!");
        close $fh;
    }

    my $before = request_counts();
    opendir(my $dh, $dir) or BAIL_OUT("Cannot open $dir: $!");
    my @names = grep { $_ ne '.' && $_ ne '..' } readdir $dh;
    closedir $dh;
    my $after = request_counts();
    is(scalar @names, 800, 'fuse_listing: 800 entries listed');
    is(request_delta($before, $after, 'afpEnumerateExt2'), 2,
       'fuse_listing: 800 entries are one page and the end page');
    unlink map {"$dir/$_"} @names;
    rmdir $dir;
    ok(!-e $dir, 'fuse_listing: listing directory removed');
}

# -----------------------------------------------------------------------
# fuse_names: a name of forty U+0390, which decomposes from two bytes each to
# six, reaches the server.
# -----------------------------------------------------------------------
{
    my $path = "$mnt_dir/" . ("\xce\x90" x 40);
    ok(sysopen(my $fh, $path, O_WRONLY | O_CREAT | O_EXCL), 'fuse_names: a file named with forty U+0390 is created');
    close $fh if $fh;
    ok(unlink($path) && !-e $path, 'fuse_names: the file named with forty U+0390 is removed');
}

# -----------------------------------------------------------------------
# fuse_sibling: a read beside a dirty fork of the same file sends no flush.
# -----------------------------------------------------------------------
SKIP: {
    skip 'this Perl has no O_DIRECT', 2 unless $O_DIRECT;
    my $path   = "$mnt_dir/sibling.bin";
    my $before = request_counts();
    sysopen(my $writer, $path, O_WRONLY | O_CREAT | O_TRUNC)
      or BAIL_OUT("Cannot create $path: $!");
    syswrite($writer, "\1" x 4096) == 4096
      or BAIL_OUT("Short write to $path: $!");
    sysopen(my $reader, $path, O_RDONLY | $O_DIRECT)
      or BAIL_OUT("Cannot open $path for direct reading: $!");
    my $got   = sysread($reader, my $buf, 4096);
    my $after = request_counts();
    is($got, 4096, 'fuse_sibling: the written bytes read back');
    is(flush_delta($before, $after), 0,
       'fuse_sibling: the read beside the dirty fork sends no flush');
    close $reader;
    close $writer;
    unlink $path;
}

# -----------------------------------------------------------------------
# fuse_close: close(2) sends no flush. A write to an O_SYNC or O_DSYNC file
# and fsync(2) each send one FPSyncFork.
# -----------------------------------------------------------------------
{
    my $path   = "$mnt_dir/flush.txt";
    my $before = settled_counts();
    open(my $fh, '>', $path) or BAIL_OUT("Cannot create $path: $!");
    print $fh "x" x 4096;
    close $fh;
    my $after = counts_after_release($before, 1);
    is(flush_delta($before, $after), 0,
       'fuse_close: closing a written file sends no flush');

    $before = request_counts();
    open($fh, '>', $path) or BAIL_OUT("Cannot open $path: $!");
    print $fh "y";
    $fh->flush;
    ok($fh->sync, 'fuse_fsync: fsync(2) succeeds');
    $after = request_counts();
    is(request_delta($before, $after, 'afpSyncFork'), 1,
       'fuse_fsync: fsync(2) sends one FPSyncFork');
    close $fh;

  SKIP: {
        skip 'Linux turns a write to a sync file into an fsync', 2 unless $^O eq 'linux';

        # The kernel flushes an O_DSYNC write as fdatasync(2) does.
        for my $flag ([O_SYNC, 'O_SYNC'], [O_DSYNC, 'O_DSYNC']) {
            my ($bits, $name) = @$flag;
            $before = request_counts();
            sysopen($fh, $path, O_WRONLY | $bits) or BAIL_OUT("Cannot open $path: $!");
            syswrite($fh, "z" x 4096) == 4096
              or BAIL_OUT("Short write to $path: $!");
            $after = request_counts();
            close $fh;
            is(request_delta($before, $after, 'afpSyncFork'), 1,
               "fuse_fsync: a write to an $name file sends one FPSyncFork");
        }
    }
    unlink $path;
}

# -----------------------------------------------------------------------
# fuse_resource_close: closing a written resource fork sends no flush.
# -----------------------------------------------------------------------
{
    my $path = "$mnt_dir/resource.txt";
    write_bytes($path, 16);
    my $before = settled_counts();
    open(my $fh, '>', "$mnt_dir/.AppleDouble/resource.txt")
      or BAIL_OUT("Cannot open the resource fork of $path: $!");
    print $fh "r" x 16;
    close $fh;
    my $after = counts_after_release($before, 1);
    is(flush_delta($before, $after), 0,
       'fuse_resource_close: closing a written resource fork sends no flush');
    unlink $path;
}

# -----------------------------------------------------------------------
# fuse_appledouble: ftruncate(2) through a resource fork's handle sets that
# fork's length. A FinderInfo pseudo-file's handle outlives a read and
# accepts an ftruncate(2).
# -----------------------------------------------------------------------
{
    my $path = "$mnt_dir/appledouble.txt";
    my $fork = "$mnt_dir/.AppleDouble/appledouble.txt";
    write_bytes($path, 16);
    sysopen(my $fh, $fork, O_WRONLY | O_CREAT | O_TRUNC) or BAIL_OUT("Cannot open the resource fork of $path: $!");
    syswrite($fh, "r" x 4096) == 4096
      or BAIL_OUT("Short write to the resource fork of $path: $!");
    ok(truncate($fh, 1024), 'fuse_appledouble: ftruncate(2) through a resource fork handle succeeds');
    close $fh;
    is(-s $fork, 1024, 'fuse_appledouble: the truncation sets the resource fork length');

    # O_DIRECT sends the second read to afpfsd too.
    sysopen($fh, "$fork.finderinfo", O_RDONLY | $O_DIRECT) or BAIL_OUT("Cannot open the FinderInfo of $path: $!");
    my $first = sysread($fh, my $info, 32);
    sysseek($fh, 0, 0);
    my $second = sysread($fh, $info, 32);
    ok(($first // 0) == 32 && ($second // 0) == 32 && $fh->sync,
       'fuse_appledouble: a FinderInfo pseudo-file reads twice through one handle and syncs');
    close $fh;

    sysopen($fh, "$fork.finderinfo", O_WRONLY) or BAIL_OUT("Cannot open the FinderInfo of $path: $!");
    ok(truncate($fh, 0), 'fuse_appledouble: ftruncate(2) of a FinderInfo pseudo-file succeeds');
    close $fh;
    unlink $path;
}

# -----------------------------------------------------------------------
# fuse_ftruncate: ftruncate(2) sets a fork's length whatever length its
# handle last recorded.
# -----------------------------------------------------------------------
{
    my $path = "$mnt_dir/ftruncate.txt";
    my $fork = "$mnt_dir/.AppleDouble/ftruncate.txt";
    write_bytes($path, 4096);
    sysopen(my $fh, $path, O_WRONLY) or BAIL_OUT("Cannot open $path: $!");
    ok(truncate($fh, 0), 'fuse_ftruncate: ftruncate(2) to 0 of a file opened without O_TRUNC succeeds');
    close $fh;
    ok(-z $path, 'fuse_ftruncate: the truncation empties the file opened without O_TRUNC');

    write_bytes($path, 4096);
    sysopen(my $first,  $path, O_RDWR)             or BAIL_OUT("Cannot open $path: $!");
    sysopen(my $second, $path, O_WRONLY | O_TRUNC) or BAIL_OUT("Cannot truncate $path: $!");
    close $second;
    truncate($first, 4096) or BAIL_OUT("Cannot ftruncate $path: $!");
    close $first;
    is(-s $path, 4096, 'fuse_ftruncate: ftruncate(2) restores the length another handle truncated');

    write_bytes($fork, 4096);
    sysopen($fh, $fork, O_WRONLY) or BAIL_OUT("Cannot open the resource fork of $path: $!");
    ok(truncate($fh, 0), 'fuse_ftruncate: ftruncate(2) to 0 of a written resource fork succeeds');
    close $fh;
    ok(-z $fork, 'fuse_ftruncate: the truncation empties the resource fork');
    unlink $path;
}

# -----------------------------------------------------------------------
# fuse_copy: coreutils cp uses copy_file_range, so a file copies on the
# server, with its resource fork, once its data fork reaches 512 KiB. Before
# Netatalk 4.5, the cap is 1 GiB for both forks together. Any other file goes
# through the client.
# -----------------------------------------------------------------------
SKIP: {
    skip 'the copy operation is 64-bit Linux, tested on x86_64 and aarch64', 16
      unless $copy_file_range_nr;
    my $src = "$mnt_dir/copy_large.src";
    my $dst = "$mnt_dir/copy_large.dst";
    write_bytes($src, 16 << 20);
    my $before = request_counts();
    is(system('cp', $src, $dst), 0, 'fuse_copy: cp of 16 MiB succeeds');
    my $after = request_counts();
    is(request_delta($before, $after, 'afpCopyFile'), 1,
       'fuse_copy: a 16 MiB copy is one FPCopyFile');
    is(request_delta($before, $after, 'afpReadExt'), 0,
       'fuse_copy: a 16 MiB copy reads nothing through the client');
    is(request_delta($before, $after, 'afpWriteExt'), 0,
       'fuse_copy: a 16 MiB copy writes nothing through the client');
    is(system('cmp', '-s', $src, $dst), 0,
       'fuse_copy: the 16 MiB copy matches its source');
    unlink $src, $dst;

    # The name is UTF-8 "copy_gate_é".
    $src = "$mnt_dir/copy_gate_\xc3\xa9.src";
    $dst = "$mnt_dir/copy_gate_\xc3\xa9.dst";
    write_bytes($src, 512 << 10);
    $before = request_counts();
    is(system('cp', $src, $dst), 0, 'fuse_copy: cp of 512 KiB succeeds');
    $after = request_counts();
    is(request_delta($before, $after, 'afpCopyFile'), 1,
       'fuse_copy: a 512 KiB copy under a non-ASCII name is one FPCopyFile');
    is(system('cmp', '-s', $src, $dst), 0,
       'fuse_copy: the 512 KiB copy matches its source');
    unlink $src, $dst;

    $src = "$mnt_dir/copy_small.src";
    $dst = "$mnt_dir/copy_small.dst";
    write_bytes($src, 508 << 10);
    $before = request_counts();
    is(system('cp', $src, $dst), 0, 'fuse_copy: cp of 508 KiB succeeds');
    $after = request_counts();
    is(request_delta($before, $after, 'afpCopyFile'), 0,
       'fuse_copy: a copy below 512 KiB stays on the client');
    is(request_delta($before, $after, 'afpReadExt'), 1,
       'fuse_copy: a 508 KiB copy is one FPReadExt');
    is(request_delta($before, $after, 'afpWriteExt'), 1,
       'fuse_copy: a 508 KiB copy is one FPWriteExt');
    is(system('cmp', '-s', $src, $dst), 0,
       'fuse_copy: the 508 KiB copy matches its source');
    unlink $src, $dst;

    $src = "$mnt_dir/copy_forked.src";
    $dst = "$mnt_dir/copy_forked.dst";
    write_bytes($src,                                    512 << 10);
    write_bytes("$mnt_dir/.AppleDouble/copy_forked.src", 4096);
    $before = request_counts();
    is(system('cp', $src, $dst), 0, 'fuse_copy: cp of a file with a resource fork succeeds');
    $after = request_counts();
    is(request_delta($before, $after, 'afpCopyFile'), 1,
       'fuse_copy: a file with a resource fork is one FPCopyFile');
    is(-s "$mnt_dir/.AppleDouble/copy_forked.dst", 4096,
       'fuse_copy: the copy on the server carries the resource fork');
    unlink $src, $dst;
}

# -----------------------------------------------------------------------
# fuse_copy_caps: a source of 256 MiB copies on the server. On Netatalk
# before 4.5, one above 1 GiB, in its data fork or both forks, goes through
# the client.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 5 unless $copy_file_range_nr;
    my $quantum = server_quantum();
    my $src     = "$mnt_dir/caps.src";
    my $dst     = "$mnt_dir/caps.dst";

    # Run counted_copy() of a whole sparse source with $resource bytes of
    # resource fork. A truncation sets the length without moving bytes.
    my $sparse_copy = sub {
        my ($size, $resource) = @_;
        sysopen(my $fh, $src, O_WRONLY | O_CREAT | O_TRUNC) or BAIL_OUT("Cannot create $src: $!");
        truncate($fh, $size)                                or BAIL_OUT("Cannot truncate $src: $!");
        close $fh;
        write_bytes("$mnt_dir/.AppleDouble/caps.src", $resource) if $resource;
        sysopen(my $in,  $src, O_RDONLY)                    or BAIL_OUT("Cannot open $src: $!");
        sysopen(my $out, $dst, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $dst: $!");
        my @counted = counted_copy($in, $out, $size, 0, 0);
        close $out;
        close $in;
        unlink $src, $dst;
        return @counted;
    };

    my ($copied, $before, $after) = $sparse_copy->(256 << 20, 0);
    ok($copied == 256 << 20 && request_delta($before, $after, 'afpCopyFile') == 1,
       'fuse_copy_caps: a source of 256 MiB is copied on the server');

  SKIP: {
        my ($major, $minor) = status_text() =~ /^\s+type: Netatalk(\d+)\.(\d+)/m;
        skip 'Netatalk 4.5 and later copy any size on the server', 4
          if defined $minor && ($major > 4 || ($major == 4 && $minor >= 5));

        for my $case (
                      [1025 << 20, 0,       'a source above 1 GiB in its data fork'],
                      [1023 << 20, 2 << 20, 'a source above 1 GiB in both forks']
        ) {
            my ($size, $resource, $what) = @$case;
            ($copied, $before, $after) = $sparse_copy->($size, $resource);
            is(offload_delta($before, $after), 0,
               "fuse_copy_caps: $what is not copied on the server");
            is($copied, $size < 16 * $quantum ? $size : 16 * $quantum,
               "fuse_copy_caps: $what copies at most sixteen quanta through the client");
        }
    }
}

# -----------------------------------------------------------------------
# fuse_copy_bound: one copy_file_range(2) through the client returns after
# sixteen quanta, and the caller continues from there.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 3 unless $copy_file_range_nr;
    my $quantum = server_quantum();
    my $src     = "$mnt_dir/bound.src";
    my $dst     = "$mnt_dir/bound.dst";
    write_bytes($src, 20 * $quantum);
    write_bytes($dst, 0);
    # The copy stays off the server because the handle below did not create
    # the destination, not because the creating handle's release is still
    # pending.
    settled_counts();
    sysopen(my $in,  $src, O_RDONLY) or BAIL_OUT("Cannot open $src: $!");
    sysopen(my $out, $dst, O_WRONLY) or BAIL_OUT("Cannot open $dst: $!");
    my $copied = copy_range($in, $out, 20 * $quantum);
    my $rest   = copy_range($in, $out, 20 * $quantum);
    close $out;
    close $in;
    is($copied, 16 * $quantum, 'fuse_copy_bound: one call copies sixteen quanta');
    is($rest,   4 * $quantum,  'fuse_copy_bound: the next call copies the rest from there');

    is(system('cmp', '-s', $src, $dst), 0, 'fuse_copy_bound: the copy matches its source');
    unlink $src, $dst;
}

# -----------------------------------------------------------------------
# fuse_copy_ranges: copies the server cannot make, of part of the source or
# at other offsets, go through the client to the offsets asked for. So does a
# whole source into a destination at another offset, with a second handle
# open or a resource fork, or written since its create.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 13 unless $copy_file_range_nr;
    my $src = "$mnt_dir/ranges.src";
    my $dst = "$mnt_dir/ranges.dst";
    write_bytes($src, 1 << 20);
    sysopen(my $in,  $src, O_RDONLY)                    or BAIL_OUT("Cannot open $src: $!");
    sysopen(my $out, $dst, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $dst: $!");
    my $before = request_counts();
    my $part   = copy_range($in, $out, 768 << 10, 0,        0);
    my $moved  = copy_range($in, $out, 32 << 10,  16 << 10, 768 << 10);
    my $after  = request_counts();
    close $out;
    is($part,  768 << 10, 'fuse_copy_ranges: a copy of part of the source copies that part');
    is($moved, 32 << 10,  'fuse_copy_ranges: a copy between other offsets copies its length');
    is(offload_delta($before, $after), 0,
       'fuse_copy_ranges: neither copy goes to the server');
    ok(same_bytes($src, $dst, 768 << 10),
       'fuse_copy_ranges: the part lands at the start of the destination');
    ok(same_bytes($src, $dst, 32 << 10, 16 << 10, 768 << 10),
       'fuse_copy_ranges: the other range lands at its destination offset');

    my $twice = "$mnt_dir/ranges.twice";
    sysopen(my $first,  $twice, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $twice: $!");
    sysopen(my $second, $twice, O_WRONLY)                    or BAIL_OUT("Cannot open $twice: $!");
    my $copied;
    ($copied, $before, $after) = counted_copy($in, $first, 1 << 20, 0, 0);
    close $second;
    close $first;
    is(offload_delta($before, $after), 0,
       'fuse_copy_ranges: a destination with a second handle open is not copied on the server');
    ok($copied == 1 << 20 && same_bytes($src, $twice, 1 << 20),
       'fuse_copy_ranges: the destination with a second handle open holds the copy');

    my $shifted = "$mnt_dir/ranges.shifted";
    sysopen(my $shift_out, $shifted, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $shifted: $!");
    ($copied, $before, $after) = counted_copy($in, $shift_out, 1 << 20, 0, 256 << 10);
    close $shift_out;
    is(offload_delta($before, $after), 0,
       'fuse_copy_ranges: a whole source to another destination offset is not copied on the server');
    ok($copied == 1 << 20 && same_bytes($src, $shifted, 1 << 20, 0, 256 << 10),
       'fuse_copy_ranges: the whole source lands at its destination offset');

    my $written = "$mnt_dir/ranges.written";
    sysopen(my $written_out, $written, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $written: $!");
    syswrite($written_out, "\xff" x (1200 << 10)) == 1200 << 10
      or BAIL_OUT("Short write to $written: $!");
    ($copied, $before, $after) = counted_copy($in, $written_out, 1 << 20, 0, 0);
    close $written_out;
    is(offload_delta($before, $after), 0,
       'fuse_copy_ranges: a destination written since its create is not copied on the server');
    ok($copied == 1 << 20 && same_bytes($src, $written, 1 << 20) && (-s $written) == (1200 << 10),
       'fuse_copy_ranges: the copy overwrites the written destination and keeps its tail');

    my $forked = "$mnt_dir/ranges.forked";
    sysopen(my $forked_out, $forked, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $forked: $!");
    set_resource_fork($forked, "r" x 4096);
    ($copied, $before, $after) = counted_copy($in, $forked_out, 1 << 20, 0, 0);
    close $forked_out;
    close $in;
    is(offload_delta($before, $after), 0,
       'fuse_copy_ranges: a destination with a resource fork is not copied on the server');
    ok($copied == 1 << 20 && (-s "$mnt_dir/.AppleDouble/ranges.forked") == 4096,
       'fuse_copy_ranges: the destination keeps its resource fork');
    unlink $src, $dst, $twice, $shifted, $written, $forked;
}

# -----------------------------------------------------------------------
# fuse_copy_metadata: a copy on the server keeps the mode given to its
# destination and takes the time of the copy as its modification time.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 4 unless $copy_file_range_nr;
    my $src = "$mnt_dir/metadata.src";
    my $dst = "$mnt_dir/metadata.dst";
    write_bytes($src, 512 << 10);
    chmod(0600, $src)                                   or BAIL_OUT("Cannot chmod $src: $!");
    utime(978307200, 978307200, $src)                   or BAIL_OUT("Cannot set the times of $src: $!");
    sysopen(my $in, $src, O_RDONLY)                     or BAIL_OUT("Cannot open $src: $!");
    sysopen(my $out, $dst, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $dst: $!");
    chmod(0640, $dst)                                   or BAIL_OUT("Cannot chmod $dst: $!");
    my $start = time;
    my ($copied, $before, $after) = counted_copy($in, $out, 512 << 10, 0, 0);
    close $out;
    close $in;
    is($copied, 512 << 10, 'fuse_copy_metadata: the copy copies the whole source');
    is(request_delta($before, $after, 'afpCopyFile'), 1,
       'fuse_copy_metadata: the copy goes to the server');
    # The sleep outlasts the kernel's attribute cache, so stat(2) asks the
    # server.
    sleep 2;
    my @st = stat $dst;
    is($st[2] & 07777, 0640, 'fuse_copy_metadata: the copy keeps the mode of its destination');
    ok($st[9] >= $start - 1, 'fuse_copy_metadata: the copy takes the time of the copy');
    unlink $src, $dst;
}

# -----------------------------------------------------------------------
# fuse_copy_sync: a copy into an O_SYNC or O_DSYNC file ends with a flush, as
# a write does.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 4 unless $copy_file_range_nr;
    my $src = "$mnt_dir/sync.src";
    write_bytes($src, 512 << 10);

    for my $flag ([O_SYNC, 'O_SYNC'], [O_DSYNC, 'O_DSYNC']) {
        my ($bits, $name) = @$flag;
        my $dst = "$mnt_dir/sync.$name";
        sysopen(my $in,  $src, O_RDONLY)                            or BAIL_OUT("Cannot open $src: $!");
        sysopen(my $out, $dst, O_WRONLY | O_CREAT | O_EXCL | $bits) or BAIL_OUT("Cannot create $dst: $!");
        my ($copied, $before, $after) = counted_copy($in, $out, 512 << 10, 0, 0);
        close $out;
        close $in;
        is($copied, 512 << 10, "fuse_copy_sync: the copy into an $name file succeeds");
        is(request_delta($before, $after, 'afpSyncFork'), 1,
           "fuse_copy_sync: the copy into an $name file sends one FPSyncFork");
        unlink $dst;
    }
    unlink $src;
}

# -----------------------------------------------------------------------
# fuse_copy_resource: a resource fork copies through the client, a read and a
# write per quantum.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 4 unless $copy_file_range_nr;
    my $src = "$mnt_dir/rsrc.src";
    my $dst = "$mnt_dir/rsrc.dst";
    write_bytes($src,                             0);
    write_bytes($dst,                             0);
    write_bytes("$mnt_dir/.AppleDouble/rsrc.src", 256 << 10);
    sysopen(my $in, "$mnt_dir/.AppleDouble/rsrc.src", O_RDONLY)
      or BAIL_OUT("Cannot open the resource fork of $src: $!");
    sysopen(my $out, "$mnt_dir/.AppleDouble/rsrc.dst", O_WRONLY)
      or BAIL_OUT("Cannot open the resource fork of $dst: $!");
    my ($copied, $before, $after) = counted_copy($in, $out, 256 << 10, 0, 0);
    close $out;
    close $in;
    is($copied, 256 << 10, 'fuse_copy_resource: the resource fork copies whole');
    is(request_delta($before, $after, 'afpReadExt'), 1,
       'fuse_copy_resource: 256 KiB of resource fork is one FPReadExt');
    is(request_delta($before, $after, 'afpWriteExt'), 1,
       'fuse_copy_resource: 256 KiB of resource fork is one FPWriteExt');
    ok(same_bytes("$mnt_dir/.AppleDouble/rsrc.src", "$mnt_dir/.AppleDouble/rsrc.dst", 256 << 10),
       'fuse_copy_resource: the copy matches its source');
    unlink $src, $dst;
}

# -----------------------------------------------------------------------
# fuse_copy_renamed: another session renames a destination or a source after
# its open and puts another file at its old name. The copy goes through the
# open fork, and the file at the old name stays as it is.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 8 unless $copy_file_range_nr;
    # A second mount is a second session, whose rename leaves this
    # session's open fork under the old name on the server.
    my $other = "$mnt_dir.other";
    mkdir $other unless -d $other;
    mount_or_bail('fuse_copy_renamed: a second mount succeeds', 'mount_afpfs', $AFP_AUTH_URL, $other);
    my $size   = 2 * server_quantum();
    my $src    = "$mnt_dir/renamed.src";
    my $dst    = "$mnt_dir/renamed.dst";
    my $kept   = "$mnt_dir/renamed.kept";
    my $before = request_counts();
    write_bytes($src, $size);
    sysopen(my $in,  $src, O_RDONLY)                    or BAIL_OUT("Cannot open $src: $!");
    sysopen(my $out, $dst, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $dst: $!");
    rename("$other/renamed.dst", "$other/renamed.kept") or BAIL_OUT("Cannot rename $other/renamed.dst: $!");
    write_bytes("$other/renamed.dst", 0);
    # A listing through this mount caches the new file under the old name
    # in this session's server.
    opendir(my $dh, $mnt_dir) or BAIL_OUT("Cannot list $mnt_dir: $!");
    my @listing = readdir $dh;
    closedir $dh;
    my $copied = copy_range($in, $out, $size);
    my $after  = request_counts();
    close $out;
    close $in;
    is($copied, $size, 'fuse_copy_renamed: copy_file_range copies the whole source');
    is(offload_delta($before, $after), 0,
       'fuse_copy_renamed: the renamed destination is not copied on the server');
    is(system('cmp', '-s', $src, $kept), 0, 'fuse_copy_renamed: the renamed destination holds the copy');
    ok(-e $dst && -z $dst, 'fuse_copy_renamed: the file at the old name stays empty');
    unlink $src, $dst, $kept;

    $src  = "$mnt_dir/renamed2.src";
    $dst  = "$mnt_dir/renamed2.dst";
    $kept = "$mnt_dir/renamed2.kept";
    write_bytes($src, $size);
    sysopen($in, $src, O_RDONLY)                          or BAIL_OUT("Cannot open $src: $!");
    sysopen($out, $dst, O_WRONLY | O_CREAT | O_EXCL)      or BAIL_OUT("Cannot create $dst: $!");
    rename("$other/renamed2.src", "$other/renamed2.kept") or BAIL_OUT("Cannot rename $other/renamed2.src: $!");
    open(my $other_src, '>', "$other/renamed2.src")       or BAIL_OUT("Cannot create $other/renamed2.src: $!");
    print $other_src "\xaa" x $size;
    close $other_src;
    opendir($dh, $mnt_dir) or BAIL_OUT("Cannot list $mnt_dir: $!");
    @listing = readdir $dh;
    closedir $dh;
    ($copied, $before, $after) = counted_copy($in, $out, $size, 0, 0);
    close $out;
    close $in;
    is(offload_delta($before, $after), 0,
       'fuse_copy_renamed: a renamed source is not copied on the server');
    ok($copied == $size && same_bytes($kept, $dst, $size),
       'fuse_copy_renamed: the copy holds the renamed source, not the file at its old name');
    unlink $src, $dst, $kept;
    is(system('afpc', 'fs', 'unmount', $other), 0, 'fuse_copy_renamed: the second mount unmounts');
    rmdir $other;
}

# -----------------------------------------------------------------------
# fuse_xattr_names: a name in the kernel's own namespaces is answered
# without a request, and a user. name or one without a namespace goes to
# the server. The kernel can ask for security.capability before each write
# and copy; the daemon's debug log shows when it does.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 11 unless $getxattr_nr;
    my $path = "$mnt_dir/xattr_names.txt";
    write_bytes($path, 0);
    my @kernel = xattr_calls($path, 'system.afpc_test');
    is($kernel[0], Errno::ENODATA(),    'fuse_xattr_names: getxattr(2) of system.afpc_test finds no attribute');
    is($kernel[1], Errno::EOPNOTSUPP(), 'fuse_xattr_names: setxattr(2) of system.afpc_test is not supported');
    is($kernel[2], Errno::ENODATA(),    'fuse_xattr_names: removexattr(2) of system.afpc_test finds no attribute');
    is($kernel[3], 0,                   'fuse_xattr_names: system.afpc_test sends no request');

    my @security = xattr_calls($path, 'security.afpc_test');
    is($security[0], Errno::ENODATA(), 'fuse_xattr_names: getxattr(2) of security.afpc_test finds no attribute');
    is($security[3], 0,                'fuse_xattr_names: security.afpc_test sends no request');

    for my $name ('user.afpc_test', 'afpc_test') {
        my @calls = xattr_calls($path, $name);
        is($calls[0], Errno::ENODATA(), "fuse_xattr_names: getxattr(2) of an absent $name finds no attribute");
        is($calls[3], 3,                "fuse_xattr_names: the three calls on $name send three requests");
    }

  SKIP: {
        skip $DEBUG_LOG_SKIP, 1 unless $AFP_FUSE_DEBUG_LOG;
        my $written = "$mnt_dir/xattr_names.bin";
        my $offset  = -s $AFP_FUSE_DEBUG_LOG;
        my $before  = request_counts();
        write_bytes($written, 4096);
        sysopen(my $in,  $written,        O_RDONLY)                    or BAIL_OUT("Cannot open $written: $!");
        sysopen(my $out, "$written.copy", O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $written.copy: $!");
        copy_range($in, $out, 4096, 0, 0) == 4096 or BAIL_OUT("Cannot copy $written: $!");
        close $out;
        close $in;
        my $after = request_counts();
        my $asked = () = daemon_log_since($offset) =~ m{\*\*\* getxattr /xattr_names\.bin\S*:security\.capability}g;
        unlink $written, "$written.copy";
        skip 'this kernel does not ask the filesystem for security.capability', 1 unless $asked;
        is(request_delta($before, $after, 'afpGetExtAttr'), 0,
           'fuse_xattr_names: a write and a copy_file_range(2) send no FPGetExtAttr');
    }
    unlink $path;
}

# -----------------------------------------------------------------------
# fuse_xattr_metadata: Netatalk's metadata attribute stays the server's
# under its user. name, without a request.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 4 unless $getxattr_nr;
    my $path = "$mnt_dir/xattr_metadata.txt";
    write_bytes($path, 0);
    my @meta = xattr_calls($path, 'user.org.netatalk.Metadata');
    is($meta[0], Errno::ENODATA(), 'fuse_xattr_metadata: getxattr(2) of user.org.netatalk.Metadata finds no attribute');
    is($meta[1], 0,                'fuse_xattr_metadata: setxattr(2) of user.org.netatalk.Metadata succeeds');
    is($meta[2], Errno::EACCES(),  'fuse_xattr_metadata: removexattr(2) of user.org.netatalk.Metadata is refused');
    is($meta[3], 0,                'fuse_xattr_metadata: user.org.netatalk.Metadata sends no request');
    unlink $path;
}

# -----------------------------------------------------------------------
# fuse_xattr_parent: an attribute call on a path whose parent the server
# does not have, as a resource fork's under .AppleDouble, fails with ENOENT.
# -----------------------------------------------------------------------
SKIP: {
    skip $SYSCALL_SKIP, 4 unless $getxattr_nr;
    my $path = "$mnt_dir/xattr_parent.txt";
    my $fork = "$mnt_dir/.AppleDouble/xattr_parent.txt";
    write_bytes($path, 0);
    my @parent = xattr_calls($fork, 'user.afpc_test');
    is($parent[0], Errno::ENOENT(), 'fuse_xattr_parent: getxattr(2) under .AppleDouble fails with ENOENT');
    is($parent[1], Errno::ENOENT(), 'fuse_xattr_parent: setxattr(2) under .AppleDouble fails with ENOENT');
    is($parent[2], Errno::ENOENT(), 'fuse_xattr_parent: removexattr(2) under .AppleDouble fails with ENOENT');
    is(errno_of(syscall($listxattr_nr, $fork, 0, 0)), Errno::ENOENT(),
       'fuse_xattr_parent: listxattr(2) under .AppleDouble fails with ENOENT');
    unlink $path;
}

unlink "$mnt_dir/sample.txt";
ok(!-e "$mnt_dir/sample.txt", 'fuse_auth: sample.txt removed');

unmount_or_bail('fuse_auth: cleanup unmount succeeds');

# -----------------------------------------------------------------------
# fuse_flush_on_close: with the mount option, the last close of a written
# file flushes it. Setting, emptying, or removing a resource fork as an
# extended attribute flushes it before the call returns.
# -----------------------------------------------------------------------
mount_or_bail(
              'fuse_flush_on_close: mount with flush_on_close succeeds',
              'mount_afpfs', '-o', 'flush_on_close', $AFP_AUTH_URL, $mnt_dir
);

{
    # The file opens without O_TRUNC: a truncation changes the fork as a
    # write does.
    my $path = "$mnt_dir/flush_on_close.txt";
    write_bytes($path, 0);
    my $before = settled_counts();
    sysopen(my $fh, $path, O_WRONLY) or BAIL_OUT("Cannot open $path: $!");
    close $fh;
    my $after = counts_after_release($before, 1);
    is(flush_delta($before, $after), 0,
       'fuse_flush_on_close: closing an unwritten file sends no flush');

    $before = $after;
    sysopen($fh, $path, O_WRONLY) or BAIL_OUT("Cannot open $path: $!");
    syswrite($fh, "x" x 4096) == 4096
      or BAIL_OUT("Short write to $path: $!");
    close $fh;
    $after = counts_after_release($before, 1);
    is(request_delta($before, $after, 'afpSyncFork'), 1,
       'fuse_flush_on_close: closing a written file sends one FPSyncFork');

    $before = $after;
    sysopen($fh, $path, O_WRONLY | O_TRUNC) or BAIL_OUT("Cannot open $path: $!");
    close $fh;
    $after = counts_after_release($before, 1);
    is(request_delta($before, $after, 'afpSyncFork'), 1,
       'fuse_flush_on_close: closing a file its open truncated sends one FPSyncFork');

    $before = $after;
    sysopen($fh, $path, O_WRONLY) or BAIL_OUT("Cannot open $path: $!");
    truncate($fh, 4096)           or BAIL_OUT("Cannot truncate $path: $!");
    close $fh;
    $after = counts_after_release($before, 1);
    is(request_delta($before, $after, 'afpSyncFork'), 1,
       'fuse_flush_on_close: closing a file truncated through its handle sends one FPSyncFork');

    $before = $after;
    open($fh, '>', "$path.new") or BAIL_OUT("Cannot create $path.new: $!");
    close $fh;
    $after = counts_after_release($before, 1);
    is(flush_delta($before, $after), 0,
       'fuse_flush_on_close: closing a file a create made, unwritten, sends no flush');
    unlink $path, "$path.new";
}

SKIP: {
    skip $SYSCALL_SKIP, 3 unless $setxattr_nr;
    my $path = "$mnt_dir/flush_on_close.rsrc";
    write_bytes($path, 0);
    my $before = settled_counts();
    set_resource_fork($path, "r" x 64);
    my $after = request_counts();
    is(request_delta($before, $after, 'afpSyncFork'), 1,
       'fuse_flush_on_close: a resource fork written as an extended attribute sends one FPSyncFork');

    $before = $after;
    set_resource_fork($path, '');
    $after = request_counts();
    is(request_delta($before, $after, 'afpSyncFork'), 1,
       'fuse_flush_on_close: a resource fork emptied as an extended attribute sends one FPSyncFork');

    set_resource_fork($path, "r" x 64);
    $before = request_counts();
    remove_resource_fork($path);
    $after = request_counts();
    is(request_delta($before, $after, 'afpSyncFork'), 1,
       'fuse_flush_on_close: a resource fork removed as an extended attribute sends one FPSyncFork');
    unlink $path;
}

unmount_or_bail('fuse_flush_on_close: unmount succeeds');

# -----------------------------------------------------------------------
# fuse_options_bound: mount_afpfs -o takes an option of up to 255 bytes,
# first or last in its list, and afpc fs mount -O 255 bytes of options. One
# byte more stops the command before it mounts.
# -----------------------------------------------------------------------
{
    my $most = 'x' x 255;
    mount_or_bail(
                  'fuse_options_bound: mount_afpfs -o takes a first and a last option of 255 bytes',
                  'mount_afpfs', '-o', "$most,$most", $AFP_AUTH_URL, $mnt_dir
    );
    unmount_or_bail('fuse_options_bound: the mount_afpfs -o mount unmounts');
    refused_mount(
                  'fuse_options_bound: mount_afpfs -o refuses a first option of 256 bytes',
                  'mount_afpfs', '-o', "x$most,ro", $AFP_AUTH_URL
    );
    refused_mount(
                  'fuse_options_bound: mount_afpfs -o refuses a last option of 256 bytes',
                  'mount_afpfs', '-o', "ro,x$most", $AFP_AUTH_URL
    );

    # Leading zeros pad attr_timeout=2 to 255 bytes; a cut value reads 0 and
    # a dropped one libfuse's default of 1.
    my $zeros  = '0' x (255 - length('attr_timeout=2'));
    my @mount  = ('afpc', 'fs', 'mount', '-u', $AFP_USER, '-p', $AFP_PASS, '-O');
    my $offset = $AFP_FUSE_DEBUG_LOG ? -s $AFP_FUSE_DEBUG_LOG : 0;
    mount_or_bail(
                  'fuse_options_bound: afpc fs mount -O takes 255 bytes of options',
                  @mount, "attr_timeout=${zeros}2", "$AFP_HOST:$AFP_VOL", $mnt_dir
    );

  SKIP: {
        skip $DEBUG_LOG_SKIP, 1 unless $AFP_FUSE_DEBUG_LOG;
        my $timeouts = fuse_init_timeouts($offset);
        skip 'the daemon logs no fuse init line', 1 unless defined $timeouts;
        is($timeouts, 'entry_timeout=1.000 negative_timeout=0.000 attr_timeout=2.000',
           'fuse_options_bound: libfuse reads attr_timeout=2 from the 255 bytes of -O');
    }
    unmount_or_bail('fuse_options_bound: the afpc fs mount -O mount unmounts');
    refused_mount(
                  'fuse_options_bound: afpc fs mount -O refuses 256 bytes of options',
                  @mount, "attr_timeout=0${zeros}2", "$AFP_HOST:$AFP_VOL"
    );
}

# -----------------------------------------------------------------------
# fuse_timeouts: mount_afpfs -o passes attr_timeout, entry_timeout and
# negative_timeout to libfuse, up to 255 bytes together. With the first two
# at zero, a stat(2) looks the name up and reads the attributes; a missing
# name's lookup lasts the negative timeout.
# -----------------------------------------------------------------------
# Leading zeros pad the timeouts to 255 bytes and keep negative_timeout at 30.
my $timeouts       = 'attr_timeout=0,entry_timeout=0,negative_timeout=';
my $timeout_zeros  = '0' x (255 - length("${timeouts}30"));
my $timeout_offset = $AFP_FUSE_DEBUG_LOG ? -s $AFP_FUSE_DEBUG_LOG : 0;
mount_or_bail(
              'fuse_timeouts: mount_afpfs -o takes 255 bytes of timeouts',
              'mount_afpfs', '-o', "${timeouts}${timeout_zeros}30", $AFP_AUTH_URL, $mnt_dir
);

SKIP: {
    skip $DEBUG_LOG_SKIP, 1 unless $AFP_FUSE_DEBUG_LOG;
    my $timeouts_read = fuse_init_timeouts($timeout_offset);
    skip 'the daemon logs no fuse init line', 1 unless defined $timeouts_read;
    is($timeouts_read, 'entry_timeout=0.000 negative_timeout=30.000 attr_timeout=0.000',
       'fuse_timeouts: libfuse reads the three timeouts from the 255 bytes');
}

SKIP: {
    skip 'these counts follow the Linux kernel caches', 2 unless $^O eq 'linux';
    my $path = "$mnt_dir/timeouts.txt";
    write_bytes($path, 4096);
    my $before = request_counts();
    stat $path for 1 .. 5;
    my $after = request_counts();
    cmp_ok(request_delta($before, $after, 'afpGetFileDirParms'), '>=', 10,
           'fuse_timeouts: five stat(2) of a file ask the server at least ten times');
    $before = $after;
    stat "$mnt_dir/timeouts.absent" for 1 .. 5;
    $after = request_counts();
    is(request_delta($before, $after, 'afpGetFileDirParms'), 1,
       'fuse_timeouts: five stat(2) of a missing name ask the server once');
    unlink $path;
}

unmount_or_bail('fuse_timeouts: unmount succeeds');
refused_mount(
              'fuse_timeouts: mount_afpfs -o refuses 256 bytes of timeouts',
              'mount_afpfs', '-o', "${timeouts}0${timeout_zeros}30", $AFP_AUTH_URL
);

# -----------------------------------------------------------------------
# fuse_flush_afp30: an AFP 3.0 session has no FPSyncFork and flushes with
# FPFlushFork, for fsync(2) and for afpc's -F or --flush-on-close.
# -----------------------------------------------------------------------
my @afp30_mount = ('afpc', 'fs', 'mount', '-u', $AFP_USER, '-p', $AFP_PASS, '-f', '3.0', '-F');
mount_or_bail(
              'fuse_flush_afp30: afpc mount on AFP 3.0 with -F succeeds',
              @afp30_mount, "$AFP_HOST:$AFP_VOL", $mnt_dir
);

{
    my $path   = "$mnt_dir/flush_afp30.txt";
    my $before = settled_counts();
    sysopen(my $fh, $path, O_WRONLY | O_CREAT | O_EXCL) or BAIL_OUT("Cannot create $path: $!");
    syswrite($fh, "x" x 4096) == 4096
      or BAIL_OUT("Short write to $path: $!");
    ok($fh->sync, 'fuse_flush_afp30: fsync(2) succeeds');
    syswrite($fh, "y" x 4096) == 4096
      or BAIL_OUT("Short write to $path: $!");
    close $fh;
    my $after = counts_after_release($before, 1);
    is(request_delta($before, $after, 'afpFlushFork'), 2,
       'fuse_flush_afp30: fsync(2) and the release of a written file each send FPFlushFork');
    is(request_delta($before, $after, 'afpSyncFork'), 0,
       'fuse_flush_afp30: no FPSyncFork goes to an AFP 3.0 session');
    unlink $path;
}

unmount_or_bail('fuse_flush_afp30: unmount succeeds');

# -----------------------------------------------------------------------
# fuse_truncate_path: truncate(2) by path sets the length asked, of a file
# and of its resource fork, and fails for a file the server will not open
# for writing, which keeps its length; a resource fork set as an extended
# attribute ends where the value does.
# -----------------------------------------------------------------------
mount_or_bail('fuse_truncate_path: authenticated mount succeeds', 'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);

{
    my $path = "$mnt_dir/truncate_path.txt";
    my $fork = "$mnt_dir/.AppleDouble/truncate_path.txt";
    write_bytes($path, 4096);
    ok(truncate($path, 100), 'fuse_truncate_path: truncate(2) by path to 100 succeeds');
    is(-s $path, 100, 'fuse_truncate_path: the file is 100 bytes long');

    write_bytes($fork, 4096);
    ok(truncate($fork, 1024), "fuse_truncate_path: truncate(2) of a resource fork's path succeeds");
    is(-s $fork, 1024, 'fuse_truncate_path: the resource fork is 1024 bytes long');

  SKIP: {
        skip $SYSCALL_SKIP, 1 unless $setxattr_nr;
        my $named = "$mnt_dir/truncate_xattr.txt";
        write_bytes($named, 0);
        set_resource_fork($named, 'r' x 64);
        set_resource_fork($named, 's' x 8);
        is(-s "$mnt_dir/.AppleDouble/truncate_xattr.txt", 8,
           'fuse_truncate_path: a shorter resource fork set as an extended attribute keeps no old tail');
        unlink $named;
    }

    write_bytes($path, 4096);
    chmod 0444, $path or BAIL_OUT("Cannot chmod $path: $!");
    my $truncated = truncate($path, 0);
    my $denied    = $!{EACCES};
    ok(!$truncated && $denied,
       'fuse_truncate_path: truncate(2) of a file the server will not open for writing fails with EACCES');
    # The sleep outlasts the kernel's attribute cache, so stat(2) asks the
    # server.
    sleep 2;
    is(-s $path, 4096, 'fuse_truncate_path: that file keeps its length');
    chmod 0644, $path;
    unlink $path;
}

unmount_or_bail('fuse_truncate_path: unmount succeeds');

# -----------------------------------------------------------------------
# fuse_unlink_busy: unlink(2) of a file another session holds open fails
# with EBUSY where the server refuses the delete, as Netatalk before 4.6
# does, and succeeds once that session closes the file.
# -----------------------------------------------------------------------
{
    my $other = "$mnt_dir.other";
    mkdir $other unless -d $other;
    mount_or_bail('fuse_unlink_busy: authenticated mount succeeds', 'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);
    mount_or_bail('fuse_unlink_busy: a second mount succeeds',      'mount_afpfs', $AFP_AUTH_URL, $other);
    my $path   = "$mnt_dir/unlink_busy.txt";
    my $before = request_counts();
    write_bytes($path, 16);
    # This mount's release goes first: libfuse renames a file it holds open
    # instead of deleting it.
    counts_after_release($before, 1);
    open(my $held, '<', "$other/unlink_busy.txt")
      or BAIL_OUT("Cannot open the file through the second mount: $!");
    my $removed = unlink $path;
    my $busy    = $!{EBUSY};
    close $held;

  SKIP: {
        skip 'the server deletes a file another session holds open', 2 if $removed && !-e $path;
        ok($busy, 'fuse_unlink_busy: unlink(2) of a file another session holds open fails with EBUSY');

        # The other session's release reaches its server after close(2).
        my $gone = 0;

        for (1 .. 50) {
            if (unlink $path) {
                $gone = 1;
                last;
            }

            select(undef, undef, undef, 0.1);
        }

        ok($gone, 'fuse_unlink_busy: unlink(2) succeeds once the other session closes the file');
    }

    is(system('afpc', 'fs', 'unmount', $other), 0, 'fuse_unlink_busy: the second mount unmounts');
    rmdir $other;
    unmount_or_bail('fuse_unlink_busy: unmount succeeds');
}

# -----------------------------------------------------------------------
# fuse_owner: without the common mapping every file shows the session's
# owner and group, and the server's own are unknown: chown(2) to those
# shown sends no change, and to another owner fails with EPERM.
# -----------------------------------------------------------------------
mount_or_bail('fuse_owner: authenticated mount succeeds', 'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);

{
    my $path = "$mnt_dir/owner.txt";
    write_bytes($path, 16);
    my @st     = stat $path;
    my $before = settled_counts();
    ok(chown($st[4], $st[5], $path), 'fuse_owner: chown(2) to the owner and group shown succeeds');
    my $after = request_counts();
    is(request_delta($before, $after, 'afpSetFileDirParms'), 0,
       'fuse_owner: chown(2) to the owner and group shown sends no change');
    my $changed = chown(65534, -1, $path);
    my $denied  = $!{EPERM};
  SKIP: {
        skip $SETATTR_SKIP, 1 if $^O eq 'darwin';
        ok(!$changed && $denied, 'fuse_owner: chown(2) to another owner fails with EPERM');
    }
    unlink $path;
}

unmount_or_bail('fuse_owner: unmount succeeds');

# -----------------------------------------------------------------------
# fuse_owner_common: with the common mapping chown(2) of a file and of a
# directory to an owner or a group the server keeps fails with EPERM, and
# the object keeps both.
# -----------------------------------------------------------------------
my @common_mount = ('afpc', 'fs', 'mount', '-u', $AFP_USER, '-p', $AFP_PASS, '-m', 'common');
mount_or_bail(
              'fuse_owner_common: a mount with the common mapping succeeds',
              @common_mount, "$AFP_HOST:$AFP_VOL", $mnt_dir
);

{
    my $file = "$mnt_dir/owner_common.txt";
    my $dir  = "$mnt_dir/owner_common.dir";
    write_bytes($file, 16);
    mkdir $dir or BAIL_OUT("Cannot create $dir: $!");

    for my $object (['file', $file], ['directory', $dir]) {
        my ($kind, $path) = @$object;
        my @st      = stat $path;
        my $changed = chown(65534, -1, $path);
        my $denied  = $!{EPERM};
        my $group   = chown(-1, 65534, $path);
        my $refused = $!{EPERM};
      SKIP: {
            skip $SETATTR_SKIP, 2 if $^O eq 'darwin';
            ok(!$changed && $denied,
               "fuse_owner_common: chown(2) of a $kind to an owner the server keeps fails with EPERM");
            ok(!$group && $refused,
               "fuse_owner_common: chown(2) of a $kind to a group the server keeps fails with EPERM");
        }
        # The sleep outlasts the kernel's attribute cache, so stat(2) asks
        # the server.
        sleep 2;
        my @now = stat $path;
        ok($now[4] == $st[4] && $now[5] == $st[5], "fuse_owner_common: the $kind keeps its owner and group");
    }

    unlink $file;
    rmdir $dir;
}

unmount_or_bail('fuse_owner_common: unmount succeeds');

# -----------------------------------------------------------------------
# fuse_chmod_other: chmod(2) of a file another user owns fails with EPERM,
# where Netatalk answers the change with success and keeps the mode.
# -----------------------------------------------------------------------
my $other_owned = "$mnt_dir/chmod_other.txt";
mount_or_bail('fuse_chmod_other: authenticated mount succeeds', 'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);
write_bytes($other_owned, 16);
chmod 0644, $other_owned or BAIL_OUT("Cannot chmod $other_owned: $!");
unmount_or_bail('fuse_chmod_other: unmount succeeds');
mount_or_bail('fuse_chmod_other: guest mount succeeds', 'mount_afpfs', $AFP_GUEST_URL, $mnt_dir);

{
    my $changed = chmod 0600, $other_owned;
    my $denied  = $!{EPERM};
    my $setuid  = chmod 04644, $other_owned;
    my $refused = $!{EPERM};
  SKIP: {
        skip $SETATTR_SKIP, 2 if $^O eq 'darwin';
        ok(!$changed && $denied,  'fuse_chmod_other: chmod(2) of a file another user owns fails with EPERM');
        ok(!$setuid  && $refused, 'fuse_chmod_other: chmod(2) that adds the set-user-ID bit alone fails with EPERM');
    }
    # The sleep outlasts the kernel's attribute cache, so stat(2) asks the
    # server.
    sleep 2;
    is((stat $other_owned)[2] & 07777, 0644, 'fuse_chmod_other: the file keeps its mode');
}

unmount_or_bail('fuse_chmod_other: guest unmount succeeds');
mount_or_bail('fuse_chmod_other: authenticated mount for the cleanup succeeds', 'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);
unlink $other_owned;
unmount_or_bail('fuse_chmod_other: unmount after the cleanup succeeds');

# -----------------------------------------------------------------------
# fuse_mtime_kept: a mode, owner or Finder info change keeps the
# modification time, as cp -p, rsync -a and tar -x expect after they set
# it. The server stamps its clock on a change that carries no time.
# -----------------------------------------------------------------------
mount_or_bail('fuse_mtime_kept: authenticated mount succeeds', 'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);

{
    my $when = 1000000000;
    my $file = "$mnt_dir/mtime_kept.txt";
    my $dir  = "$mnt_dir/mtime_kept.dir";
    write_bytes($file, 16);
    mkdir $dir or BAIL_OUT("Cannot create $dir: $!");

    utime $when, $when, $file or BAIL_OUT("Cannot set the time of $file: $!");
    chmod 0600, $file or BAIL_OUT("Cannot chmod $file: $!");
    is((stat $file)[9], $when, 'fuse_mtime_kept: chmod(2) of a file keeps its modification time');

    utime $when, $when, $dir or BAIL_OUT("Cannot set the time of $dir: $!");
    chmod 0750, $dir or BAIL_OUT("Cannot chmod $dir: $!");
    is((stat $dir)[9], $when, 'fuse_mtime_kept: chmod(2) of a directory keeps its modification time');

  SKIP: {
        skip $SYSCALL_SKIP, 1 unless $setxattr_nr;
        my $name = 'user.com.apple.FinderInfo';
        my $info = 'TEXTttxt' . ("\0" x 24);
        utime $when, $when, $file or BAIL_OUT("Cannot set the time of $file: $!");
        syscall($setxattr_nr, $file, $name, $info, length $info, 0) == 0
          or BAIL_OUT("Cannot set the Finder info of $file: $!");
        is((stat $file)[9], $when, 'fuse_mtime_kept: a Finder info change keeps the modification time');
    }

    # cp -p creates the copy without the group's and others' bits, then
    # gives it the source's mode.
    my $copy = "$mnt_dir/mtime_kept.copy";
    chmod 0640, $file or BAIL_OUT("Cannot chmod $file: $!");
    utime $when, $when, $file or BAIL_OUT("Cannot set the time of $file: $!");
    is(system('cp', '-p', $file, $copy), 0,     'fuse_mtime_kept: cp -p succeeds');
    is((stat $copy)[9],                  $when, 'fuse_mtime_kept: cp -p keeps the modification time');
    unlink $file, $copy;
    rmdir $dir;
}

unmount_or_bail('fuse_mtime_kept: unmount succeeds');
mount_or_bail(
              'fuse_mtime_kept: a mount with the common mapping succeeds',
              @common_mount, "$AFP_HOST:$AFP_VOL", $mnt_dir
);

{
    my $when = 1000000000;
    my $file = "$mnt_dir/mtime_kept.owner";
    write_bytes($file, 16);
    utime $when, $when, $file or BAIL_OUT("Cannot set the time of $file: $!");
    my $changed = chown(65534, -1, $file);
  SKIP: {
        skip $SETATTR_SKIP, 1 if $^O eq 'darwin';
        ok(!$changed, 'fuse_mtime_kept: chown(2) to an owner the server keeps fails');
    }
    # The sleep outlasts the kernel's attribute cache, so stat(2) asks the
    # server.
    sleep 2;
    is((stat $file)[9], $when, 'fuse_mtime_kept: that owner request keeps the modification time');
    unlink $file;
}

unmount_or_bail('fuse_mtime_kept: unmount of the common mount succeeds');

# -----------------------------------------------------------------------
# fuse_create_mode: a file or a directory created through the mount has the
# mode the create asked for, also a file without the owner's write
# permission, which the creator still writes through its descriptor.
# FPCreateFile and FPCreateDir carry no mode. A FIFO fails to create.
# -----------------------------------------------------------------------
mount_or_bail('fuse_create_mode: authenticated mount succeeds', 'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);

{
    my $old     = umask 077;
    my $private = "$mnt_dir/create_mode.private";
    my $before  = settled_counts();
    sysopen(my $fh, $private, O_WRONLY | O_CREAT | O_EXCL, 0600) or BAIL_OUT("Cannot create $private: $!");
    close $fh;
    my $after = counts_after_release($before, 1);
    is((stat $private)[2] & 07777, 0600, 'fuse_create_mode: a file created with mode 0600 has mode 0600');
    is(request_delta($before, $after, 'afpSetFileDirParms'), 1,
       'fuse_create_mode: the mode takes one FPSetFileDirParms');

    umask 0;
    my $readonly = "$mnt_dir/create_mode.readonly";
    sysopen($fh, $readonly, O_WRONLY | O_CREAT | O_EXCL, 0444) or BAIL_OUT("Cannot create $readonly: $!");
    my $written = syswrite($fh, 'data');
    close $fh;
    is($written,                    4,    'fuse_create_mode: the creator writes a file created with mode 0444');
    is((stat $readonly)[2] & 07777, 0444, 'fuse_create_mode: that file has mode 0444');

    umask 077;
    my $dir = "$mnt_dir/create_mode.dir";
    mkdir $dir, 0777 or BAIL_OUT("Cannot create $dir: $!");
    is((stat $dir)[2] & 0777, 0700, 'fuse_create_mode: a directory created under umask 077 has mode 0700');

  SKIP: {
        skip $SYSCALL_SKIP, 1 unless $mknodat_nr;
        my $node = "$mnt_dir/create_mode.node";
        # AT_FDCWD, and a regular file's type bits; libfuse makes the file
        # with fuse_create().
        syscall($mknodat_nr, -100, $node, 0100640, 0) == 0
          or BAIL_OUT("Cannot mknod $node: $!");
        is((stat $node)[2] & 07777, 0600, 'fuse_create_mode: mknod(2) under umask 077 gives mode 0600');
        unlink $node;
    }

    # AFP has no FIFOs.
    my $fifo    = "$mnt_dir/create_mode.fifo";
    my $made    = POSIX::mkfifo($fifo, 0644);
    my $refused = $!{EPERM};
    ok(!$made && $refused, 'fuse_create_mode: mkfifo(3) fails with EPERM');
    ok(!-e $fifo,          'fuse_create_mode: and leaves no file');
    unlink $fifo;

    umask $old;
    chmod 0644, $readonly;
    unlink $private, $readonly;
    rmdir $dir;
}

unmount_or_bail('fuse_create_mode: unmount succeeds');

# -----------------------------------------------------------------------
# fuse_mount_time: ten mounts in a row each complete within 5 s. Every
# mount closes its status connection from the connecting thread while the
# loop thread waits on it; a daemon that sleeps on that takes 10 s more.
# -----------------------------------------------------------------------
{
    my $slowest = 0;

    for my $round (1 .. 10) {
        my $start = Time::HiRes::time();
        mount_or_bail(
                      "fuse_mount_time: mount $round succeeds",
                      'mount_afpfs', $AFP_AUTH_URL, $mnt_dir
        );
        my $elapsed = Time::HiRes::time() - $start;
        $slowest = $elapsed if $elapsed > $slowest;
        unmount_or_bail("fuse_mount_time: unmount $round succeeds");
    }

    cmp_ok($slowest, '<', 5,
           sprintf('fuse_mount_time: the slowest of ten mounts took %.2f s', $slowest));
}

done_testing;
