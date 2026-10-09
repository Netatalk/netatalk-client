ARG RUN_DEPS="\
    fuse3 \
    libgcrypt20 \
    libreadline8t64 \
    netatalk \
    perl \
    procps \
    util-linux \
    "
ARG BUILD_DEPS="\
    build-essential \
    libfuse3-dev \
    libgcrypt20-dev \
    libreadline-dev \
    meson \
    ninja-build \
    pkg-config \
    "

FROM debian:13.7-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS build

ARG RUN_DEPS
ARG BUILD_DEPS
ENV RUN_DEPS=$RUN_DEPS
ENV BUILD_DEPS=$BUILD_DEPS

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
&&  apt-get install --yes --no-install-recommends \
    $RUN_DEPS \
    $BUILD_DEPS

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

RUN meson setup build \
    -Dbuildtype=debug \
    -Denable-docs=false \
    -Denable-fuse=true \
&&  meson compile -C build

RUN meson install --destdir=/staging/ -C build \
&&  test -x /staging/usr/local/bin/afpfsd

FROM debian:13.7-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS deploy

ARG RUN_DEPS
ENV RUN_DEPS=$RUN_DEPS

COPY --from=build /staging/ /

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
&&  apt-get install --yes --no-install-recommends $RUN_DEPS \
&&  apt-get autoremove --yes \
&&  apt-get clean \
&&  rm -rf /var/lib/apt/lists/*

COPY /test/integration/entrypoint.sh /test/entrypoint.sh
COPY /test/integration/test_afpcmd_batch.t /test/test_afpcmd_batch.t
COPY /test/integration/test_afpcmd_interactive.t /test/test_afpcmd_interactive.t
COPY /test/integration/test_afpgetstatus.t /test/test_afpgetstatus.t
COPY /test/integration/test_fuse.t /test/test_fuse.t

WORKDIR /test
CMD ["/test/entrypoint.sh"]
