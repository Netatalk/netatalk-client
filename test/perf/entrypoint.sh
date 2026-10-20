#!/bin/sh

# Netatalk Client performance container entrypoint
# Copyright (C) 2026 Andy Lemin (andylemin)
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# Starts the Netatalk server of the base image through the testsuite
# entrypoint, mounts its volume with this image's afpfsd and, when MOUNTS
# names smb, exports the same directory through Samba and mounts it with the
# kernel CIFS client. One benchmark runs over each mount in turn and writes
# its CSV into the /results directory the caller mounts:
#   TESTSUITE=lan     vfs_lantest    -> /results/lantest-<mount>.csv
#   TESTSUITE=speed   vfs_speedtest  -> /results/speedtest-<mount>.csv
# With FLAMEGRAPH set, perf samples the afpfsd processes during the AFP run
# and writes /results/flamegraph.svg.
#
# Environment, beyond the server variables the testsuite entrypoint takes
# (AFP_USER, AFP_PASS, AFP_GROUP, SHARE_NAME, the afp.conf knobs and
# AFP_SERVER_CPUS, which pins smbd as well):
#   TESTSUITE            lan | speed (required)
#   MOUNTS               afp, smb, or both in run order (default afp)
#   AFP_TEST_ITERATIONS  iterations, or measured runs per size (default 10)
#   AFP_SUBTESTS         speed operations (default Read,Write,Copy,ServerCopy)
#   AFP_VERSION_LABEL    AFP version the mount requests (default 3.4)
#   AFP_TEST_CPUS        taskset list for afpfsd and the benchmark
#   TEST_FLAGS           flags placed before the benchmark's own
#   FLAMEGRAPH           profile afpfsd; PERF_FREQ sets the rate (907 Hz)
#   SMB_PORT             smbd port (default 445)

set -eu

RESULTS=/results
AFP_MNT=/mnt/afpfs
SMB_MNT=/mnt/smb
SMB_PORT="${SMB_PORT:-445}"
FLAMEGRAPH_DIR=/opt/FlameGraph
PERF_FREQ="${PERF_FREQ:-907}"
SERVER_PIN=""
PIN=""
PERF_PID=""
RC=0

fatal() {
    echo "FATAL: $*" >&2
    exit 1
}

start_profiling() {
    pids=$(pgrep afpfsd | tr '\n' ',' | sed 's/,$//')
    [ -n "$pids" ] || fatal "no afpfsd process to profile"
    echo "*** perf record on afpfsd PIDs $pids at $PERF_FREQ Hz" >&2
    perf record --inherit -F "$PERF_FREQ" --call-graph dwarf -p "$pids" \
        -o /tmp/perf.data &
    PERF_PID=$!
    sleep 1
}

# Folded stacks, the SVG, the ten busiest leaf functions and the share of
# samples whose leaf is a symbol of afpfsd or libafpclient.
stop_profiling() {
    kill -INT "$PERF_PID" 2> /dev/null || true
    wait "$PERF_PID" 2> /dev/null || true
    sync
    [ -f /tmp/perf.data ] || fatal "perf record left no data"
    # perf 7.1 keeps one libdw session per process and binds it to the first
    # thread it unwinds, so every other thread of the multithreaded afpfsd
    # would come out as a bare comm with no user frames (ESRCH; mainline perf
    # attaches by pid). One perf script per thread gives each its own session.
    for tid in $(perf script -i /tmp/perf.data -F tid | sort -un); do
        perf script -i /tmp/perf.data --tid "$tid"
    done > /tmp/perf.script
    "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" /tmp/perf.script > /tmp/perf.folded
    perl -pi -e 's/;__clone(?:;__clone)+/;__clone/g' /tmp/perf.folded
    "$FLAMEGRAPH_DIR/flamegraph.pl" \
        --title "Netatalk Client afpfsd flamegraph, $TESTSUITE on the Netatalk (AFP) FUSE mount, $(date -u '+%Y-%m-%d %H:%M UTC')" \
        --subtitle "release build with -g, perf @ ${PERF_FREQ} Hz, DWARF unwinding" \
        /tmp/perf.folded > /tmp/flamegraph.svg
    echo "*** Folded stacks: $(wc -l < /tmp/perf.folded) unique stacks"
    awk '{
            count = $NF
            stack = $0
            sub(/ [0-9]+$/, "", stack)
            n = split(stack, parts, ";")
            totals[parts[n]] += count
         }
         END { for (f in totals) print totals[f], f }' /tmp/perf.folded \
        | sort -rn | head -10 > /tmp/leaves
    while read -r cnt fn; do
        echo "FLAMEGRAPH_LEAF: $cnt $fn"
    done < /tmp/leaves
    nm /usr/local/bin/afpfsd /usr/local/lib/libafpclient.so* \
        | awk '/^[0-9a-f]+ [TtWw] / { print $3 }' | sort -u > /tmp/client_syms
    awk -v symfile=/tmp/client_syms '
        BEGIN { while ((getline s < symfile) > 0) syms[s] = 1 }
        {
            count = $NF
            stack = $0
            sub(/ [0-9]+$/, "", stack)
            n = split(stack, parts, ";")
            total += count
            if (parts[n] in syms) self += count
        }
        END {
            printf "FLAMEGRAPH_SELF_TIME: %.1f%%\n",
                (total > 0 ? 100 * self / total : 0)
        }' /tmp/perf.folded
    cp /tmp/flamegraph.svg "$RESULTS/flamegraph.svg"
}

mount_afp() {
    mkdir -p "$AFP_MNT"
    # The manager logs this line after its socket listens; afpc probed
    # before that would start a second, unpinned manager of its own.
    # shellcheck disable=SC2086
    $PIN afpfsd --foreground --manager --logmethod stdout 2>&1 | tee /tmp/afpfsd.log >&2 &
    wait_for 10 grep -q "Starting manager daemon on" /tmp/afpfsd.log \
        || fatal "afpfsd did not start"
    afpc fs mount -u "$AFP_USER" -p "$AFP_PASS" -a dhx2 \
        -f "${AFP_VERSION_LABEL:-3.4}" "127.0.0.1:$SHARE_NAME" "$AFP_MNT" >&2 \
        || fatal "AFP mount failed"
    mountpoint -q "$AFP_MNT" || fatal "$AFP_MNT is not a mount"
}

unmount_afp() {
    afpc fs unmount "$AFP_MNT" >&2 || echo "WARNING: unmount of $AFP_MNT failed" >&2
    afpc fs exit >&2 || true
}

# Samba exports the directory the AFP volume serves, with the Apple
# interoperability modules a Mac-serving Samba carries and Samba's default
# locking and caching; the AFP user gets the same password.
mount_smb() {
    mkdir -p "$SMB_MNT" /var/log/samba /var/lib/samba/private /run/samba /etc/samba
    cat << EOF > /etc/samba/smb.conf
[global]
    security = user
    map to guest = never
    smb ports = $SMB_PORT
    log file = /var/log/samba/smbd.log
    log level = 1
    ea support = yes
    vfs objects = catia fruit streams_xattr

[$SHARE_NAME]
    path = ${NETATALK_SHARE_DIR:-/mnt/afpshare}
    read only = no
    valid users = $AFP_USER
EOF
    printf '%s\n%s\n' "$AFP_PASS" "$AFP_PASS" | smbpasswd -a -s "$AFP_USER" >&2 \
        || fatal "smbpasswd could not add $AFP_USER"
    # shellcheck disable=SC2086
    $SERVER_PIN smbd --daemon
    wait_for 30 afp_listening "$SMB_PORT" 127.0.0.1 || fatal "smbd did not start"
    mount -t cifs "//127.0.0.1/$SHARE_NAME" "$SMB_MNT" \
        -o "user=$AFP_USER,pass=$AFP_PASS,port=$SMB_PORT,vers=3.1.1" \
        || fatal "CIFS mount failed; the host kernel needs the cifs module"
}

unmount_smb() {
    umount "$SMB_MNT" || echo "WARNING: unmount of $SMB_MNT failed" >&2
}

# One suite over the mount at $1, named $2; its CSV is /results/$STEM-$2.csv.
run_suite() {
    mnt="$1"
    name="$2"
    out="$RESULTS/$STEM-$name.csv"
    set +e
    case "$TESTSUITE" in
        lan)
            # shellcheck disable=SC2086
            $PIN vfs_lantest ${TEST_FLAGS:-} -n "${AFP_TEST_ITERATIONS:-10}" \
                -P "$mnt" > "$out"
            ;;
        speed)
            # shellcheck disable=SC2086
            $PIN vfs_speedtest ${TEST_FLAGS:-} -n "${AFP_TEST_ITERATIONS:-10}" \
                -f "${AFP_SUBTESTS:-Read,Write,Copy,ServerCopy}" -P "$mnt" > "$out"
            ;;
        *)
            fatal "TESTSUITE must be lan or speed"
            ;;
    esac
    rc=$?
    set -e
    echo "==== $TESTSUITE over $name completed (exit code: $rc) ====" >&2
    [ "$rc" -eq 0 ] || RC=$rc
}

case "${TESTSUITE:-}" in
    lan) STEM=lantest ;;
    speed) STEM=speedtest ;;
    *) fatal "TESTSUITE must be lan or speed" ;;
esac

[ -n "${AFP_USER:-}" ] && [ -n "${AFP_PASS:-}" ] && [ -n "${SHARE_NAME:-}" ] \
    || fatal "AFP_USER, AFP_PASS and SHARE_NAME must be set"

[ -d "$RESULTS" ] || fatal "no $RESULTS directory; mount one with -v <dir>:$RESULTS"

if [ -n "${AFP_SERVER_CPUS:-}" ]; then
    SERVER_PIN="taskset -c $AFP_SERVER_CPUS"
fi

if [ -n "${AFP_TEST_CPUS:-}" ]; then
    PIN="taskset -c $AFP_TEST_CPUS"
fi

# The testsuite entrypoint with no suite keeps netatalk in the foreground,
# pinned through AFP_SERVER_CPUS.
TESTSUITE="" /entrypoint.sh >&2 &
# shellcheck source=/dev/null
. /integration/lib.sh
wait_for 30 afp_listening 548 127.0.0.1 || fatal "the AFP server did not start"

for m in ${MOUNTS:-afp}; do
    case "$m" in
        afp)
            mount_afp
            START=$(date +%s)
            if [ -n "${FLAMEGRAPH:-}" ]; then
                start_profiling
            fi
            run_suite "$AFP_MNT" afp
            echo "FLAMEGRAPH_RUNTIME: $(($(date +%s) - START))s" >&2
            if [ -n "${FLAMEGRAPH:-}" ]; then
                stop_profiling
            fi
            unmount_afp
            ;;
        smb)
            mount_smb
            run_suite "$SMB_MNT" smb
            unmount_smb
            ;;
        *)
            fatal "MOUNTS names '$m'; afp and smb are the mounts"
            ;;
    esac
done

exit "$RC"
