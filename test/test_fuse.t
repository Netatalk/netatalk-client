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
use Getopt::Long qw(GetOptions);

my $AFP_USER = $ENV{AFP_TEST_USER} // 'test_usr';
my $AFP_PASS = $ENV{AFP_TEST_PASSWORD} // 'test_pwd';
my $AFP_HOST = 'localhost';
my $AFP_VOL  = 'afpfs_test';
my $mnt_dir = getcwd() . '/afpfs_mnt';

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

sub mount_or_bail {
    my ($description, @cmd) = @_;
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

is(system('afpfsd', '--manager'), 0, 'prepare: afpfsd daemon started');

# -----------------------------------------------------------------------
# fuse_auth: authenticated mount
# -----------------------------------------------------------------------
sleep 1;
mount_or_bail('fuse_auth: authenticated mount succeeds',
    'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);

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

# -----------------------------------------------------------------------
# fuse_resume: authenticated suspend/resume keeps mounted volume usable
# -----------------------------------------------------------------------
is(system('afp_client', 'suspend', $mnt_dir), 0,
    'fuse_resume: authenticated suspend succeeds');

open(my $status_fh, '-|', 'afp_client', 'status', $mnt_dir)
    or BAIL_OUT("Cannot run afp_client status: $!");
my $status = do { local $/; <$status_fh> };
my $status_close_ok = close $status_fh;
my $status_rc = $?;
ok($status_close_ok, 'fuse_resume: status command exits successfully')
    or diag('afp_client status exited with '
        . ($status_rc == -1 ? "close error: $!" : 'status ' . ($status_rc >> 8)));
like($status, qr/connection: .*disconnected/,
    'fuse_resume: status reports disconnected server after suspend');

is(system('afp_client', 'resume', $mnt_dir), 0,
    'fuse_resume: authenticated resume succeeds without new password');

open(my $resume_rfh, '<', "$mnt_dir/sample.txt")
    or BAIL_OUT("Cannot read file after resume: $!");
my $resume_content = do { local $/; <$resume_rfh> };
close $resume_rfh;
like($resume_content, qr/^You should read this back$/m,
    'fuse_resume: existing file readable after resume');

open(my $resume_wfh, '>', "$mnt_dir/resume.txt")
    or BAIL_OUT("Cannot write file after resume: $!");
print $resume_wfh "Resume write survived reconnect\n";
close $resume_wfh;

open(my $resume_check_fh, '<', "$mnt_dir/resume.txt")
    or BAIL_OUT("Cannot read resume check file: $!");
my $resume_check = do { local $/; <$resume_check_fh> };
close $resume_check_fh;
like($resume_check, qr/^Resume write survived reconnect$/m,
    'fuse_resume: new file writable after resume');

unlink "$mnt_dir/resume.txt";
ok(!-e "$mnt_dir/resume.txt", 'fuse_resume: resume check file removed');

is(system('afp_client', 'unmount', $mnt_dir), 0,
    'fuse_auth: authenticated unmount succeeds');

# -----------------------------------------------------------------------
# fuse_auth: guest mount
# -----------------------------------------------------------------------
sleep 1;
mount_or_bail('fuse_auth: guest mount succeeds',
    'mount_afpfs', $AFP_GUEST_URL, $mnt_dir);

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
mount_or_bail('fuse_auth: cleanup mount succeeds',
    'mount_afpfs', $AFP_AUTH_URL, $mnt_dir);

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
