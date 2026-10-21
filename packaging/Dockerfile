# Native x86-64 baseline: Debian 12 / glibc 2.36, not the build host's glibc.
# Pin base images and SDK/packaging-tool checksums; apt security updates intentionally
# remain enabled. See the generated manifest for the exact native package set.
FROM debian:bookworm-slim@sha256:a4672c0cb26fbdde88e38fa2dfb6c681942306680e41e4378b28770b6e79ee91 AS toolchain
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 LANG=C.UTF-8 DOTNET_ROOT=/opt/dotnet PATH=/opt/dotnet:$PATH
# Bump deliberately for trust-store incidents; invalidate cached apt installation.
ARG CA_SECURITY_REFRESH=2026-10-21
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libicu72 libssl3 libgssapi-krb5-2 \
    g++ make pkg-config python3 ffmpeg libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev libsdl2-dev libncurses-dev libjson-c-dev ncurses-term libcurl4-openssl-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
# Keep packaging-only utilities in a separate cached layer.
RUN apt-get update && apt-get install -y --no-install-recommends file desktop-file-utils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY tools/install_dotnet.py tools/install_dotnet.py
RUN python3 tools/install_dotnet.py --destination /opt/dotnet
LABEL io.osu-player.role="appimage-toolchain"
# Project sources and build products deliberately never enter image layers.
# tools/build_appimage.py runs the build and minimal-image smoke test in
# disposable containers, retaining only this reusable toolchain image.
