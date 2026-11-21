#!/usr/bin/perl

# CSV layout, text table and cleanup checks for vfs_lantest on a plain
# filesystem
# Copyright (C) 2026 Andy Lemin (andylemin)
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

use strict;
use warnings;

use Test::More;
use Encode qw(decode);
use File::Spec;
use File::Temp qw(tempdir);

@ARGV == 1 or BAIL_OUT('Usage: test_vfs_lantest.t VFS_LANTEST_PATH');
my ($tool) = @ARGV;
$tool = File::Spec->rel2abs($tool);
-x $tool or BAIL_OUT("vfs_lantest is not executable: $tool");

my $dir     = tempdir(CLEANUP => 1);
my $time_re = qr/\d+\.\d{3}/;
my $row_re  = qr/^(.+),($time_re),($time_re),($time_re),($time_re),($time_re),(\d+)$/;

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

my $mean_total   = 0;
my $median_total = 0;
my %fractions;

for my $row (@rows) {
    my ($name, $mean, $sd, $min, $max, $median, $mbs) = $row =~ $row_re;
    $name =~ s/ \[.*\]$//;
    ok($min <= $mean && $mean <= $max && $min <= $median && $median <= $max,
       "full: spread is consistent for $name");
    ok($sd <= $max - $min + 0.002, "full: the deviation fits the spread for $name");
    my $us = sprintf('%.0f', $mean * 1000);
    is($mbs, $name =~ /one large file$/ ? int(4_000_000 / $us) : 0, "full: MB/s is 4 MiB over the mean for $name");
    $mean_total   += $mean;
    $median_total += $median;
    # A time under 1 ms may be the 1 µs floor, so only longer times prove
    # the microseconds.
    my %column = (mean => $mean, min => $min, max => $max, median => $median);
    $fractions{$_}++ for grep { $column{$_} >= 1 && $column{$_} !~ /\.000$/ } keys %column;
}

ok($fractions{$_}, "full: the $_ column keeps its microseconds") for qw(mean min max median);
my @sums = map { /^Sum of all file ops = \d+,($time_re),,,,,$/ ? $1 : () } @$out;
is(scalar @sums, 1, 'full: the sum row has the seven columns');
ok(@sums && abs($sums[0] - $mean_total) < 0.0005, 'full: the sum row totals the means');
my $aggregate_rows = grep {/^# Aggregate,Value$/} @$out;
is($aggregate_rows, 1, 'full: the aggregate block follows');
my $total_rows = grep {/^Total file ops,56082$/} @$out;
is($total_rows, 1, 'full: the file op total counts the 4 MiB large file in 1 MiB chunks');
my @averages = map { /^Avg time per file op \(µs\),(\d+)$/ ? $1 : () } @$out;
is(scalar @averages, 1, 'full: the average per op is a whole number of microseconds');
my @medians = map { /^Total median time \(ms\),($time_re)$/ ? $1 : () } @$out;
ok(@medians && abs($medians[0] - $median_total) < 0.0005, 'full: the total median time totals the medians');
ok(@averages && @medians && abs($averages[0] - $medians[0] * 1000 / 56082) <= 0.5001,
   'full: the average per op is the total median time over the op count');

is(leftovers(), 0, 'full: the test directory is removed');

# -----------------------------------------------------------------------
# text: the table lines up although "±" and "×" take two bytes each
# -----------------------------------------------------------------------
($out, $rc) = run_tool('-D', '-n', '1', '-s', '4', '-f', '1,4', '-P', $dir);
is($rc, 0, 'text: run exits 0');
my @table  = grep {/^(?:Test |-{66} |Writing one large file|Create 2000 dirs tree)/} @$out;
my %widths = map  { length(decode('UTF-8', s/\n\z//r)) => 1 } @table;
is(scalar @table,        5, 'text: the header, two rules and two test rows');
is(scalar(keys %widths), 1, 'text: every table line has one width in columns');

done_testing;
