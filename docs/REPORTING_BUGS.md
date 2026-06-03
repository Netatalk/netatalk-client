# Reporting Bugs

This quick document describes how to gather debugging information for Netatalk Client.

Thank you for contributing to the project by submitting a bug report!  This
software's quality is heavily influenced by reported bugs.  We will help
you as best we can.

Before we start:

- be aware of sending confidential information over the wire and saving it in your logs
- more information may be required, but this is a good start

When ready, file an issue ticket in this project with a description
of the problem.

## A. Problems with afpcmd

This should be the first thing to try.

### Start a network dump for afpcmd

You can run the following on Mac OS X, Linux or other systems.  You can do this
on either the client, server, or a system that sits in between the client and
server.  You'll need root access.

Run:

    tcpdump -s0 -w my_network_capture port 548

There are other tools to do this, like wireshare/ethereal.  Use those if it is
easier, but save it in a tcpdump format.

Keep this running in a window until after you've shown the bug.

### Perform your operation

Use afpcmd to connect, authenticate, transfer, etc.

Save the console contents to a file called my_command.txt.

### Mail a copy of the console and network capture

Tar up all the relevant files with something like:

	tar -czf my_afpfsd_bug.tar.gz \
		my_network_capture my_debug_log my_status my_command.txt

## B. For problems with the FUSE client

### Kill off any lingering versions of afpfsd

By default, a process called afpfsd runs in the background, you may have one
lingering.

The most graceful way to stop it is to run:

    fusermount -u /path/to/mountpoint
    afp_client exit

If this doesn't work, try:

    killall afpfsd

If the process still exists (use 'ps aux |grep afpfsd' to check), be
rutheless:

    killall -9 afpfsd

### Rerun afpfsd in debug mode

Run:

    afpfsd -d > my_debug_log

This will record lots of logging output to a file.  Keep this running in a
window.

### Start a network dump for afpfsd

(as above)

### Setup the mount

How you setup the mount is dependent on your environment.  Run your
'afp_client mount ...' command and copy it into a file called my_command.txt

### Grab the status output

Get the status with:

    afp_client status > my_status

This will exit quickly.

### Rerun whatever causes the problem

### Send off a bug report

Kill the tcpdump and afpfsd processes.

Tar up all the relevant files with something like:

	tar -czf my_afpfsd_bug.tar.gz \
		my_network_capture my_debug_log my_status my_command.txt
