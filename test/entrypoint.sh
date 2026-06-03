#!/bin/sh

# Netatalk Client AFP client testsuite container entrypoint
# Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.

set -e
TEST_USR="test_usr"
TEST_PWD="test_pwd"
adduser --no-create-home --disabled-password --gecos '' "$TEST_USR" > /dev/null 2>&1 || true
echo "$TEST_USR:$TEST_PWD" | chpasswd
[ -d /mnt/afpfs ] || mkdir /mnt/afpfs
chmod 2755 /mnt/afpfs
chown "$TEST_USR:$TEST_USR" /mnt/afpfs
rm -f /var/lock/netatalk

cat << EOF > /etc/netatalk/afp.conf
[Global]
log file = /var/log/afpd.log
log level = default:debug
server name = afpfs_testsrv
uam list = uams_guest.so uams_dhx2.so
[afpfs_test]
path = /mnt/afpfs
volume name = afpfs_test
EOF

netatalk
sleep 2
prove ./test_afpgetstatus.t
prove ./test_afpcmd_batch.t
pkill -x afpsld || true
while pgrep -x afpsld > /dev/null 2>&1; do sleep 0.1; done
prove ./test_afpcmd_interactive.t
