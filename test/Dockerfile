ARG RUN_DEPS="\
    libgcrypt20 \
    libreadline8t64 \
    netatalk \
    perl \
    "
ARG BUILD_DEPS="\
    build-essential \
    libgcrypt20-dev \
    libreadline-dev \
    meson \
    ninja-build \
    pkg-config \
    "

FROM debian:13.5-slim@sha256:b6e2a152f22a40ff69d92cb397223c906017e1391a73c952b588e51af8883bf8 AS build

ARG RUN_DEPS
ARG BUILD_DEPS
ENV RUN_DEPS=$RUN_DEPS
ENV BUILD_DEPS=$BUILD_DEPS

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
&&  apt-get install --yes --no-install-recommends \
    $RUN_DEPS \
    $BUILD_DEPS

WORKDIR /afpfs-ng-code
COPY cmdline/ ./cmdline/
COPY daemon/ ./daemon/
COPY fuse/ ./fuse/
COPY include/ ./include/
COPY lib/ ./lib/
COPY meson_options.txt .
COPY meson.build .
RUN rm -rf build

RUN meson setup build \
    -Dbuildtype=debug \
    -Denable-docs=false \
    -Denable-fuse=true \
&&  meson compile -C build

RUN meson install --destdir=/staging/ -C build

FROM debian:13.5-slim@sha256:b6e2a152f22a40ff69d92cb397223c906017e1391a73c952b588e51af8883bf8 AS deploy

ARG RUN_DEPS
ENV RUN_DEPS=$RUN_DEPS

COPY --from=build /staging/ /

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
&&  apt-get install --yes --no-install-recommends $RUN_DEPS \
&&  apt-get autoremove --yes \
&&  apt-get clean \
&&  rm -rf /var/lib/apt/lists/*

COPY /test/entrypoint.sh /test/entrypoint.sh
COPY /test/test_afpcmd_batch.t /test/test_afpcmd_batch.t
COPY /test/test_afpcmd_interactive.t /test/test_afpcmd_interactive.t
COPY /test/test_afpgetstatus.t /test/test_afpgetstatus.t

WORKDIR /test
CMD ["/test/entrypoint.sh"]
