# Performance image: the per-pin base of test/perf/base.Dockerfile (server,
# container entrypoint, Samba, profiling tools) with this tree's client and
# mount benchmarks installed over it. Built by .github/workflows/perf.yml;
# test/perf/README.md has the local recipe.
#
# BASE_IMAGE is the base image of the pinned netatalk commit, passed with
# --build-arg as the workflow and test/perf/README.md do. The build stage uses
# the same Alpine release so the client links against the libraries the base
# ships.

ARG BASE_IMAGE
# perl: meson resolves the Perl test runner of test/unit at configure time
ARG BUILD_DEPS="\
    build-base \
    fuse3-dev \
    libedit-dev \
    libgcrypt-dev \
    meson \
    ninja \
    perl \
    pkgconf \
    "

FROM alpine:3.24.2@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6 AS build

ARG BUILD_DEPS
ENV BUILD_DEPS=$BUILD_DEPS

RUN apk update \
&&  apk add --no-cache $BUILD_DEPS

WORKDIR /netatalk-client-code
COPY cmdline/ ./cmdline/
COPY common/ ./common/
COPY control/ ./control/
COPY daemon/ ./daemon/
COPY discovery/ ./discovery/
COPY fuse/ ./fuse/
COPY include/ ./include/
COPY lib/ ./lib/
COPY test/ ./test/
COPY meson_options.txt .
COPY meson.build .
RUN rm -rf build

# Release, as the server's perf image; -g adds the symbols perf resolves and
# changes no generated code.
RUN meson setup build \
    -Dbuildtype=release \
    -Dc_args=-g \
    -Denable-docs=false \
    -Denable-fuse=true \
    -Dwith-testsuite=true \
    -Dzeroconf=disabled \
&&  meson compile -C build

RUN meson install --destdir=/staging/ -C build \
&&  test -x /staging/usr/local/bin/afpfsd \
&&  test -x /staging/usr/local/bin/vfs_lantest

FROM ${BASE_IMAGE} AS deploy

COPY --from=build /staging/ /
COPY test/perf/entrypoint.sh /entrypoint_perf.sh

ENTRYPOINT ["/bin/sh", "/entrypoint_perf.sh"]
