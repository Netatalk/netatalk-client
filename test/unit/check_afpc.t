#!/usr/bin/perl

# Black-box parser and invocation-name checks for afpc
# Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
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

@ARGV == 2 && ($ARGV[1] eq 'fuse' || $ARGV[1] eq 'no-fuse')
  or BAIL_OUT('Usage: check_afpc.t AFPC_PATH fuse|no-fuse');
my ($afpc, $build) = @ARGV;
$afpc = File::Spec->rel2abs($afpc);
-x $afpc or BAIL_OUT("afpc is not executable: $afpc");

# Capture combined stdout+stderr without a shell. Bound both reading and
# waiting for the child, and reap it if the command times out.
sub afpc_run {
    my (@command) = @_;

    pipe(my $out_r, my $out_w) or BAIL_OUT("pipe: $!");
    my $pid = fork() // BAIL_OUT("fork: $!");
    if ($pid == 0) {
        close $out_r;
        open(STDOUT, '>&', $out_w) or die "dup stdout: $!";
        open(STDERR, '>&', $out_w) or die "dup stderr: $!";
        close $out_w;
        exec {$command[0]} @command or die "exec: $!";
    }

    close $out_w;
    my ($out, $status, $error);
    {
        local $SIG{ALRM} = sub {
            die "afpc timed out after 5s running: @command\n";
        };
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

    return ($out, $status);
}

sub afpc_expect {
    my ($name, $code, $text, @command) = @_;
    my ($out, $status) = afpc_run(@command);

    is($status, $code << 8, "$name: exits $code") or diag($out);
    like($out, qr/\Q$text\E/, "$name: diagnostic");
}

# -----------------------------------------------------------------------
# parser: missing arguments, unknown commands, invalid namespace arguments
# -----------------------------------------------------------------------
afpc_expect('no_args', 2, 'Usage:', $afpc);
afpc_expect('unknown_command', 2, 'unknown command', $afpc, 'discover-extra');
afpc_expect('sl_status_extra_arg', 2, 'Usage:',
            $afpc, 'sl', 'status', 'mountpoint');

# -----------------------------------------------------------------------
# invocation: reject unsupported executable names
# -----------------------------------------------------------------------
my $directory = tempdir(CLEANUP => 1);
my $unknown   = "$directory/not-afpc";
symlink($afpc, $unknown) or BAIL_OUT("Cannot create afpc symlink: $!");
afpc_expect('unsupported_name', 2, 'unsupported invocation name',
            $unknown,           'help');

# -----------------------------------------------------------------------
# fuse_dispatch: diagnostics depend on the configured FUSE support
# -----------------------------------------------------------------------
if ($build eq 'fuse') {
    my $url = "$directory/mount_afpfs";
    symlink($afpc, $url) or BAIL_OUT("Cannot create mount_afpfs symlink: $!");

    afpc_expect('mount_namespace', 2, "Try 'afpc fs mount'.", $afpc, 'mount');
    afpc_expect('status_namespace', 2,
                "Try 'afpc fs status' or 'afpc sl status'.", $afpc, 'status');
    afpc_expect('unknown_fs_command', 2,    'afpc fs <command>',
                $afpc,                'fs', 'status-extra');
    afpc_expect('mount_url_dispatch', 2, "use 'afpc discover'",
                $url, 'discover');
} else {
    afpc_expect('mount_without_fuse', 2,
                'unavailable because FUSE support was not built', $afpc, 'mount');
    afpc_expect('status_namespace', 2, "Try 'afpc sl status'.", $afpc, 'status');
    afpc_expect(
                'fs_without_fuse', 2,
                'namespace is unavailable because FUSE support was not built',
                $afpc, 'fs', 'status'
    );
}

done_testing;
