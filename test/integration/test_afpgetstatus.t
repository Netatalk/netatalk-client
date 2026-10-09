#!/usr/bin/perl

# Tests for afpgetstatus
# Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

use strict;
use warnings;

use Test::More;

my $AFP_HOST = 'localhost';

# Run afpgetstatus and capture stdout+stderr combined.
sub afpgetstatus_run {
    my ($arg) = @_;

    pipe(my $out_r, my $out_w) or die "pipe: $!";

    my $pid = fork() // die "fork: $!";
    if ($pid == 0) {
        close $out_r;
        open(STDOUT, '>&', $out_w) or die "dup stdout: $!";
        open(STDERR, '>&', $out_w) or die "dup stderr: $!";
        close $out_w;
        exec('afpgetstatus', $arg) or die "exec: $!";
        exit 1;
    }

    close $out_w;
    local $/;
    my $output = <$out_r>;
    close $out_r;
    waitpid($pid, 0);
    return ($output // '', $?);
}

# -----------------------------------------------------------------------
# test_getstatus: query server at localhost and validate output sections
# -----------------------------------------------------------------------
{
    my ($out, $status) = afpgetstatus_run($AFP_HOST);

    is($status, 0, 'getstatus: exits 0');

    like( $out, qr/AFP response from/,
          'getstatus: response header present');

    like( $out, qr/Server name:\s+afpfs_testsrv/,
          'getstatus: server name is afpfs_testsrv');

    like( $out, qr/Server type:/,
          'getstatus: server type present');

    like( $out, qr/AFP versions:/,
          'getstatus: AFP versions section present');

    like( $out, qr/UAMs:/,
          'getstatus: UAMs section present');

    like( $out, qr/No User Authent/,
          'getstatus: guest UAM listed');

    like( $out, qr/DHX2/,
          'getstatus: DHX2 UAM listed');

    like( $out, qr/Flags:/,
          'getstatus: Flags section present');

    like( $out, qr/Signature:/,
          'getstatus: Signature section present');

    like( $out, qr/Shared volumes:/,
          'getstatus: Shared volumes section present');

    like( $out, qr/\bafpfs_test\b/,
          'getstatus: afpfs_test volume listed');
}

# -----------------------------------------------------------------------
# test_getstatus_url: same query using afp:// URL prefix
# -----------------------------------------------------------------------
{
    my ($out, $status) = afpgetstatus_run("afp://$AFP_HOST");

    is($status, 0, 'getstatus_url: exits 0 with afp:// prefix');

    like( $out, qr/Server name:\s+afpfs_testsrv/,
          'getstatus_url: server name matches');

    like( $out, qr/\bafpfs_test\b/,
          'getstatus_url: afpfs_test volume listed');
}

# -----------------------------------------------------------------------
# test_getstatus_help: -h flag prints usage and exits 0
# -----------------------------------------------------------------------
{
    pipe(my $out_r, my $out_w) or die "pipe: $!";
    my $pid = fork() // die "fork: $!";
    if ($pid == 0) {
        close $out_r;
        open(STDOUT, '>&', $out_w) or die "dup stdout: $!";
        open(STDERR, '>&', $out_w) or die "dup stderr: $!";
        close $out_w;
        exec('afpgetstatus', '-h') or die "exec: $!";
        exit 1;
    }
    close $out_w;
    local $/;
    my $out = <$out_r>;
    close $out_r;
    waitpid($pid, 0);

    like($out, qr/Usage/i, 'getstatus_help: usage text present');
}

done_testing;
