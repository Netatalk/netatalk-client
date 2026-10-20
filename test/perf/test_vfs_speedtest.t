#!/usr/bin/perl

# CSV layout and cleanup checks for vfs_speedtest on a plain filesystem
# Copyright (C) 2026 Andy Lemin (andylemin)
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

use strict;
use warnings;

use Test::More;
use File::Spec;
use File::Temp qw(tempdir);

@ARGV == 1 or BAIL_OUT('Usage: test_vfs_speedtest.t VFS_SPEEDTEST_PATH');
my ($tool) = @ARGV;
$tool = File::Spec->rel2abs($tool);
-x $tool or BAIL_OUT("vfs_speedtest is not executable: $tool");

my $dir      = tempdir(CLEANUP => 1);
my $ops      = qr/(?:Read|Write|Copy|ServerCopy)/;
my $run_re   = qr/^$ops,\d+,\d+\.\d{3},\d+,\d+\.\d\d$/;
my $stats_re = qr/^${ops}_(?:mean|median|stddev|min|max),,\d+\.\d{3},\d+,(?:\d+\.\d\d)?$/;
my $row_8k   = qr/^8192,0\.008,(?:\d+\.\d\d,){4}\d+\.\d,\d+\.\d$/;
my $row_1m   = qr/^1048576,1\.000,(?:\d+\.\d\d,){4}\d+\.\d,\d+\.\d$/;
my $header   = "file_size_bytes,file_size_mb,mean_throughput_mbs,median_throughput_mbs,"
  . "min_throughput_mbs,max_throughput_mbs,stddev_ms,mean_time_ms\n";
my @sweep = ('-c', '-D', '-n', '3', '-z', '0.0078125,1', '-f', 'Read,Write,Copy,ServerCopy', '-P', $dir);

# Run the tool and return its stdout lines and exit code; stderr passes
# through to the harness.
sub run_tool {
    my (@args) = @_;
    open(my $fh, '-|', $tool, @args) or BAIL_OUT("cannot run $tool: $!");
    my @out = <$fh>;
    close $fh;
    my $status = $?;
    return (\@out, ($status & 127) ? 128 + ($status & 127) : $status >> 8);
}

sub leftovers {
    opendir(my $dh, $dir) or BAIL_OUT("opendir $dir: $!");
    my @names = grep { !/^\.\.?$/ } readdir $dh;
    closedir $dh;
    return scalar @names;
}

# -----------------------------------------------------------------------
# usage: a missing directory and an unknown operation are refused
# -----------------------------------------------------------------------
my ($out, $rc) = run_tool('-c');
is($rc, 1, 'usage: no -P exits 1');
($out, $rc) = run_tool('-c', '-f', 'Move', '-P', $dir);
is($rc, 1, 'usage: unknown operation exits 1');

# -----------------------------------------------------------------------
# single: one size, Write only, two measured runs after the warmup
# -----------------------------------------------------------------------
($out, $rc) = run_tool('-c', '-D', '-d', '1', '-n', '2', '-P', $dir);
is($rc, 0, 'single: run exits 0');
is($out->[0], "test_name,iteration,file_size_mb,microseconds,throughput_mbs\n",
   'single: the CSV header is the first line');
my @runs = grep {/$run_re/} @$out;
is(scalar @runs, 2, 'single: two Write runs recorded');
like($runs[0], qr/^Write,1,1\.000,/, 'single: the warmup run is left out');
my $summary_rows = grep {/^# File Size Performance Summary/} @$out;
is($summary_rows, 0, 'single: one size prints no summary block');

is(leftovers(), 0, 'single: the test directory is removed');

# -----------------------------------------------------------------------
# sweep: four operations over two sizes, three measured runs each
# -----------------------------------------------------------------------
($out, $rc) = run_tool(@sweep);
is($rc, 0, 'sweep: run exits 0');
@runs = grep {/$run_re/} @$out;
is(scalar @runs, 24, 'sweep: 4 operations x 2 sizes x 3 runs');
my @stats = grep {/$stats_re/} @$out;
is(scalar @stats, 40, 'sweep: five statistics rows per operation and size');

for my $op (qw(Read Write Copy ServerCopy)) {
    my ($i) = grep { $out->[$_] eq "# File Size Performance Summary for $op\n" } 0 .. $#$out;
    ok(defined $i, "sweep: summary block for $op");
    is($out->[$i + 1], $header, "sweep: $op summary header");
    like($out->[$i + 2], $row_8k, "sweep: $op row for 8 KiB");
    like($out->[$i + 3], $row_1m, "sweep: $op row for 1 MiB");
}

is(leftovers(), 0, 'sweep: the test directory is removed');

done_testing;
