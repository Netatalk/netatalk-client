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
use File::Path qw(make_path remove_tree);
use File::Temp qw(tempfile);
use IO::Select;
use IO::Socket::UNIX;
use POSIX       qw(mkfifo);
use Time::HiRes ();

my $AFP_HOST       = 'localhost';
my $AFP_SERVER     = 'afpfs_testsrv';
my $AFP_VOL        = 'afpfs_test';
my $AFP_VOL_NOUNIX = 'afpfs_test_nounix';
my $AFP_USER       = 'test_usr';
my $AFP_PASS       = 'test_pwd';
my $AFP_PASS_TMP   = 'test_pwd_tmp';
my $AFP_URL        = "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}/${AFP_VOL}";

# Feed newline-separated commands to afpcmd via stdin and return combined
# stdout+stderr.  Uses fork+exec to avoid any shell quoting issues.
sub afpcmd_pipe {
    my ($url, @cmds) = @_;
    my $input   = join("\n", @cmds) . "\n";
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
        kill 'KILL', $pid;
        waitpid($pid, 0);
        BAIL_OUT("afpcmd timed out after ${timeout}s running: " . join('; ', @cmds));
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
    like($out, qr/^\s+afpOpenVol: 1$/m,               'test_status: request counts list the volume open');
    like($out, qr/Filesystem statistics for volume:/, 'test_status: df header');
    like($out, qr/Total space:/,                      'test_status: df total');
}

# -----------------------------------------------------------------------
# test_navigation: pwd shows root, ls lists entries, cd enters/leaves dirs
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'pwd',
                          'mkdir navtest',
                          'ls',
                          'cd navtest',
                          'pwd',
                          'cd ..',
                          'pwd',
                          'rmdir navtest',
                          'quit'
    );
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
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          pagination_setup_commands($dir, 257),
                          "ls $dir",
                          '',
                          'quit'
    );

    like(
          $out,
          qr/256 entries shown; press Enter for next page, or q then Enter to quit/,
          'test_ls_pagination_continue: prompt after first page'
    );
    like( $out, qr/^-.* page000$/m,
          'test_ls_pagination_continue: first page shown');
    like( $out, qr/^-.* page256$/m,
          'test_ls_pagination_continue: next page shown');

    afpcmd_pipe($AFP_URL, "rm -r $dir", 'quit');
}

# -----------------------------------------------------------------------
# test_ls_pagination_quit: q stops ls pagination before the next page
# -----------------------------------------------------------------------
{
    my $dir = "pagetest_quit_$$";
    local $ENV{AFPCMD_TEST_TIMEOUT} = $ENV{AFPCMD_TEST_TIMEOUT} // 60;
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          pagination_setup_commands($dir, 257),
                          "ls $dir",
                          'q',
                          'quit'
    );

    like(
          $out,
          qr/256 entries shown; press Enter for next page, or q then Enter to quit/,
          'test_ls_pagination_quit: prompt advertises q'
    );
    like( $out, qr/^-.* page255$/m,
          'test_ls_pagination_quit: first page reaches page255');
    unlike($out, qr/^-.* page256$/m,
           'test_ls_pagination_quit: q suppresses next page');

    afpcmd_pipe($AFP_URL, "rm -r $dir", 'quit');
}

# -----------------------------------------------------------------------
# test_local_navigation: lpwd shows cwd; lcd changes it; bad path errors
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lpwd',
                          'lcd /tmp',
                          'lpwd',
                          'lcd /nonexistent_dir_xyz',
                          'quit'
    );
    like($out, qr/Now in local directory/,      'test_local_navigation: lpwd');
    like($out, qr{Now in local directory /tmp}, 'test_local_navigation: lcd /tmp');
    like($out, qr/Changing directories:/,       'test_local_navigation: lcd bad path error');
}

# -----------------------------------------------------------------------
# test_file_lifecycle: touch, cat, chmod, mv, cp, rm
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'touch lctest.txt',
                          'ls',
                          'cat lctest.txt',
                          'chmod 600 lctest.txt',
                          'ls',
                          'chmod 640 lctest.txt',
                          'ls',
                          'mv lctest.txt lcrenamed.txt',
                          'ls',
                          'cp lcrenamed.txt lccopy.txt',
                          'ls',
                          'rm lcrenamed.txt',
                          'rm lccopy.txt',
                          'quit'
    );
    like($out, qr/lctest\.txt/,                  'test_file_lifecycle: touch creates file');
    like($out, qr/^-rw------- .* lctest\.txt$/m, 'test_file_lifecycle: chmod 600');
    like($out, qr/^-rw-r----- .* lctest\.txt$/m, 'test_file_lifecycle: chmod 640');
    like($out, qr/lcrenamed\.txt/,               'test_file_lifecycle: mv rename');
    like($out, qr/Copied \d+ bytes/,             'test_file_lifecycle: cp reports bytes');
    like($out, qr/lccopy\.txt/,                  'test_file_lifecycle: cp creates copy');
}

# -----------------------------------------------------------------------
# test_copy_onto_itself: cp refuses a target that is the source, named
# directly or through its directory, and the file keeps its contents
# -----------------------------------------------------------------------
{
    my $tmpfile = "/tmp/afpcmd_cpself_$$.txt";
    open(my $fh, '>', $tmpfile) or BAIL_OUT("Cannot create temp file: $!");
    print $fh "copy onto itself\n";
    close $fh;
    (my $tmpname = $tmpfile) =~ s{.*/}{};

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          'mkdir cpself',
                          'cd cpself',
                          "put $tmpname",
                          "cp $tmpname $tmpname",
                          'cd ..',
                          "cp cpself/$tmpname cpself",
                          "cat cpself/$tmpname",
                          "rm cpself/$tmpname",
                          'rmdir cpself',
                          'quit'
    );
    unlink $tmpfile;
    my @refused = $out =~ /are the same file/g;
    is(scalar @refused, 2, 'test_copy_onto_itself: both copies are refused');
    like($out, qr/copy onto itself/, 'test_copy_onto_itself: the file keeps its contents');
}

# -----------------------------------------------------------------------
# test_put_over_readonly: put and cp over a file the session may not
# write fail and leave that file
# -----------------------------------------------------------------------
{
    my $tmpfile = "/tmp/afpcmd_putro_$$.txt";
    open(my $fh, '>', $tmpfile) or BAIL_OUT("Cannot create temp file: $!");
    print $fh "kept contents\n";
    close $fh;
    (my $tmpname = $tmpfile) =~ s{.*/}{};

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          "put $tmpname",
                          "cp $tmpname $tmpname.src",
                          "chmod 444 $tmpname",
                          "put $tmpname",
                          "cp $tmpname.src $tmpname",
                          'ls',
                          "chmod 644 $tmpname",
                          "rm $tmpname",
                          "rm $tmpname.src",
                          'quit'
    );
    unlink $tmpfile;
    like($out, qr/Permission denied opening file/,         'test_put_over_readonly: the second put fails');
    like($out, qr/Could not open target file for writing/, 'test_put_over_readonly: the cp fails');
    like($out, qr/^-r--r--r-- .* \Q$tmpname\E$/m,          'test_put_over_readonly: the file stays');
}

# -----------------------------------------------------------------------
# test_copy_onto_a_longer_file: cp over a longer file leaves no part of it
# -----------------------------------------------------------------------
{
    my $long  = "/tmp/afpcmd_cplong_$$.txt";
    my $short = "/tmp/afpcmd_cpshort_$$.txt";
    open(my $lfh, '>', $long) or BAIL_OUT("Cannot create $long: $!");
    print $lfh "the head, then the LONGER-TAIL\n";
    close $lfh;
    open(my $sfh, '>', $short) or BAIL_OUT("Cannot create $short: $!");
    print $sfh "short\n";
    close $sfh;
    (my $lname = $long)  =~ s{.*/}{};
    (my $sname = $short) =~ s{.*/}{};

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          "put $lname",
                          "put $sname",
                          "cp $sname $lname",
                          "cat $lname",
                          "rm $lname",
                          "rm $sname",
                          'quit'
    );
    unlink $long, $short;
    like($out, qr/Copied 6 bytes/, 'test_copy_onto_a_longer_file: cp copies the file');
    unlike($out, qr/LONGER-TAIL/, 'test_copy_onto_a_longer_file: and leaves no tail of the target');
}

# -----------------------------------------------------------------------
# test_touch_directory: touch of a directory's name finds the directory
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'mkdir tdtest.dir',
                          'touch tdtest.dir',
                          'ls',
                          'rmdir tdtest.dir',
                          'quit'
    );
    like($out, qr/^d.* tdtest\.dir$/m, 'test_touch_directory: the directory is listed');
    unlike($out, qr/Could not (?:create file|update timestamp)/, 'test_touch_directory: touch of its name succeeds');
}

# -----------------------------------------------------------------------
# test_touch_existing_file: touch of an existing file gives it the time of
# the touch
# -----------------------------------------------------------------------
{
    my $tmpfile = "/tmp/afpcmd_touch_$$.txt";
    open(my $fh, '>', $tmpfile) or BAIL_OUT("Cannot create temp file: $!");
    close $fh;
    # Mid-2001, the same year in every time zone
    utime 994000000, 994000000, $tmpfile or BAIL_OUT("Cannot set the time of $tmpfile: $!");
    (my $tmpname = $tmpfile) =~ s{.*/}{};

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          "put $tmpname",
                          'ls',
                          "touch $tmpname",
                          'ls',
                          "rm $tmpname",
                          'quit'
    );
    unlink $tmpfile;
    my @years = $out =~ /^-.* (\d{4})-\d\d-\d\d \d\d:\d\d \Q$tmpname\E$/mg;
    is_deeply(\@years, ['2001', (localtime)[5] + 1900], 'test_touch_existing_file: touch sets the time');
}

# -----------------------------------------------------------------------
# test_copy_unreadable_target: cp to a target in a directory the session
# may not search goes past the target's stat and fails at the create
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'touch cuttest.txt',
                          'mkdir cuttest.dir',
                          'chmod 600 cuttest.dir',
                          'cp cuttest.txt cuttest.dir/copy.txt',
                          'chmod 755 cuttest.dir',
                          'rmdir cuttest.dir',
                          'rm cuttest.txt',
                          'quit'
    );
    like($out, qr/Permission denied creating target file/, 'test_copy_unreadable_target: cp fails at the create');
}

# -----------------------------------------------------------------------
# test_delete_through_a_file: rm of a path through a file finds nothing,
# and the file the path names last at the root stays
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'touch dtx.txt',
                          'touch dty.txt',
                          'rm dtx.txt/dty.txt',
                          'rm dtx.txt',
                          'rm dty.txt',
                          'quit'
    );
    like( $out, qr{File not found: dtx\.txt/dty\.txt},
          'test_delete_through_a_file: rm of a path through a file finds nothing');
    unlike($out, qr/File not found: dty\.txt$/m,
           'test_delete_through_a_file: the file at the root stays for its own rm');
}

# -----------------------------------------------------------------------
# test_created_modes: touch makes a file of mode 0644 and mkdir a directory
# of mode 0755, not the server's own modes
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'touch cmtest.txt',
                          'mkdir cmtest.dir',
                          'ls',
                          'rm cmtest.txt',
                          'rmdir cmtest.dir',
                          'quit'
    );
    like($out, qr/^-rw-r--r-- .* cmtest\.txt$/m, 'test_created_modes: touch gives mode 0644');
    like($out, qr/^drwxr-xr-x .* cmtest\.dir$/m, 'test_created_modes: mkdir gives mode 0755');
}

# -----------------------------------------------------------------------
# test_put_readonly_tree: put -r of a directory and a file without the
# owner's write permission uploads the file, and both end with their modes
# -----------------------------------------------------------------------
{
    my $tree = "/tmp/afpcmd_rotree_$$";
    mkdir $tree                       or BAIL_OUT("Cannot create $tree: $!");
    open(my $fh, '>', "$tree/ro.txt") or BAIL_OUT("Cannot create $tree/ro.txt: $!");
    print $fh "read-only contents\n";
    close $fh;
    chmod 0444, "$tree/ro.txt";
    chmod 0555, $tree;
    (my $name = $tree) =~ s{.*/}{};

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          "put -r $name",
                          "cat $name/ro.txt",
                          "ls $name",
                          'ls',
                          "chmod 755 $name",
                          "chmod 644 $name/ro.txt",
                          "rm $name/ro.txt",
                          "rmdir $name",
                          'quit'
    );
    chmod 0755, $tree;
    unlink "$tree/ro.txt";
    rmdir $tree;
    like($out, qr/read-only contents/,         'test_put_readonly_tree: the file uploads into the directory');
    like($out, qr/^-r--r--r-- .* ro\.txt$/m,   'test_put_readonly_tree: the file has mode 0444');
    like($out, qr/^dr-xr-xr-x .* \Q$name\E$/m, 'test_put_readonly_tree: the directory has mode 0555');
}

# -----------------------------------------------------------------------
# test_put_fifo: put of a FIFO is refused without waiting for a writer
# -----------------------------------------------------------------------
{
    my $fifo = "/tmp/afpcmd_fifo_$$";
    mkfifo($fifo, 0600) or BAIL_OUT("Cannot create $fifo: $!");
    (my $name = $fifo) =~ s{.*/}{};

    my $out = afpcmd_pipe($AFP_URL, 'lcd /tmp', "put $name", 'quit');
    unlink $fifo;
    like($out, qr/Not a regular file/, 'test_put_fifo: put of a FIFO is refused');
}

# -----------------------------------------------------------------------
# test_put_tree_with_a_fifo: put -r of a tree that holds a FIFO names it
# -----------------------------------------------------------------------
{
    my $tree = "/tmp/afpcmd_fifotree_$$";
    mkdir $tree                or BAIL_OUT("Cannot create $tree: $!");
    mkfifo("$tree/pipe", 0600) or BAIL_OUT("Cannot create $tree/pipe: $!");
    (my $name = $tree) =~ s{.*/}{};

    my $out = afpcmd_pipe($AFP_URL, 'lcd /tmp', "put -r $name", "rm -r $name", 'quit');
    unlink "$tree/pipe";
    rmdir $tree;
    like($out, qr{Not a regular file: .*/pipe}, 'test_put_tree_with_a_fifo: the FIFO is refused by name');
}

# -----------------------------------------------------------------------
# test_put_dir_onto_file: put -r of a directory onto a file of its name
# fails and leaves the file's mode
# -----------------------------------------------------------------------
{
    my $tree = "/tmp/afpcmd_ontofile_$$";
    mkdir $tree or BAIL_OUT("Cannot create $tree: $!");
    (my $name = $tree) =~ s{.*/}{};

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          "touch $name",
                          "chmod 600 $name",
                          "put -r $name",
                          'ls',
                          "rm $name",
                          'quit'
    );
    rmdir $tree;
    like($out, qr/exists and is not a directory/, 'test_put_dir_onto_file: put -r fails');
    like($out, qr/^-rw------- .* \Q$name\E$/m,    'test_put_dir_onto_file: the file keeps its mode');
}

# -----------------------------------------------------------------------
# test_put_deep_tree: put -r of a tree 40 directories deep uploads the
# file at its bottom
# -----------------------------------------------------------------------
{
    my $tree = "/tmp/afpcmd_deep_$$";
    my $deep = join('/', $tree, ('d') x 40);
    make_path($deep);
    open(my $fh, '>', "$deep/deep.txt") or BAIL_OUT("Cannot create $deep/deep.txt: $!");
    print $fh "deep contents\n";
    close $fh;
    (my $name   = $tree) =~ s{.*/}{};
    (my $remote = $deep) =~ s{^/tmp/}{};

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          "put -r $name",
                          "cat $remote/deep.txt",
                          "rm -r $name",
                          'quit'
    );
    remove_tree($tree);
    like($out, qr/deep contents/, 'test_put_deep_tree: the deepest file uploads');
}

# -----------------------------------------------------------------------
# test_copy_into_itself_by_another_name: cp -r into the source, or into its
# parent so that the copy is the source, named in another Unicode
# normalization, is refused
# -----------------------------------------------------------------------
{
    my $nfc = "caf\xc3\xa9.dir";
    my $nfd = "cafe\xcc\x81.dir";
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          "mkdir $nfc",
                          "mkdir $nfc/sub",
                          "cp -r $nfc $nfd/copy",
                          "cp -r $nfc/sub $nfd",
                          "ls $nfc",
                          "rm -r $nfc",
                          'quit'
    );
    my @refused = $out =~ /Cannot copy a directory into itself/g;
    is(scalar @refused, 2, 'test_copy_into_itself_by_another_name: both copies are refused');
    unlike($out, qr/^d.* copy$/m, 'test_copy_into_itself_by_another_name: and make no copy');
}

# -----------------------------------------------------------------------
# test_copy_tree_onto_a_directory_of_a_file_name: cp -r into a copy that
# holds a directory at a file's name fails for that file, and the file
# does not land inside the directory
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'mkdir cptree.src',
                          'touch cptree.src/x',
                          'mkdir cptree.dst',
                          'mkdir cptree.dst/cptree.src',
                          'mkdir cptree.dst/cptree.src/x',
                          'cp -r cptree.src cptree.dst',
                          'ls cptree.dst/cptree.src/x',
                          'rm -r cptree.src',
                          'rm -r cptree.dst',
                          'quit'
    );
    like( $out, qr/Could not open target file for writing/,
          'test_copy_tree_onto_a_directory_of_a_file_name: the file\'s copy fails');
    unlike($out, qr/^-.* x$/m, 'test_copy_tree_onto_a_directory_of_a_file_name: and lands nowhere');
}

# -----------------------------------------------------------------------
# test_copy_tree_without_unix_privileges: cp -r on a volume without UNIX
# privileges copies a directory of the tree as a directory
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}/${AFP_VOL_NOUNIX}",
                          'mkdir nucopy.src',
                          'mkdir nucopy.src/sub',
                          'touch nucopy.src/sub/f',
                          'cp -r nucopy.src nucopy.dst',
                          'ls nucopy.dst/sub',
                          'rm -r nucopy.src',
                          'rm -r nucopy.dst',
                          'quit'
    );
    like($out, qr/^-.* f$/m, 'test_copy_tree_without_unix_privileges: the directory\'s file is copied');
}

# -----------------------------------------------------------------------
# test_remove_tree_without_unix_privileges: without UNIX privileges a listing
# shows a directory as one, at the mount's size, and rm -r removes the tree
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}/${AFP_VOL_NOUNIX}",
                          'mkdir nurm',
                          'mkdir nurm/sub',
                          'touch nurm/sub/f',
                          'ls nurm',
                          'rm -r nurm',
                          'quit'
    );
    like($out, qr/^d\S* +58 .* sub$/m, 'test_remove_tree_without_unix_privileges: a folder lists as one at its size');
    like($out, qr/^Deleted directory: nurm$/m, 'test_remove_tree_without_unix_privileges: rm -r removes the tree');
}

# -----------------------------------------------------------------------
# test_directory_lifecycle: mkdir, rmdir, rmdir error cases
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'mkdir dltest',
                          'ls',
                          'touch dltest/inside.txt',
                          'ls dltest',
                          'rmdir dltest',
                          'rm dltest/inside.txt',
                          'rmdir dltest',
                          'rmdir nonexistent_dir_xyz',
                          'quit'
    );
    like($out, qr/dltest/,              'test_directory_lifecycle: mkdir');
    like($out, qr/inside\.txt/,         'test_directory_lifecycle: ls inside dir');
    like($out, qr/Directory not empty/, 'test_directory_lifecycle: rmdir not empty error');
    like($out, qr/Directory not found/, 'test_directory_lifecycle: rmdir nonexistent error');
}

# -----------------------------------------------------------------------
# test_command_recursive_flag: -r is accepted after interactive commands
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
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
                          'quit'
    );
    unlike($out, qr/expecting format:/,
           'test_command_recursive_flag: command-local -r parses');
    like( $out, qr/Cannot copy a directory into itself/,
          'test_command_recursive_flag: trailing slash self-copy is rejected');
    like( $out, qr/exists and is not a directory/,
          'test_command_recursive_flag: directory-to-file collision is rejected');
    like( $out, qr/file\.txt/,
          'test_command_recursive_flag: cp recursively copied nested file');
    like( $out, qr/Deleted directory: rflag-src/,
          'test_command_recursive_flag: rm recursively removed source');
    like( $out, qr/Deleted directory: rflag-moved/,
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

    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'lcd /tmp',
                          "put $tmpname",
                          "cat $tmpname",
                          "get $tmpname",
                          "rm $tmpname",
                          'quit'
    );

    like($out, qr/Transfer complete\./,         'test_get_put: put transfer complete');
    like($out, qr/get_put test content line 1/, 'test_get_put: cat shows content');
    like($out, qr/Transfer complete\./,         'test_get_put: get transfer complete');

    ok(-f $tmpfile, 'test_get_put: downloaded file exists');
    if (-f $tmpfile) {
        open(my $rfh, '<', $tmpfile) or BAIL_OUT("Cannot read downloaded file: $!");
        my $content = do { local $/; <$rfh> };
        close $rfh;
        like( $content, qr/get_put test content line 1/,
              'test_get_put: downloaded file content matches');
        unlink $tmpfile;
    }
}

# -----------------------------------------------------------------------
# test_metadata_commands: round-trip FinderInfo, ResourceFork, and xattr
# -----------------------------------------------------------------------
{
    my $base         = "/tmp/afpcmd_metadata_$$";
    my $finder_in    = "$base.finder.in";
    my $finder_out   = "$base.finder.out";
    my $resource_in  = "$base.resource.in";
    my $resource_out = "$base.resource.out";
    my $xattr_in     = "$base.xattr.in";
    my $xattr_out    = "$base.xattr.out";

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
    my $out = afpcmd_pipe(
                          $AFP_URL,
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
                          'quit'
    );
    umask $old_umask;

    unlike($out, qr/failed:/, 'test_metadata_commands: commands succeed');
    like( $out, qr/org\.netatalk\.afpcmd-test/,
          'test_metadata_commands: xattr appears in list');

    for my $pair (
                  [$finder_in,   $finder_out,   'FinderInfo'],
                  [$resource_in, $resource_out, 'ResourceFork'],
                  [$xattr_in,    $xattr_out,    'xattr']
    ) {
        if (!-e $pair->[1]) {
            fail('test_metadata_commands: ' . $pair->[2] . ' output exists');
            next;
        }
        open(my $left,  '<:raw', $pair->[0]) or BAIL_OUT("read $pair->[0]: $!");
        open(my $right, '<:raw', $pair->[1]) or BAIL_OUT("read $pair->[1]: $!");
        my $left_data  = do { local $/; <$left> };
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
# test_exit_reconnect: exit detaches volume and opens the picker automatically
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'exit',
                          '1',
                          'quit'
    );
    like($out, qr/Detached from volume/,         'test_exit_reconnect: exit detaches');
    like($out, qr/Available volumes on/,         'test_exit_reconnect: picker opens');
    like($out, qr/^[ \t]+1[ \t]+\Q$AFP_VOL\E$/m, 'test_exit_reconnect: picker numbers volume');
    like($out, qr/Attached to volume/,           'test_exit_reconnect: number reattaches');
}

# -----------------------------------------------------------------------
# test_initial_volume_picker: a URL without a volume opens the picker
# -----------------------------------------------------------------------
{
    my $server_url = "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}";
    my $out        = afpcmd_pipe($server_url, '1', 'quit');
    like($out, qr/Available volumes on/,         'test_initial_volume_picker: picker opens');
    like($out, qr/^[ \t]+1[ \t]+\Q$AFP_VOL\E$/m, 'test_initial_volume_picker: volume numbered');
    like($out, qr/q\s+Quit/,                     'test_initial_volume_picker: quit action labeled');
    like($out, qr/Attached to volume/,           'test_initial_volume_picker: number attaches');
    unlike($out, qr/Use 'ls' to list available volumes/,
           'test_initial_volume_picker: no obsolete help text');
}

# -----------------------------------------------------------------------
# test_volume_picker_quit: only a selection number or exact q is accepted
# -----------------------------------------------------------------------
{
    my $server_url = "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}";
    my $out        = afpcmd_pipe($server_url, '+1', 'Q', 'not-a-number', 'q', 'pwd');
    my @errors     = ($out =~ /Choose a listed volume number, or q to quit\./g);
    like( $out, qr/Choose a listed volume number, or q to quit\./,
          'test_volume_picker_quit: other input rejected');
    is(scalar @errors, 3,
       'test_volume_picker_quit: signed, uppercase, and command input rejected');
    like($out, qr/q\s+Quit/, 'test_volume_picker_quit: quit action labeled');
    unlike($out, qr/You're not attached to a volume/,
           'test_volume_picker_quit: q exits before later commands');
}

# -----------------------------------------------------------------------
# test_passwd: change to temp password, verify, change back, verify
# -----------------------------------------------------------------------
{
    my $url_orig        = "afp://${AFP_USER}:${AFP_PASS}\@${AFP_HOST}/${AFP_VOL}";
    my $url_tmp         = "afp://${AFP_USER}:${AFP_PASS_TMP}\@${AFP_HOST}/${AFP_VOL}";
    my $restore_pending = 1;
    my $test_pid        = $$;

    END {
        local $?;    # Preserve the test exit status across cleanup.
        if ($restore_pending && $$ == $test_pid) {
            my $out = afpcmd_pipe(
                                  $url_tmp,
                                  'passwd',
                                  $AFP_PASS_TMP,
                                  $AFP_PASS,
                                  $AFP_PASS,
                                  'quit'
            );
            if ($out =~ /Password changed successfully\./) {
                $restore_pending = 0;
            } else {
                diag('test_passwd: END cleanup could not restore the original password');
            }
        }
    }

    my $out = afpcmd_pipe(
                          $url_orig,
                          'passwd',
                          $AFP_PASS,
                          $AFP_PASS_TMP,
                          $AFP_PASS_TMP,
                          'quit'
    );

    if ($out =~ /Password change failed: not supported by the current authentication method\./) {
        $restore_pending = 0;
        pass('test_passwd: skipped (UAM does not support password change)');
    } else {
        like($out, qr/Password changed successfully\./, 'test_passwd: change to temp password');

        afpcmd_pipe($url_tmp, 'quit');    # verify temp password works (no assertion needed)

        my $out2 = afpcmd_pipe(
                               $url_tmp,
                               'passwd',
                               $AFP_PASS_TMP,
                               $AFP_PASS,
                               $AFP_PASS,
                               'quit'
        );
        like($out2, qr/Password changed successfully\./, 'test_passwd: revert to original password');
        $restore_pending = 0 if $out2 =~ /Password changed successfully\./;

        afpcmd_pipe($url_orig, 'quit');    # verify original password restored
    }
}

# -----------------------------------------------------------------------
# test_disconnect: disconnect from server; afpsld, whose last client asked
# it to exit, has retired its socket by the time it answers
# -----------------------------------------------------------------------
{
    my $out = afpcmd_pipe(
                          $AFP_URL,
                          'disconnect'
    );
    like($out, qr/Disconnected from $AFP_SERVER/, 'test_disconnect: disconnect from server');
    ok(!-e "/tmp/afp_sl-$>", "test_disconnect: afpsld's socket is gone once it answers the exit");
}

# -----------------------------------------------------------------------
# afpsld's own protocol: a request is a command byte, three bytes of
# padding, the request's length and a close flag, then the command's fields
# as a C struct of a 64-bit build lays them out.
# -----------------------------------------------------------------------
my $ipc_hello = pack('C x3 L L L S S L', 50, 24, 0, 0x41465053, 3, 0, 0);

# A write request for a volume, with the size it names.
sub ipc_write_request {
    my ($volume, $size) = @_;
    return pack('C x3 L L x4 Q L x4 Q L L', 27, 48, 0, $volume, 0, 0, $size, 0);
}

# A read request for a volume, with the length it asks for.
sub ipc_read_request {
    my ($volume, $length) = @_;
    return pack('C x3 L L x4 Q L x4 Q L L', 23, 48, 0, $volume, 0, 0, $length, 0);
}

# Read count bytes of an answer: the bytes, '' at the end of the connection
# or undef when afpsld sends nothing within five seconds.
sub ipc_read {
    my ($socket, $count) = @_;
    my $got = '';

    while (length($got) < $count) {
        IO::Select->new($socket)->can_read(5) or return undef;
        my $read = sysread($socket, my $chunk, $count - length($got));
        return '' unless $read;
        $got .= $chunk;
    }

    return $got;
}

# The result of afpsld's next answer, -1 at the end of the connection, an
# answer it cuts short among them, or -2 when no answer comes.
sub ipc_answer {
    my ($socket) = @_;
    my $header = ipc_read($socket, 8);
    return -2 unless defined $header;
    return -1 unless length $header;
    my ($result, $length) = unpack('C x3 L', $header);
    my $size = $length > 8 ? $length - 8 : 0;
    my $rest = ipc_read($socket, $size);
    return -2 unless defined $rest;
    return length($rest) == $size ? $result : -1;
}

# Connect to afpsld, which afpc starts, and agree the protocol.
sub ipc_connect {
    my ($label) = @_;
    my $started = `afpc sl status 2>&1`;
    $? == 0 or BAIL_OUT("afpsld does not start: $started");
    my $path   = "/tmp/afp_sl-$>";
    my $socket = IO::Socket::UNIX->new(Type => SOCK_STREAM, Peer => $path)
      or BAIL_OUT("Cannot connect to $path: $!");
    syswrite($socket, $ipc_hello);
    is(ipc_answer($socket), 0, "$label: afpsld accepts the protocol");
    return $socket;
}

# -----------------------------------------------------------------------
# afpsld_write_size: a write request larger than the limit is refused, and
# the connection ends with the answer, as the data it names cannot be
# skipped.
# -----------------------------------------------------------------------
{
    local $SIG{PIPE} = 'IGNORE';
    my $socket = ipc_connect('afpsld_write_size');
    syswrite($socket, ipc_write_request(0xdeadbeef, 0xffffffff));
    is(ipc_answer($socket),  1, 'afpsld_write_size: a write of 4 GiB less one byte is refused');
    is(ipc_answer($socket), -1, 'afpsld_write_size: the connection ends with the answer');
}

# -----------------------------------------------------------------------
# afpsld_write_data: the data of a write is read before the volume is
# looked up, so the answer for a volume that is gone leaves the connection
# at the next request.
# -----------------------------------------------------------------------
{
    local $SIG{PIPE} = 'IGNORE';
    my $socket = ipc_connect('afpsld_write_data');
    syswrite($socket, ipc_write_request(0xdeadbeef, 64) . ("\0" x 64));
    is(ipc_answer($socket), 6, 'afpsld_write_data: a write to a volume that is gone is answered not attached');
    syswrite($socket, $ipc_hello);
    is(ipc_answer($socket), 0, 'afpsld_write_data: the next request is read after the data');
}

# -----------------------------------------------------------------------
# afpsld_write_short: a write request too short to name its data is
# refused, and the connection ends with the answer, as the data it may
# carry cannot be found.
# -----------------------------------------------------------------------
{
    local $SIG{PIPE} = 'IGNORE';
    my $socket = ipc_connect('afpsld_write_short');
    syswrite($socket, pack('C x3 L L', 27, 20, 0) . ("\0" x 8) . $ipc_hello);
    is(ipc_answer($socket),  1, 'afpsld_write_short: a write request of 20 bytes is refused');
    is(ipc_answer($socket), -1, 'afpsld_write_short: the connection ends with the answer');
}

# -----------------------------------------------------------------------
# afpsld_read_size: a read request for more than the limit is refused
# before the volume is looked up.
# -----------------------------------------------------------------------
{
    local $SIG{PIPE} = 'IGNORE';
    my $socket = ipc_connect('afpsld_read_size');
    syswrite($socket, ipc_read_request(0xdeadbeef, 0xffffffff));
    is(ipc_answer($socket), 1, 'afpsld_read_size: a read of 4 GiB less one byte is refused');
    syswrite($socket, $ipc_hello);
    is(ipc_answer($socket), 0, 'afpsld_read_size: the next request is read');
}

# The process ids of the AFP server's processes, one for each session.
sub afpd_pids {
    my @pids = `pgrep -x afpd`;
    chomp @pids;
    return @pids;
}

# Start an afpcmd session, whose input and output the test holds.
sub session_start {
    pipe(my $out_r, my $out_w) or BAIL_OUT("pipe: $!");
    pipe(my $in_r,  my $in_w)  or BAIL_OUT("pipe: $!");
    my $pid = fork() // BAIL_OUT("fork: $!");

    if ($pid == 0) {
        close $out_r;
        close $in_w;
        open(STDIN,  '<&', $in_r)  or die "dup stdin: $!";
        open(STDOUT, '>&', $out_w) or die "dup stdout: $!";
        open(STDERR, '>&', $out_w) or die "dup stderr: $!";
        close $in_r;
        close $out_w;
        exec('afpcmd', $AFP_URL) or die "exec: $!";
    }

    close $out_w;
    close $in_r;
    $in_w->autoflush(1);
    return {pid => $pid, in => $in_w, out => $out_r, seen => ''};
}

# Read the session's output until it matches the pattern, for up to the
# seconds given; whether it did.
sub session_wait {
    my ($session, $pattern, $seconds) = @_;
    my $end = Time::HiRes::time() + $seconds;

    while ($session->{seen} !~ $pattern && Time::HiRes::time() < $end) {
        IO::Select->new($session->{out})->can_read(0.2) or next;
        sysread($session->{out}, my $chunk, 4096)       or last;
        $session->{seen} .= $chunk;
    }

    return $session->{seen} =~ $pattern ? 1 : 0;
}

# End the session with its quit command, or kill it after the seconds given.
sub session_end {
    my ($session, $seconds) = @_;
    local $SIG{PIPE} = 'IGNORE';
    syswrite($session->{in}, "quit\n");
    close $session->{in};
    my $end = Time::HiRes::time() + $seconds;

    while (Time::HiRes::time() < $end) {
        waitpid($session->{pid}, POSIX::WNOHANG()) == $session->{pid} and return;
        Time::HiRes::sleep(0.1);
    }

    kill 'KILL', $session->{pid};
    waitpid($session->{pid}, 0);
}

# -----------------------------------------------------------------------
# afpsld_servers: a command held up on one server does not hold up another
# session. The first session's afpd stops, its listing waits for it, and a
# second session, which is a second server to the daemon, runs meanwhile.
# -----------------------------------------------------------------------

# The afpd processes the test has stopped, which run again however it ends.
my @stopped_afpd;
END { kill 'CONT', @stopped_afpd if @stopped_afpd; }

SKIP: {
    my %before = map { $_ => 1 } afpd_pids();
    my $first  = session_start();
    session_wait($first, qr/Connected to server/, 20)
      or BAIL_OUT('afpsld_servers: the first session does not connect');
    my @mine = grep { !$before{$_} } afpd_pids();

    if (@mine != 1) {
        session_end($first, 20);
        skip('afpsld_servers: the first session\'s afpd is not the only new one', 4);
    }

    # afpd's session runs with root's real ID, which only root may signal.
    if (kill('STOP', $mine[0]) != 1) {
        my $error = $!;
        session_end($first, 20);
        skip("afpsld_servers: cannot stop afpd $mine[0]: $error", 4);
    }

    push @stopped_afpd, $mine[0];
    syswrite($first->{in}, "ls\npwd\n");
    Time::HiRes::sleep(0.5);
    my $name   = "afpsld_servers_$$";
    my $start  = Time::HiRes::time();
    my $second = session_start();
    syswrite($second->{in}, "touch $name\nls\npwd\n");
    my $served = session_wait($second, qr{^-[^\n]* \Q$name\E\n.*?Now in directory /}ms, 12)
      && $second->{seen} !~ /Could not/;
    my $took = Time::HiRes::time() - $start;
    my $held = !session_wait($first, qr{Now in directory /}, 0.3);
    kill 'CONT', $mine[0];
    @stopped_afpd = ();
    ok($served, 'afpsld_servers: the second session lists its directory');
    cmp_ok($took, '<', 3, sprintf('afpsld_servers: it did so in %.1f s, while the first server waited', $took));
    ok($held, 'afpsld_servers: the first session\'s listing was still waiting for its server');
    ok(session_wait($first, qr{Now in directory /}, 20),
       'afpsld_servers: the first session finishes once its server runs');
    session_end($second, 10);
    session_end($first,  20);
    afpcmd_pipe($AFP_URL, "rm $name", 'quit');
}

# The request counts of a status text, by command name.
sub request_counts {
    my ($text) = @_;
    my %counts = $text =~ /^\s+(afp\w+): (\d+)$/mg;
    BAIL_OUT('afpcmd status listed no request counts') unless %counts;
    return \%counts;
}

# Stop the daemon, which keeps every session it served, so that the next
# command starts a daemon whose status counts that command alone.
sub fresh_daemon {
    my $stopped = `afpc sl exit 2>&1`;
    $? == 0 or BAIL_OUT("afpsld does not stop: $stopped");

    for (1 .. 50) {
        my @running = grep {
            my (undef, $state, $uid) = split;
            $state !~ /^Z/ && $uid == $>
        } `ps -C afpsld -o pid=,stat=,euid= 2>/dev/null`;
        @running or return;
        Time::HiRes::sleep(0.1);
    }

    BAIL_OUT('afpsld stays up: a client is still registered');
}

# Write a file of the given size.
sub write_file {
    my ($path, $bytes) = @_;
    open(my $fh, '>:raw', $path) or BAIL_OUT("Cannot create $path: $!");
    print {$fh} substr(pack('N*', 0 .. ($bytes >> 2)), 0, $bytes);
    close $fh;
}

# -----------------------------------------------------------------------
# afpsld_copy: a new copy of a file the server can copy is one FPCopyFile
# and moves no data through the client; a copy over an existing file goes
# through the daemon a quantum at a time.
# -----------------------------------------------------------------------
{
    require File::Compare;
    my $dir    = File::Temp::tempdir(CLEANUP => 1);
    my $name   = "afpsld_copy_$$.bin";
    my $size   = 4 << 20;
    my $source = "$dir/$name";
    write_file($source, $size);
    my $log = `afpcmd '$source' '$AFP_URL' 2>&1`;
    is($?, 0, 'afpsld_copy: the file uploads') or diag($log);

    fresh_daemon();
    my $out    = afpcmd_pipe($AFP_URL, "cp $name $name.new", 'status', 'quit');
    my $counts = request_counts($out);
    like($out, qr/Copied $size bytes/, 'afpsld_copy: cp reports the copy');
    is($counts->{afpCopyFile}, 1, 'afpsld_copy: a new copy is one FPCopyFile');
    is($counts->{afpReadExt}  // 0, 0, 'afpsld_copy: the copy reads nothing through the client');
    is($counts->{afpWriteExt} // 0, 0, 'afpsld_copy: the copy writes nothing through the client');
    my $copy = File::Temp::tempdir(CLEANUP => 1);
    $log = `afpcmd '$AFP_URL/$name.new' '$copy' 2>&1`;
    is($?,                                                 0, "afpsld_copy: the server's copy downloads") or diag($log);
    is(File::Compare::compare($source, "$copy/$name.new"), 0, "afpsld_copy: the server's copy matches its source");

    fresh_daemon();
    $out    = afpcmd_pipe($AFP_URL, "cp $name $name.new", 'status', 'quit');
    $counts = request_counts($out);
    my ($quantum) = $out =~ /^\s+quantum: (\d+)$/m;
    my $quanta = int(($size + $quantum - 1) / $quantum);
    is($counts->{afpCopyFile} // 0, 0,       'afpsld_copy: a copy over a file sends no FPCopyFile');
    is($counts->{afpReadExt},       $quanta, 'afpsld_copy: the copy over a file reads a quantum at a time');
    is($counts->{afpWriteExt},      $quanta, 'afpsld_copy: the copy over a file writes a quantum at a time');

    $log = `afpcmd '$AFP_URL/$name.new' '$dir' 2>&1`;
    is($?,                                                0, 'afpsld_copy: the copy downloads') or diag($log);
    is(File::Compare::compare($source, "$dir/$name.new"), 0, 'afpsld_copy: the copy matches its source');
    afpcmd_pipe($AFP_URL, "rm $name", "rm $name.new", 'quit');
}

# -----------------------------------------------------------------------
# afpsld_session_lock: a command on a session waits for the one the
# session runs, whichever client sends it. The session's afpd stops with
# its listing waiting, and a probe resumes the session, alone on a fresh
# daemon, to list its volumes, which afpsld answers from memory under the
# session's lock.
# -----------------------------------------------------------------------
SKIP: {
    my $probe = '/test/afpsl_probe';
    -x $probe or skip('afpsld_session_lock: no afpsl_probe in the image', 3);
    fresh_daemon();
    my %before  = map { $_ => 1 } afpd_pids();
    my $session = session_start();
    session_wait($session, qr/Connected to server/, 20)
      or BAIL_OUT('afpsld_session_lock: the session does not connect');
    my @mine = grep { !$before{$_} } afpd_pids();

    if (@mine != 1 || kill('STOP', $mine[0]) != 1) {
        session_end($session, 20);
        skip('afpsld_session_lock: the session\'s afpd cannot be stopped alone', 3);
    }

    push @stopped_afpd, $mine[0];
    syswrite($session->{in}, "ls\npwd\n");
    Time::HiRes::sleep(0.5);
    open(my $probe_out, '-|', $probe, $AFP_URL) or BAIL_OUT("Cannot run $probe: $!");
    my $early = IO::Select->new($probe_out)->can_read(3);
    kill 'CONT', $mine[0];
    @stopped_afpd = ();
    my $answer = join('', <$probe_out>);
    close $probe_out;
    ok(!$early, 'afpsld_session_lock: the probe\'s volume list waits while the session\'s listing does');
    like($answer, qr/^getvols 0 in /m, 'afpsld_session_lock: the volume list answers once the listing has')
      or diag($answer);
    ok(session_wait($session, qr{Now in directory /}, 20),
       'afpsld_session_lock: the listing finishes once its server runs');
    session_end($session, 20);
}

# -----------------------------------------------------------------------
# afpcmd_remove_tree: rm -r removes a directory of more than 1000 entries.
# -----------------------------------------------------------------------
{
    local $ENV{AFPCMD_TEST_TIMEOUT} = $ENV{AFPCMD_TEST_TIMEOUT} // 60;
    my $dir  = "afpsld_tree_$$";
    my $made = afpcmd_pipe($AFP_URL, "mkdir $dir", (map { sprintf('touch %s/entry%04d', $dir, $_) } 1 .. 1001), 'quit');
    unlike($made, qr/Could not/, 'afpcmd_remove_tree: the directory and its 1001 entries are made');
    my $out = afpcmd_pipe($AFP_URL, "rm -r $dir", 'quit');
    like($out, qr/Deleted directory: \Q$dir\E/, 'afpcmd_remove_tree: rm -r of a directory of 1001 entries succeeds');
    unlike(afpcmd_pipe($AFP_URL, 'ls', 'quit'), qr/\Q$dir\E/, 'afpcmd_remove_tree: the directory is gone');
    afpcmd_pipe($AFP_URL, "rm -r $dir", 'quit');
}

done_testing;
