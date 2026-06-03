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
use Cwd qw(getcwd);

my $mnt_dir = getcwd() . '/afpfs_mnt';

# -----------------------------------------------------------------------
# prepare test environment: ensure mount directory exists, start afpfsd
# -----------------------------------------------------------------------
mkdir $mnt_dir unless -d $mnt_dir;
ok(-d $mnt_dir, 'prepare: mount directory exists');

is(system('afpfsd', '--manager'), 0, 'prepare: afpfsd daemon started');

# -----------------------------------------------------------------------
# fuse_auth: authenticated mount
# -----------------------------------------------------------------------
sleep 1;
is(system('mount_afpfs', 'afp://test_usr:test_pwd@localhost/afpfs_test', $mnt_dir), 0,
    'fuse_auth: authenticated mount succeeds');

open(my $wfh, '>', "$mnt_dir/sample.txt")
    or BAIL_OUT("Cannot write to mounted share: $!");
print $wfh "You should read this back\n";
close $wfh;

open(my $rfh, '<', "$mnt_dir/sample.txt")
    or BAIL_OUT("Cannot read from mounted share: $!");
my $content = do { local $/; <$rfh> };
close $rfh;
like($content, qr/^You should read this back$/m,
    'fuse_auth: file content readable after write');

is(system('afp_client', 'unmount', $mnt_dir), 0,
    'fuse_auth: authenticated unmount succeeds');

# -----------------------------------------------------------------------
# fuse_auth: guest mount
# -----------------------------------------------------------------------
sleep 1;
is(system('mount_afpfs', 'afp://localhost/afpfs_test', $mnt_dir), 0,
    'fuse_auth: guest mount succeeds');

ok(-f "$mnt_dir/sample.txt",
    'fuse_auth: guest mount shows previously written file');

open(my $gfh, '<', "$mnt_dir/sample.txt")
    or BAIL_OUT("Cannot read file on guest mount: $!");
my $guest_content = do { local $/; <$gfh> };
close $gfh;
like($guest_content, qr/^You should read this back$/m,
    'fuse_auth: guest mount file content matches');

is(system('afp_client', 'unmount', $mnt_dir), 0,
    'fuse_auth: guest unmount succeeds');

# -----------------------------------------------------------------------
# fuse_auth: authenticated mount cleanup
# -----------------------------------------------------------------------
sleep 1;
is(system('mount_afpfs', 'afp://test_usr:test_pwd@localhost/afpfs_test', $mnt_dir), 0,
    'fuse_auth: cleanup mount succeeds');

open(my $cfh, '<', "$mnt_dir/sample.txt")
    or BAIL_OUT("Cannot read file on cleanup mount: $!");
my @lines = <$cfh>;
close $cfh;
like($lines[0], qr/^You should read this back$/,
    'fuse_auth: cleanup mount file content matches');
is(scalar @lines, 1, 'fuse_auth: file has exactly one line');

unlink "$mnt_dir/sample.txt";
ok(!-e "$mnt_dir/sample.txt", 'fuse_auth: sample.txt removed');

is(system('afp_client', 'unmount', $mnt_dir), 0,
    'fuse_auth: cleanup unmount succeeds');

done_testing;
