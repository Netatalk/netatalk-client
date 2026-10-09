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
AFP_SERVER_LOG="/var/log/afpd.log"

if [ -n "${AFP_TEST_LOG_DIR:-}" ]; then
    mkdir -p "$AFP_TEST_LOG_DIR"
    AFP_SERVER_LOG="$AFP_TEST_LOG_DIR/afpd.log"
    touch "$AFP_SERVER_LOG"
    chmod 0644 "$AFP_SERVER_LOG"
    export AFP_FUSE_DEBUG_LOG="${AFP_FUSE_DEBUG_LOG:-$AFP_TEST_LOG_DIR/afpfsd.log}"
fi

run_test() {
    test="$1"
    echo "==> Running $test"
    prove -v "$test"
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
log file = $AFP_SERVER_LOG
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

if [ "${AFP_TEST_FUSE:-0}" = 1 ]; then
    if [ ! -c /dev/fuse ]; then
        echo "FUSE tests require --device /dev/fuse --cap-add SYS_ADMIN" >&2
        exit 1
    fi
    # Only fusermount3 needs mount privileges. Run the client and its daemons
    # as the test user, with SYS_ADMIN as the helper's sole bounding capability.
    fuse_workdir=$(mktemp -d /tmp/afp-fuse-test.XXXXXX)
    chown "$TEST_USR:$TEST_USR" "$fuse_workdir"
    if [ -n "${AFP_FUSE_DEBUG_LOG:-}" ]; then
        touch "$AFP_FUSE_DEBUG_LOG"
        chown "$TEST_USR:$TEST_USR" "$AFP_FUSE_DEBUG_LOG"
        chmod 0644 "$AFP_FUSE_DEBUG_LOG"
    fi
    cd "$fuse_workdir"
    setpriv --bounding-set=-all,+sys_admin \
        --reuid="$TEST_USR" --regid="$TEST_USR" --clear-groups \
        prove -v /test/test_fuse.t
fi
