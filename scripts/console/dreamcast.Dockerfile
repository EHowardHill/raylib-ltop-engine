# Dreamcast toolchain image: KallistiOS + kos-ports + raylib (raylib4Consoles fork).
# Base image ships the SH4 toolchain, KOS, kos-ports libGL (GLdc) and mkdcdisc:
# https://hub.docker.com/r/maishuji/dc-kos-image
FROM maishuji/dc-kos-image:16.2.0-06sep26

USER root
ENV KOS_BASE=/opt/toolchains/dc/kos
ENV KOS_PORTS=/opt/toolchains/dc/kos-ports

# raylib for Dreamcast, installed into $(KOS_PORTS)/raylib4dc/inst/{include,lib}
# (audio is on by default for PLATFORM_DREAMCAST in the raylib4Consoles makefile).
RUN bash -c 'source $KOS_BASE/environ.sh \
    && git clone --depth 1 -b raylib4Consoles_6.0 https://github.com/raylib4Consoles/raylib /tmp/raylib \
    && cd /tmp/raylib/src \
    && make PLATFORM=PLATFORM_DREAMCAST -j"$(nproc)" \
    && make PLATFORM=PLATFORM_DREAMCAST install \
    && rm -rf /tmp/raylib'

WORKDIR /src
