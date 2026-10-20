# Base of the performance image, built once per pinned netatalk commit: the
# netatalk testsuite image (server, release build, container entrypoint) plus
# the packages every performance run needs, so Samba, libfuse, the CIFS mount
# helper and perf move only when the pin moves. test/perf/Dockerfile puts the
# client of each PR on top.
#
# BASE_IMAGE is the netatalk testsuite image built with BUILDTYPE=release at
# the commit .github/workflows/perf.yml pins; the caller passes it with
# --build-arg, as the workflow and test/perf/README.md do.

ARG BASE_IMAGE

FROM ${BASE_IMAGE}

# fuse3 mounts the volume; samba-server, samba-common-tools and cifs-utils
# serve and mount the Samba (SMB) share; binutils, musl-dbg, perf and perl
# are the flamegraph pipeline of the server's debug image.
RUN apk update \
&&  apk add --no-cache \
    binutils \
    cifs-utils \
    fuse3 \
    musl-dbg \
    perf \
    perl \
    samba-common-tools \
    samba-server

ADD https://raw.githubusercontent.com/brendangregg/FlameGraph/41fee1f99f9276008b7cd112fca19dc3ea84ac32/stackcollapse-perf.pl /opt/FlameGraph/stackcollapse-perf.pl
ADD https://raw.githubusercontent.com/brendangregg/FlameGraph/41fee1f99f9276008b7cd112fca19dc3ea84ac32/flamegraph.pl /opt/FlameGraph/flamegraph.pl
RUN chmod 0755 /opt/FlameGraph/stackcollapse-perf.pl /opt/FlameGraph/flamegraph.pl
