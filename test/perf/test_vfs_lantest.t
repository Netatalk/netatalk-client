#!/usr/bin/perl

# CSV layout and cleanup checks for vfs_lantest on a plain filesystem
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

@ARGV == 1 or BAIL_OUT('Usage: test_vfs_lantest.t VFS_LANTEST_PATH');
my ($tool) = @ARGV;
$tool = File::Spec->rel2abs($tool);
-x $tool or BAIL_OUT("vfs_lantest is not executable: $tool");

my $dir    = tempdir(CLEANUP => 1);
my $row_re = qr/^(.+),(\d+),([\d.]+),(\d+),(\d+),(\d+),(\d+)$/;

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
# usage: a missing directory and an unknown test number are refused
# -----------------------------------------------------------------------
my ($out, $rc) = run_tool('-c');
is($rc, 1, 'usage: no -P exits 1');
($out, $rc) = run_tool('-c', '-f', '18', '-P', $dir);
is($rc, 1, 'usage: test number out of range exits 1');

# -----------------------------------------------------------------------
# subset: tests 9 and 10 pull in the creation and deletion of the files
# -----------------------------------------------------------------------
($out, $rc) = run_tool('-c', '-D', '-n', '1', '-f', '9,10', '-P', $dir);
is($rc, 0, 'subset: run exits 0');
my @rows = grep {/$row_re/} @$out;
is(scalar @rows, 4, 'subset: creation and deletion rows are implied');
like($rows[0], qr/^Creating 2000 files/, 'subset: creation runs first');
like($rows[3], qr/^Deleting 2000 files/, 'subset: deletion runs last');
is(leftovers(), 0, 'subset: the test directory is removed');

# -----------------------------------------------------------------------
# full: every test, six iterations so the outlier drop runs, a 4 MiB large
# file; -D because a plain filesystem rejects O_DIRECT for the 1024-byte
# transfers
# -----------------------------------------------------------------------
($out, $rc) = run_tool('-c', '-D', '-n', '6', '-s', '4', '-P', $dir);
is($rc, 0, 'full: run exits 0');
is($out->[0], "Test,Time_ms,Time±,Min_ms,Max_ms,Median_ms,MB/s\n",
   'full: the CSV header is the first line');
@rows = grep {/$row_re/} @$out;
is(scalar @rows, 17, 'full: one row per test');

for my $row (@rows) {
    my ($name, $mean, $sd, $min, $max, $median) = $row =~ $row_re;
    $name =~ s/ \[.*\]$//;
    ok($min <= $mean && $mean <= $max && $min <= $median && $median <= $max,
       "full: spread is consistent for $name");
}

my $sum_rows = grep {/^Sum of all file ops = \d+,\d+,,,,,$/} @$out;
is($sum_rows, 1, 'full: the sum row has the seven columns');
my $aggregate_rows = grep {/^# Aggregate,Value$/} @$out;
is($aggregate_rows, 1, 'full: the aggregate block follows');
my $total_rows = grep {/^Total file ops,56082$/} @$out;
is($total_rows, 1, 'full: the file op total counts the 4 MiB large file in 1 MiB chunks');
my $average_rows = grep {/^Avg time per file op \(µs\),\d+$/} @$out;
is($average_rows, 1, 'full: the average per op is a whole number of microseconds');

is(leftovers(), 0, 'full: the test directory is removed');

done_testing;
