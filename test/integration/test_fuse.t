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
use Fcntl        qw(O_RDONLY O_WRONLY O_RDWR O_CREAT O_EXCL O_TRUNC O_SYNC O_DSYNC);
use Getopt::Long qw(GetOptions);
use IO::Handle;

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
my $removexattr_nr     = syscall_nr(x86_64 => 197, aarch64 => 14);
my $SYSCALL_SKIP       = 'system calls by number need a 64-bit Perl on x86_64 or aarch64 Linux';

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

done_testing;
