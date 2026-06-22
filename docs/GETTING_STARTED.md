# Getting Started with Netatalk Client

This is a quick guide on how to use the two different AFP clients
included with the Netatalk Client suite: the FUSE client and the command line client.

## The FUSE client

This will let you mount remote filesystems over AFP,
and access them as if they were local filesystems.

As the user who will be needing to access the files, start the management daemon
by running:

    afpfsd --manager

This should fork off.  You should see messages in /var/log/messages.  For more
details, run it with the '--debug' option to see detailed debug info.

Note that if afpfsd is not running, afp_client or mount_afpfs will start it
automatically, so in most cases you don't need to start it manually.

Mount the _File Sharing_ volume from afpserver.local on /home/myuser/fusemount
authenticated as user _myuser_ (you will be prompted for the password):

    % mount_afpfs "afp://myuser@afpserver.local/File Sharing" /home/myuser/fusemount

After inputting the password when prompted, you should be able to access files on _fusemount_.

Mount the _Dropbox_ volume as guest without authentication (the server must allow guest access):

    % mount_afpfs "afp://afpserver.local/Dropbox" /home/myuser/fusemount

The same, but forcing the UAM of your choice with the _AUTH_ parameter (usually not needed):

    % mount_afpfs "afp://myuser;AUTH=dhx:-@afpserver.local/File Sharing" /home/myuser/fusemount

You can see status by running 'afp_client status'.  See afpfsd(1),
mount_afpfs(1) and afp_client(1) for more info.

### Mounting on boot

For operating systems that support it, add an AFP mount to fstab so it mounts automatically on boot.
The FUSE filesystem source is 'afpfs', and can be configured as follows:

1. create a file called '/etc/fuse.conf' with one line:
`user_allow_other`
2. make sure that any user doing a mount is a member of the group 'fuse' so it can read and write to /dev/fuse
3. create an entry in /etc/fstab entry in the following format:

    afpfs#afp://username:mypass10.211.55.2/myafpvol /tmp/xa20 fuse user=myuser,group=fuse 0 0

Here, username and mypass are the login information on the server 10.211.55.2.
The volume name is myafpvol.  /tmp/xa20 is the name of the mountpoint.
The user= field is the local user, group needs to be the same the group owner of /dev/fuse (which is typically fuse).

Yes, you will need to put your password in clear text.  There is currently no facility to handle open directory.
Patches welcome.

## Running the command line client

There are two modes:

### interactive mode

afpcmd is a command line tool like an FTP client.

Just run:

    afpcmd "afp://username@servername/volumename"

If you enter no volumename, it shows which ones are available.
You can put a password after the username - "username:password" - but it's usually better
to let it prompt you for the password so it doesn't end up in your shell history.

Examples of available commands:

- get _filename_: retrieves the filename
- put _filename_: send the file
- get -r _directory_ / put -r _directory_: recursively transfer a directory
- cp -r _source_ _target_: recursively copy a remote directory
- chmod -r _mode_ _directory_ / rm -r _directory_: recursively change or remove a tree
- ls: show directory listings
- xattr / finderinfo / resourcefork: inspect and modify AFP metadata

Others are available too; touch, chmod, chown, rm, mv, etc.  See
afpcmd(1) for more.

### batch transfer

This will let you quickly transfer one file or recursively a directory,
and then return you to the command prompt.

E.g.

    > afpcmd afp://user:pass@server/alexdevries/linux-2.6.14.tar.bz2 .
    Connected to server Cubalibre using UAM "DHX2"
    Connected to volume alexdevries
        Getting file /linux-2.6.14.tar.bz2
    Transferred 39172170 bytes in 2.862 seconds. (13687 kB/s)

Transfers preserve FinderInfo, resource forks, generic extended attributes,
file modes, and modification times by default.
Use '-M netatalk', '-M xattr', or '-M macos' to select local metadata storage,
and '-M none' to transfer only the data fork.

See afpcmd(1) for more information.

## getting status

You can get status information on servers with 'afpgetstatus _servername_'.  
This provides some information without having to log in.

See afpgetstatus(1) for more information.
