# AFP File Sharing Client for Netatalk and Macs

## Description

**Netatalk Client** is a file sharing client written in C
which can be used to access AFP shares exposed by multiple devices,
notably personal file sharing on older Mac OS X and Classic Mac OS computers,
[Netatalk](https://netatalk.io/) servers hosted on Linux/*BSD/Solaris/macOS,
Apple AirPort and Time Capsule products as well as other AFP enabled NAS devices from various vendors.

Netatalk Client is an improved fork of [afpfs-ng](https://sourceforge.net/projects/afpfs-ng/).
It supports most major features of the AFP protocol v2.1 through v3.4, secure authentication,
file and directory operations, filesystem extended attributes, and FUSE-based mounting of AFP shares.

## Usage

You can use Netatalk Client either to mount an AFP share with FUSE,
or interactively with the command-line client.

The shared library *libafpclient* can also be used to add AFP support to other applications.
For instance, [kio-afp](https://invent.kde.org/dmark/kio-afp) which provides a KDE KIO worker for browsing
AFP shares in Dolphin and other KDE applications.

### FUSE

Mount the *File Sharing* volume from afpserver.local on /home/myuser/fusemount
authenticated as user *myuser* (you will be prompted for the password):

    % afp_client mount --user myuser "afpserver.local:File Sharing" /home/myuser/fusemount

Get status information about all AFP volumes mounted by the current user:

    % afp_client status

Unmount the volume when you are done:

    % afp_client unmount /home/myuser/fusemount

Shut down the FUSE management daemon (*afpfsd*) when no FUSE mounts are active:

    % afp_client exit

There is also an alternative command *mount_afpfs* included for mounting by AFP URL:

    % mount_afpfs "afp://myuser@afpserver.local/File Sharing" /home/myuser/fusemount

**Note:** Quotation marks around the AFP URL are required when spaces,
colons, or other special characters are present.

### Command line client

The *afpcmd* command line client allows you to interactively access AFP shares.
In the most basic use case, it takes an AFP URL as argument.

Open volume File Sharing on afpserver.local:

    $ afpcmd "afp://myuser@afpserver.local/File Sharing"
    Password: [input hidden]
    Connected to server afpserver
    afpcmd:

Connect anonymously to afpserver.local, list all volumes available to guest users:

    $ afpcmd "afp://afpserver.local"
    Connected to server afpserver
    Not attached to a volume. Run the 'ls' command to list available volumes
    afpcmd: cd Dropbox
    Attached to volume Dropbox
    afpcmd: ls
    -rw-r--r--   6148 2025-07-11 14:09 .DS_Store
    -rw-r--r-- 108320 2025-10-12 13:59 mytarball.tar.xz
    -rw-------      0 2025-10-12 00:39 bork.txt
    -rw-r--r-- 525362 2024-10-09 13:02 group_photo.jpg
    -rw-r--r--  46954 2023-08-03 02:03 Information Sheet.xlsx
    drwxrwxrwx      0 2025-10-12 00:22 Scanned Documents
    afpcmd:

cd to change directories, *ls* to list, *get* file to retrieve file, *put* file to download file,
and *help* for a list of all supported commands.

Use *afpcmd* in batch mode to download files from the AFP share to the local machine:

    $ afpcmd "afp://myuser@afpserver.local/File Sharing/mytarball.tar.xz" .
    Password: [input hidden]
    Connected to server afpserver
        Downloading file mytarball.tar.xz
        Transferred 108320 bytes in 0.015 seconds. (7200 kB/s)
    Transfer complete. 108320 bytes received.

## Differences with afpfs-ng

Netatalk Client has the goal of maintaining continuity with afpfs-ng commands and APIs,
so that moving from one to the other is as seamless as possible. However we do not guarantee
libafpclient ABI compatibility with afpfs-ng, and differences in behavior and supported features exist.

- afpfsd.h is now a shim header which includes afp_server.h
- afpfs-ng had to be built either with FUSE or Stateless API support, but Netatalk Client supports both simultaneously
- As a result of the above, the Stateless client controller daemon is now called *afpsld* instead of *afpfsd*
  to avoid namespace conflicts with the FUSE controller daemon
- *mount_afp* has been renamed to *mount_afpfs* to avoid namespace conflicts with the macOS *mount_afp* command
- The *afpcmd* command line client has been rewritten to use the Stateless API instead of implementing
  libafpclient calls directly, and now relies on the *afpsld* daemon to manage AFP sessions and connections
  instead of implementing its own session management
- The *afpfsd* FUSE controller daemon has been rewritten to have one process per FUSE mount,
  with a manager process to handle mount requests and manage the afpfsd processes.
  This allows for better isolation and stability of FUSE mounts compared to a single process handling all mounts.

We have also introduced numerous new options and features, which will not be listed in full here.
Refer to the documentation and command line help text.

## How to Contribute

This project follows the same [Contributing guidelines](https://github.com/Netatalk/netatalk/blob/main/CONTRIBUTING.md)
and coding standards as the Netatalk project.

The same [Code of Conduct policy](https://github.com/Netatalk/netatalk/blob/main/CODE_OF_CONDUCT.md) is effective as well.

We are looking forward to your contributions!

## License

Netatalk Client is distributed under the terms of the GNU General Public License v2.
See COPYING in this repository for the full text of the license

## Credits

This project was forked from [afpfs-ng](https://sourceforge.net/projects/afpfs-ng/) by Daniel Markstedt in 2024
and renamed to *Netatalk Client* to serve as a companion to the [Netatalk](https://netatalk.io/) AFP file server project.
*afpfs-ng* was created by Alex deVries in 2006 and maintained until 2009.
It was in turn inspired by the 1996 Linux kernel module [afpfs](https://github.com/heksterb/afpfs) by Ben Hekster.

It also contains elements from a [historical afpfs-ng fork](https://github.com/simonvetter/afpfs-ng)
created by Simon Vetter in 2015, which added IPv6 support, UTF8 support and various bug fixes
from the Boxee and XBMC (Kodi) projects.

A heartfelt thank you to everyone who has contributed to making AFP a cross-platform
file sharing platform over the years!
