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

my $AFP_URL      = 'afp://test_usr:test_pwd@localhost/afpfs_test';
my $work_dir     = tempdir(CLEANUP => 1);
my $test_file    = "afpcmd_test_$$.txt";
my $test_path    = "$work_dir/$test_file";
my $sidecar_path = "$work_dir/._$test_file";

# -----------------------------------------------------------------------
# batch_no_args: bare invocation prints usage and exits promptly
# -----------------------------------------------------------------------
{
    pipe(my $out_r, my $out_w) or BAIL_OUT("pipe: $!");

    my $pid = fork() // BAIL_OUT("fork: $!");
    if ($pid == 0) {
        close $out_r;
        open(STDOUT, '>&', $out_w) or die "dup stdout: $!";
        close $out_w;
        exec {'afpcmd'} 'afpcmd' or die "exec afpcmd: $!";
    }

    close $out_w;
    my ($out, $status, $error);
    {
        local $SIG{ALRM} = sub { die "bare afpcmd did not exit after 5s\n" };
        alarm 5;
        eval {
            local $/;
            $out = <$out_r> // '';
            waitpid($pid, 0) == $pid or die "waitpid: $!";
            $status = $?;
        };
        $error = $@;
        alarm 0;
    }
    close $out_r;

    if ($error) {
        kill 'KILL', $pid;
        waitpid($pid, 0);
        BAIL_OUT($error);
    }

    is($status, 1 << 8, 'batch_no_args: afpcmd exits 1');
    like($out, qr/--browse/,  'batch_no_args: usage includes --browse');
    like($out, qr/<afp url>/, 'batch_no_args: usage includes AFP URL syntax');
}

# -----------------------------------------------------------------------
# batch_upload
# -----------------------------------------------------------------------

open(my $fh, '>', $test_path) or BAIL_OUT("Cannot create test file: $!");
print $fh "Hello from afpcmd batch transfer test\nLine 2\nLine 3\n";
close $fh;

my $finder_info   = pack('C*', 1 .. 32);
my $resource_fork = join('', map { chr(($_ * 17) & 0xff) } 0 .. 8191);
my $appledouble =
    pack('NN', 0x00051607, 0x00020000)
  . ("\0" x 16)
  . pack('n',   2)
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
       'batch_download: FinderInfo and ResourceFork match');
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
# batch_overwrite: an upload over a longer file of the same name leaves the
# new contents alone, without the old file's tail.
# -----------------------------------------------------------------------
{
    my $name = "afpcmd_overwrite_$$.txt";
    my $path = "$work_dir/$name";
    open(my $long, '>', $path) or BAIL_OUT("Cannot create $path: $!");
    print $long "first line\nsecond line\nthird line\n";
    close $long;
    is(system('afpcmd', $path, $AFP_URL), 0, 'batch_overwrite: the first upload succeeds');
    open(my $short, '>', $path) or BAIL_OUT("Cannot rewrite $path: $!");
    print $short "short\n";
    close $short;
    is(system('afpcmd', $path, $AFP_URL), 0, 'batch_overwrite: the upload over it succeeds');
    unlink $path;
    is(system('afpcmd', "$AFP_URL/$name", $work_dir), 0, 'batch_overwrite: the download succeeds');
    my $got = '';

    if (open(my $read, '<', $path)) {
        $got = do { local $/; <$read> };
        close $read;
    }

    is($got, "short\n", 'batch_overwrite: the file holds the second upload alone');
    unlink $path;
    open(my $afpcmd, '|-', "afpcmd '$AFP_URL' > /dev/null 2>&1")
      or BAIL_OUT("Cannot run afpcmd: $!");
    print $afpcmd "rm $name\nquit\n";
    close $afpcmd;
}

# -----------------------------------------------------------------------
# batch_read_error: an upload whose local read fails exits non-zero. A read
# of /proc/self/mem at offset 0 fails with EIO.
# -----------------------------------------------------------------------
SKIP: {
    skip 'only Linux has /proc/self/mem', 2 unless $^O eq 'linux';
    my $out = qx(afpcmd /proc/self/mem '$AFP_URL' 2>&1);
    isnt($?, 0, 'batch_read_error: the upload exits non-zero');
    like($out, qr/Error reading local file/, 'batch_read_error: at the read');
    open(my $afpcmd, '|-', "afpcmd '$AFP_URL' > /dev/null 2>&1")
      or BAIL_OUT("Cannot run afpcmd: $!");
    print $afpcmd "rm mem\nquit\n";
    close $afpcmd;
}

# -----------------------------------------------------------------------
# recursive_macos_round_trip
# -----------------------------------------------------------------------

my $tree_name    = "afpcmd_tree_$$";
my $tree_path    = "$work_dir/$tree_name";
my $tree_file    = "$tree_path/payload";
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
is(
   system(
          'afpcmd', '-r', '--metadata=macos',
          "$AFP_URL/$tree_name", $tree_path
   ),
   0,
   'recursive_macos: directory download succeeds'
);
ok(-s $tree_sidecar, 'recursive_macos: downloaded sidecar is non-empty');
is(system('afpcmd', '-r', '--metadata=macos', $tree_path, $AFP_URL), 0,
   'recursive_macos: downloaded directory uploads without treating sidecar as a file');

done_testing;
