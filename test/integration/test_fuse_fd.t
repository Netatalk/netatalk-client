#!/usr/bin/perl

# Netatalk Client FUSE client tests: a mount on a descriptor of /dev/fuse
# Copyright (C) 2026 Andy Lemin (andylemin)
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

# The entrypoint mounts /dev/fuse, open as fd 9, on $AFP_TEST_FUSE_FD_DIR as a
# container's parent would, and runs this suite unprivileged with no manager.

use strict;
use warnings;

use Test::More;
use POSIX       qw(WNOHANG);
use Time::HiRes ();

my $AFP_USER = $ENV{AFP_TEST_USER}     // 'test_usr';
my $AFP_PASS = $ENV{AFP_TEST_PASSWORD} // 'test_pwd';
my $fd_dir   = $ENV{AFP_TEST_FUSE_FD_DIR};
my $log      = "/tmp/afpc-foreground.$$.log";

plan skip_all => 'needs /dev/fuse mounted on descriptor 9 at AFP_TEST_FUSE_FD_DIR'
  unless defined $fd_dir && -e '/dev/fd/9';

# Wait up to $seconds for $check to return true, and return its last value.
sub within {
    my ($seconds, $check) = @_;
    my $deadline = Time::HiRes::time() + $seconds;
    my $result;

    until ($result = $check->()) {
        last if Time::HiRes::time() >= $deadline;
        Time::HiRes::sleep(0.05);
    }

    return $result;
}

# Test whether the mount on $fd_dir answers within $seconds. A lookup on a
# mount nobody serves blocks, so it runs in a child.
sub served {
    my ($seconds) = @_;
    my $pid = fork() // BAIL_OUT("Cannot fork: $!");

    if ($pid == 0) {
        POSIX::_exit(opendir(my $dh, $fd_dir) ? 0 : 1);
    }

    my $status;
    my $ended = within($seconds, sub { waitpid($pid, WNOHANG) == $pid and defined($status = $?) });
    return $status == 0 ? 1 : 0 if $ended;
    kill 'KILL', $pid;
    waitpid($pid, 0);
    return 0;
}

# Return what afpc has written.
sub afpc_output {
    open(my $fh, '<', $log) or return '';
    local $/;
    return <$fh> // '';
}

# -----------------------------------------------------------------------
# fuse_fd_not_fuse: a mount point /dev/fd/N whose descriptor is not a FUSE
# device, a pipe here, is refused at once, before libfuse reads it.
# -----------------------------------------------------------------------
{
    pipe(my $reader, my $writer) or BAIL_OUT("Cannot make a pipe: $!");
    my $refused = fork() // BAIL_OUT("Cannot fork afpc: $!");

    if ($refused == 0) {
        close $writer;
        open(STDIN,  '<&', $reader)  or die "stdin: $!";
        open(STDOUT, '>',  $log)     or die "stdout: $!";
        open(STDERR, '>&', \*STDOUT) or die "stderr: $!";
        exec(
              'afpc', 'fs', 'mount', '--foreground', '--user', $AFP_USER, '--pass', $AFP_PASS,
              'localhost:afpfs_test', '/dev/fd/0'
        ) or die "exec afpc: $!";
    }

    close $reader;
    my $status;
    ok(within(5, sub { waitpid($refused, WNOHANG) == $refused and defined($status = $?) }),
       'fuse_fd_not_fuse: afpc ends within 5 s while the pipe stays open');
    isnt($status, 0, 'fuse_fd_not_fuse: afpc exits with a status other than 0');
    like(afpc_output(), qr{is not an open FUSE device}, 'fuse_fd_not_fuse: the daemon says why');
    close $writer;

    if (waitpid($refused, WNOHANG) == 0) {
        kill 'KILL', $refused;
        waitpid($refused, 0);
    }
}

# -----------------------------------------------------------------------
# fuse_fd_mount: afpc fs mount --foreground serves /dev/fd/N with a daemon of
# its own and unmounts at a SIGTERM to afpc's group, not at an ignored SIGHUP.
# -----------------------------------------------------------------------
my $afpc = fork() // BAIL_OUT("Cannot fork afpc: $!");

if ($afpc == 0) {
    POSIX::setpgid(0, 0) or die "setpgid: $!";
    $SIG{HUP} = 'IGNORE';
    open(STDOUT, '>',  $log)     or die "stdout: $!";
    open(STDERR, '>&', \*STDOUT) or die "stderr: $!";
    exec(
          'afpc', 'fs', 'mount', '--foreground', '--user', $AFP_USER, '--pass', $AFP_PASS,
          'localhost:afpfs_test', '/dev/fd/9'
    ) or die "exec afpc: $!";
}

# afpc and its daemon hold the descriptor from here on.
POSIX::close(9);

SKIP: {
    ok(served(10), 'fuse_fd_mount: the mount on /dev/fd/9 is served')
      or do { diag(afpc_output()); skip 'the mount is not served', 10 };
    my $file = "$fd_dir/fd_mount.txt";
    open(my $out, '>', $file) or BAIL_OUT("Cannot create $file: $!");
    print $out "through the descriptor\n";
    close $out               or BAIL_OUT("Cannot write $file: $!");
    open(my $in, '<', $file) or BAIL_OUT("Cannot open $file: $!");
    my $text = <$in>;
    close $in;
    is($text, "through the descriptor\n", 'fuse_fd_mount: a file written through the mount reads back');
    ok(unlink($file), 'fuse_fd_mount: the file is removed');
    chomp(my @daemons = `pgrep -P $afpc afpfsd`);
    is(scalar @daemons,         1, 'fuse_fd_mount: the daemon is a child of afpc');
    is(waitpid($afpc, WNOHANG), 0, 'fuse_fd_mount: afpc waits while the mount runs');
    my $status_text = qx{afpc fs status /dev/fd/9 2>&1};
    my $status_rc   = $?;
    is($status_rc, 0, 'fuse_fd_mount: afpc fs status of the mount point succeeds') or diag($status_text);
    like($status_text, qr/afpfs_test/, 'fuse_fd_mount: the status is the daemon\'s, which names the volume');
    kill 'HUP', $afpc;
    Time::HiRes::sleep(1);
    ok(served(2) && waitpid($afpc, WNOHANG) == 0, 'fuse_fd_mount: a SIGHUP afpc was started ignoring leaves the mount');
    kill 'TERM', -$afpc;
    my $status;
    ok(within(5, sub { waitpid($afpc, WNOHANG) == $afpc and defined($status = $?) }),
       'fuse_fd_mount: afpc ends within 5 s of a SIGTERM to its process group');
    is($status, 0, 'fuse_fd_mount: afpc exits with status 0') or diag(afpc_output());
    like( afpc_output(), qr{Volume /dev/fd/9 unmounted},
          'fuse_fd_mount: the SIGTERM unmounts the volume');
}

if (waitpid($afpc, WNOHANG) == 0) {
    kill 'KILL', $afpc;
    waitpid($afpc, 0);
}

unlink $log;
done_testing;
