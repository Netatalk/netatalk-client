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

run_test() {
    test="$1"
    echo "==> Running $test"
    prove "$test"
}

live_afpsld_pids() {
    ps -C afpsld -o pid=,stat= 2> /dev/null \
        | awk '$2 !~ /^Z/ { print $1 }'
}

stop_afpsld() {
    pids=$(live_afpsld_pids)

    if [ -z "$pids" ]; then
        return
    fi

    echo "==> Stopping afpsld: $pids"
    kill $pids 2> /dev/null || true

    attempts=0
    while [ -n "$(live_afpsld_pids)" ] && [ "$attempts" -lt 50 ]; do
        sleep 0.1
        attempts=$((attempts + 1))
    done

    pids=$(live_afpsld_pids)

    if [ -n "$pids" ]; then
        echo "afpsld did not stop after 5 seconds; forcing shutdown" >&2
        ps -C afpsld -o pid=,ppid=,stat=,etime=,cmd= >&2 || true
        kill -KILL $pids 2> /dev/null || true
    fi
}

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
run_test ./test_afpgetstatus.t
run_test ./test_afpcmd_batch.t
stop_afpsld
run_test ./test_afpcmd_interactive.t
