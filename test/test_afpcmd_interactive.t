#!/usr/bin/perl

# Interactive-mode tests for afpcmd
# Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

use strict;
use warnings;

use Test::More;
use File::Temp qw(tempfile);

my $AFP_HOST     = 'localhost';
my $AFP_SERVER   = 'afpfs_testsrv';
my $AFP_VOL      = 'afpfs_test';
my $AFP_USER     = 'test_usr';
my $AFP_PASS     = 'test_pwd';
my $AFP_PASS_TMP = 'test_pwd_tmp';
my $AFP_URL      = "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}/${AFP_VOL}";

# Feed newline-separated commands to afpcmd via stdin and return combined
# stdout+stderr.  Uses fork+exec to avoid any shell quoting issues.
sub afpcmd_pipe {
    my ($url, @cmds) = @_;
    my $input = join("\n", @cmds) . "\n";
    my $timeout = $ENV{AFPCMD_TEST_TIMEOUT} // 15;

    pipe(my $out_r, my $out_w) or die "pipe: $!";
    pipe(my $in_r,  my $in_w)  or die "pipe: $!";

    my $pid = fork() // die "fork: $!";
    if ($pid == 0) {
        # Child: wire up stdin/stdout/stderr, then exec afpcmd
        close $out_r;
        close $in_w;
        open(STDIN,  '<&', $in_r)  or die "dup stdin: $!";
        open(STDOUT, '>&', $out_w) or die "dup stdout: $!";
        open(STDERR, '>&', $out_w) or die "dup stderr: $!";
        close $in_r;
        close $out_w;
        exec('afpcmd', $url) or die "exec: $!";
        exit 1;
    }

    # Parent: write commands then read all output
    close $out_w;
    close $in_r;
    print $in_w $input;
    close $in_w;

    my $output;
    my $read_error;
    {
        local $SIG{ALRM} = sub { die "timeout\n" };
        local $/;
        alarm $timeout;
        eval { $output = <$out_r>; };
        $read_error = $@;
        alarm 0;
    }

    if ($read_error) {
        kill 'TERM', $pid;
        waitpid($pid, 0);
        BAIL_OUT("afpcmd timed out after ${timeout}s running: "
            . join('; ', @cmds));
    }

    close $out_r;
    waitpid($pid, 0);
    return $output // '';
}

sub pagination_setup_commands {
    my ($dir, $count) = @_;
    return (
        "mkdir $dir",
        map { sprintf('touch %s/page%03d', $dir, $_) } 0 .. ($count - 1),
    );
}

# -----------------------------------------------------------------------
# test_help: help and ? alias both list known commands
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL, 'help', '?', 'quit');
    like($out, qr/get/,      'test_help: get entry');
    like($out, qr/Retrieve/, 'test_help: get description');
    like($out, qr/put/,      'test_help: put entry');
    like($out, qr/Send/,     'test_help: put description');
}

# -----------------------------------------------------------------------
# test_status: status prints server name; df prints volume statistics
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL, 'status', 'df', 'quit');
    like($out, qr/afpfs_testsrv/,                     'test_status: server name');
    like($out, qr/Filesystem statistics for volume:/,  'test_status: df header');
    like($out, qr/Total space:/,                       'test_status: df total');
}

# -----------------------------------------------------------------------
# test_navigation: pwd shows root, ls lists entries, cd enters/leaves dirs
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL,
        'pwd',
        'mkdir navtest',
        'ls',
        'cd navtest',
        'pwd',
        'cd ..',
        'pwd',
        'rmdir navtest',
        'quit');
    like($out, qr/Now in directory \//,        'test_navigation: pwd at root');
    like($out, qr/navtest/,                    'test_navigation: ls shows navtest');
    like($out, qr/Now in directory \/navtest/, 'test_navigation: pwd in navtest');
}

# -----------------------------------------------------------------------
# test_ls_pagination_continue: ls prompts and continues to the next page
# -----------------------------------------------------------------------
{
    my $dir = "pagetest_continue_$$";
    local $ENV{AFPCMD_TEST_TIMEOUT} = $ENV{AFPCMD_TEST_TIMEOUT} // 60;
    my $out = afpcmd_pipe($AFP_URL,
        pagination_setup_commands($dir, 257),
        "ls $dir",
        '',
        'quit');

    like($out,
        qr/256 entries shown; press Enter for next page, or q then Enter to quit/,
        'test_ls_pagination_continue: prompt after first page');
    like($out, qr/^-.* page000$/m,
        'test_ls_pagination_continue: first page shown');
    like($out, qr/^-.* page256$/m,
        'test_ls_pagination_continue: next page shown');

    afpcmd_pipe($AFP_URL, "rm -r $dir", 'quit');
}

# -----------------------------------------------------------------------
# test_ls_pagination_quit: q stops ls pagination before the next page
# -----------------------------------------------------------------------
{
    my $dir = "pagetest_quit_$$";
    local $ENV{AFPCMD_TEST_TIMEOUT} = $ENV{AFPCMD_TEST_TIMEOUT} // 60;
    my $out = afpcmd_pipe($AFP_URL,
        pagination_setup_commands($dir, 257),
        "ls $dir",
        'q',
        'quit');

    like($out,
        qr/256 entries shown; press Enter for next page, or q then Enter to quit/,
        'test_ls_pagination_quit: prompt advertises q');
    like($out, qr/^-.* page255$/m,
        'test_ls_pagination_quit: first page reaches page255');
    unlike($out, qr/^-.* page256$/m,
        'test_ls_pagination_quit: q suppresses next page');

    afpcmd_pipe($AFP_URL, "rm -r $dir", 'quit');
}

# -----------------------------------------------------------------------
# test_local_navigation: lpwd shows cwd; lcd changes it; bad path errors
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL,
        'lpwd',
        'lcd /tmp',
        'lpwd',
        'lcd /nonexistent_dir_xyz',
        'quit');
    like($out, qr/Now in local directory/,       'test_local_navigation: lpwd');
    like($out, qr{Now in local directory /tmp},  'test_local_navigation: lcd /tmp');
    like($out, qr/Changing directories:/,        'test_local_navigation: lcd bad path error');
}

# -----------------------------------------------------------------------
# test_file_lifecycle: touch, cat, chmod, mv, cp, rm
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL,
        'touch lctest.txt',
        'ls',
        'cat lctest.txt',
        'chmod 600 lctest.txt',
        'ls',
        'chmod 644 lctest.txt',
        'ls',
        'mv lctest.txt lcrenamed.txt',
        'ls',
        'cp lcrenamed.txt lccopy.txt',
        'ls',
        'rm lcrenamed.txt',
        'rm lccopy.txt',
        'quit');
    like($out, qr/lctest\.txt/,          'test_file_lifecycle: touch creates file');
    like($out, qr/\-rw\-\-\-\-\-\-\-/,  'test_file_lifecycle: chmod 600');
    like($out, qr/\-rw\-r\-\-r\-\-/,    'test_file_lifecycle: chmod 644');
    like($out, qr/lcrenamed\.txt/,       'test_file_lifecycle: mv rename');
    like($out, qr/Copied \d+ bytes/,     'test_file_lifecycle: cp reports bytes');
    like($out, qr/lccopy\.txt/,          'test_file_lifecycle: cp creates copy');
}

# -----------------------------------------------------------------------
# test_directory_lifecycle: mkdir, rmdir, rmdir error cases
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL,
        'mkdir dltest',
        'ls',
        'touch dltest/inside.txt',
        'ls dltest',
        'rmdir dltest',
        'rm dltest/inside.txt',
        'rmdir dltest',
        'rmdir nonexistent_dir_xyz',
        'quit');
    like($out, qr/dltest/,                    'test_directory_lifecycle: mkdir');
    like($out, qr/inside\.txt/,               'test_directory_lifecycle: ls inside dir');
    like($out, qr/Directory not empty/, 'test_directory_lifecycle: rmdir not empty error');
    like($out, qr/Directory not found/, 'test_directory_lifecycle: rmdir nonexistent error');
}

# -----------------------------------------------------------------------
# test_command_recursive_flag: -r is accepted after interactive commands
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL,
        'mkdir rflag-src',
        'mkdir rflag-src/subdir',
        'touch rflag-src/subdir/file.txt',
        'chmod -r 755 rflag-src',
        'cp -r rflag-src/ rflag-src/subdir/copy',
        'mkdir rflag-collision',
        'mkdir rflag-collision/rflag-src',
        'touch rflag-collision/rflag-src/subdir',
        'cp -r rflag-src rflag-collision',
        'cp -r rflag-src rflag-copy',
        'ls rflag-copy/subdir',
        'mv rflag-copy rflag-moved',
        'rm -r rflag-src',
        'rm -r rflag-collision',
        'rm -r rflag-moved',
        'quit');
    unlike($out, qr/expecting format:/,
        'test_command_recursive_flag: command-local -r parses');
    like($out, qr/Cannot copy a directory into itself/,
        'test_command_recursive_flag: trailing slash self-copy is rejected');
    like($out, qr/Copy target exists and is not a directory:/,
        'test_command_recursive_flag: directory-to-file collision is rejected');
    like($out, qr/file\.txt/,
        'test_command_recursive_flag: cp recursively copied nested file');
    like($out, qr/Deleted directory: rflag-src/,
        'test_command_recursive_flag: rm recursively removed source');
    like($out, qr/Deleted directory: rflag-moved/,
        'test_command_recursive_flag: moved destination removed recursively');
}

# -----------------------------------------------------------------------
# test_get_put: put a local file, verify with cat, get it back, check content
# -----------------------------------------------------------------------
{
    my $tmpfile = "/tmp/afpcmd_gptest_$$.txt";
    open(my $fh, '>', $tmpfile) or BAIL_OUT("Cannot create temp file: $!");
    print $fh "get_put test content line 1\nget_put test content line 2\n";
    close $fh;
    (my $tmpname = $tmpfile) =~ s{.*/}{};

    my $out = afpcmd_pipe($AFP_URL,
        'lcd /tmp',
        "put $tmpname",
        "cat $tmpname",
        "get $tmpname",
        "rm $tmpname",
        'quit');

    like($out, qr/Transfer complete\./,          'test_get_put: put transfer complete');
    like($out, qr/get_put test content line 1/,  'test_get_put: cat shows content');
    like($out, qr/Transfer complete\./,          'test_get_put: get transfer complete');

    ok(-f $tmpfile, 'test_get_put: downloaded file exists');
    if (-f $tmpfile) {
        open(my $rfh, '<', $tmpfile) or BAIL_OUT("Cannot read downloaded file: $!");
        my $content = do { local $/; <$rfh> };
        close $rfh;
        like($content, qr/get_put test content line 1/,
            'test_get_put: downloaded file content matches');
        unlink $tmpfile;
    }
}

# -----------------------------------------------------------------------
# test_metadata_commands: round-trip FinderInfo, ResourceFork, and xattr
# -----------------------------------------------------------------------
{
    my $base = "/tmp/afpcmd_metadata_$$";
    my $finder_in = "$base.finder.in";
    my $finder_out = "$base.finder.out";
    my $resource_in = "$base.resource.in";
    my $resource_out = "$base.resource.out";
    my $xattr_in = "$base.xattr.in";
    my $xattr_out = "$base.xattr.out";

    open(my $fh, '>:raw', $finder_in) or BAIL_OUT("finder input: $!");
    print $fh pack('C*', 1 .. 32);
    close $fh;
    open($fh, '>:raw', $resource_in) or BAIL_OUT("resource input: $!");
    print $fh join('', map { chr(($_ * 29) & 0xff) } 0 .. 9999);
    close $fh;
    open($fh, '>:raw', $xattr_in) or BAIL_OUT("xattr input: $!");
    print $fh "binary\0xattr\xffvalue";
    close $fh;

    my $old_umask = umask 0000;
    my $out = afpcmd_pipe($AFP_URL,
        'touch metadata-test',
        "finderinfo set metadata-test $finder_in",
        "finderinfo get metadata-test $finder_out",
        "resourcefork set metadata-test $resource_in",
        "resourcefork get metadata-test $resource_out",
        "xattr set metadata-test org.netatalk.afpcmd-test $xattr_in",
        'xattr list metadata-test',
        "xattr get metadata-test org.netatalk.afpcmd-test $xattr_out",
        'xattr remove metadata-test org.netatalk.afpcmd-test',
        'finderinfo remove metadata-test',
        'resourcefork remove metadata-test',
        'rm metadata-test',
        'quit');
    umask $old_umask;

    unlike($out, qr/failed:/, 'test_metadata_commands: commands succeed');
    like($out, qr/org\.netatalk\.afpcmd-test/,
        'test_metadata_commands: xattr appears in list');

    for my $pair ([$finder_in, $finder_out, 'FinderInfo'],
                  [$resource_in, $resource_out, 'ResourceFork'],
                  [$xattr_in, $xattr_out, 'xattr']) {
        if (!-e $pair->[1]) {
            fail('test_metadata_commands: ' . $pair->[2] . ' output exists');
            next;
        }
        open(my $left, '<:raw', $pair->[0]) or BAIL_OUT("read $pair->[0]: $!");
        open(my $right, '<:raw', $pair->[1]) or BAIL_OUT("read $pair->[1]: $!");
        my $left_data = do { local $/; <$left> };
        my $right_data = do { local $/; <$right> };
        close $left;
        close $right;
        is($right_data, $left_data,
            'test_metadata_commands: ' . $pair->[2] . ' round trip');
        is((stat($pair->[1]))[2] & 07777, 0600,
            'test_metadata_commands: ' . $pair->[2] . ' output is private');
    }

    unlink $finder_in, $finder_out, $resource_in, $resource_out,
           $xattr_in, $xattr_out;
}

# -----------------------------------------------------------------------
# test_exit_reconnect: exit detaches volume; ls lists volumes; cd reattaches
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL,
        'exit',
        'ls',
        "cd $AFP_VOL",
        'quit');
    like($out, qr/Detached from volume/, 'test_exit_reconnect: exit detaches');
    like($out, qr/Available volumes on/, 'test_exit_reconnect: ls shows volumes');
    like($out, qr/Attached to volume/,   'test_exit_reconnect: cd reattaches');
}

# -----------------------------------------------------------------------
# test_passwd: change to temp password, verify, change back, verify
# -----------------------------------------------------------------------
{
    my $url_orig = "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}/${AFP_VOL}";
    my $url_tmp  = "afp://${AFP_USER}:${AFP_PASS_TMP}\@${AFP_HOST}/${AFP_VOL}";

    my $out = afpcmd_pipe($url_orig,
        'passwd',
        $AFP_PASS,
        $AFP_PASS_TMP,
        $AFP_PASS_TMP,
        'quit');

    if ($out =~ /Password change failed: not supported by the current authentication method\./) {
        pass('test_passwd: skipped (UAM does not support password change)');
    } else {
        like($out, qr/Password changed successfully\./, 'test_passwd: change to temp password');

        afpcmd_pipe($url_tmp, 'quit');   # verify temp password works (no assertion needed)

        my $out2 = afpcmd_pipe($url_tmp,
            'passwd',
            $AFP_PASS_TMP,
            $AFP_PASS,
            $AFP_PASS,
            'quit');
        like($out2, qr/Password changed successfully\./, 'test_passwd: revert to original password');

        afpcmd_pipe($url_orig, 'quit');  # verify original password restored
    }
}

# -----------------------------------------------------------------------
# test_disconnect: disconnect from server
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe($AFP_URL,
        'disconnect');
    like($out, qr/Disconnected from $AFP_SERVER/, 'test_disconnect: disconnect from server');
}

done_testing;
