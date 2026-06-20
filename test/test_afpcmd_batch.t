#!/usr/bin/perl

# Batch-mode tests for afpcmd
# Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

use strict;
use warnings;

use Test::More;
use File::Path qw(remove_tree);
use File::Temp qw(tempdir);

my $AFP_URL  = 'afp://test_usr:test_pwd@localhost/afpfs_test';
my $work_dir = tempdir(CLEANUP => 1);
my $test_file = "afpcmd_test_$$.txt";
my $test_path = "$work_dir/$test_file";
my $sidecar_path = "$work_dir/._$test_file";

# -----------------------------------------------------------------------
# batch_upload
# -----------------------------------------------------------------------

open(my $fh, '>', $test_path) or BAIL_OUT("Cannot create test file: $!");
print $fh "Hello from afpcmd batch transfer test\nLine 2\nLine 3\n";
close $fh;

my $finder_info = pack('C*', 1 .. 32);
my $resource_fork = join('', map { chr(($_ * 17) & 0xff) } 0 .. 8191);
my $appledouble = pack('NN', 0x00051607, 0x00020000)
    . ("\0" x 16)
    . pack('n', 2)
    . pack('NNN', 9, 50, length($finder_info))
    . pack('NNN', 2, 82, length($resource_fork))
    . $finder_info
    . $resource_fork;
open(my $adh, '>:raw', $sidecar_path)
    or BAIL_OUT("Cannot create AppleDouble file: $!");
print $adh $appledouble;
close $adh;
chmod 0640, $test_path;
my $test_mtime = time() - 3600;
utime $test_mtime, $test_mtime, $test_path;

my $orig_cksum = `cksum "$test_path"`;
chomp $orig_cksum;
$orig_cksum =~ s/\s+\S+$//;    # strip filename, keep "CRC SIZE"

is(system('afpcmd', '--metadata=macos', $test_path, $AFP_URL), 0,
    'batch_upload: afpcmd exits 0');

# -----------------------------------------------------------------------
# batch_download
# -----------------------------------------------------------------------

unlink $test_path;
unlink $sidecar_path;
ok(!-e $test_path, 'batch_download: local copy removed before download');

is(system('afpcmd', '--metadata=macos', "$AFP_URL/$test_file", $work_dir), 0,
    'batch_download: afpcmd exits 0');

ok(-e $test_path, 'batch_download: file exists after download');

my $new_cksum = `cksum "$test_path"`;
chomp $new_cksum;
$new_cksum =~ s/\s+\S+$//;

is($new_cksum, $orig_cksum, 'batch_download: checksum matches original');
ok(-e $sidecar_path, 'batch_download: AppleDouble sidecar exists');
if (-e $sidecar_path) {
    open(my $read_ad, '<:raw', $sidecar_path)
        or BAIL_OUT("Cannot read downloaded AppleDouble file: $!");
    my $downloaded_ad = do { local $/; <$read_ad> };
    close $read_ad;
    is($downloaded_ad, $appledouble,
        'batch_download: FinderInfo and resource fork match');
}
if (-e $test_path) {
    is((stat($test_path))[2] & 07777, 0640,
        'batch_download: mode preserved');
    is((stat($test_path))[9], $test_mtime,
        'batch_download: modification time preserved');
} else {
    fail('batch_download: mode preserved');
    fail('batch_download: modification time preserved');
}

# -----------------------------------------------------------------------
# recursive_macos_round_trip
# -----------------------------------------------------------------------

my $tree_name = "afpcmd_tree_$$";
my $tree_path = "$work_dir/$tree_name";
my $tree_file = "$tree_path/payload";
my $tree_sidecar = "$tree_path/._payload";

mkdir $tree_path or BAIL_OUT("Cannot create recursive test directory: $!");
open(my $tree_fh, '>', $tree_file)
    or BAIL_OUT("Cannot create recursive test file: $!");
print $tree_fh "recursive metadata round trip\n";
close $tree_fh;
open(my $tree_adh, '>:raw', $tree_sidecar)
    or BAIL_OUT("Cannot create recursive AppleDouble file: $!");
print $tree_adh $appledouble;
close $tree_adh;

is(system('afpcmd', '-r', '--metadata=macos', $tree_path, $AFP_URL), 0,
    'recursive_macos: initial directory upload succeeds');

remove_tree($tree_path);
is(system('afpcmd', '-r', '--metadata=macos',
          "$AFP_URL/$tree_name", $tree_path), 0,
    'recursive_macos: directory download succeeds');
ok(-s $tree_sidecar, 'recursive_macos: downloaded sidecar is non-empty');
is(system('afpcmd', '-r', '--metadata=macos', $tree_path, $AFP_URL), 0,
    'recursive_macos: downloaded directory uploads without treating sidecar as a file');

done_testing;
