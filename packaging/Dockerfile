# Native x86-64 baseline: Debian 12 / glibc 2.36, not the build host's glibc.
# Pin base images and packaging-tool checksums; apt security updates intentionally
# remain enabled. See the generated manifest for the exact native package set.
FROM mcr.microsoft.com/dotnet/sdk:8.0.426-bookworm-slim@sha256:8467fac908c4b0ac0496335989ca196721537277e42474a40f61e34fdc8ad772 AS build
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make pkg-config python3 ffmpeg libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev libsdl2-dev libncurses-dev libjson-c-dev ncurses-term \
    && rm -rf /var/lib/apt/lists/*
# Keep packaging-only utilities in a separate cached layer.
RUN apt-get update && apt-get install -y --no-install-recommends file desktop-file-utils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make -j2 test && make test-indexer
RUN dotnet run --project tests/osu-indexer-tests --configuration Release --no-build --no-restore \
    -- --fixture-output /synthetic-fixture
RUN make appimage-local DOTNET=dotnet
RUN python3 tests/appimage_integration.py --dotnet dotnet

# Prove the package loads and reads an actual synthetic Realm in a minimal image
# with no .NET, FFmpeg, SDL, ncurses, or json-c installation. This never runs osu!.
FROM debian:bookworm-slim@sha256:a4672c0cb26fbdde88e38fa2dfb6c681942306680e41e4378b28770b6e79ee91 AS smoke
WORKDIR /work
COPY --from=build /src/artifacts/dist/ /work/
COPY --from=build /synthetic-fixture/ /work/fixture/
RUN ./osu-player-x86_64.AppImage --appimage-extract-and-run --help \
    && ./osu-player-x86_64.AppImage --appimage-extract > /dev/null \
    && LD_LIBRARY_PATH=/work/squashfs-root/usr/lib \
       /work/squashfs-root/usr/lib/osu-player/indexer/osu-indexer --osu-dir /work/fixture > /work/library.ndjson \
    && /work/squashfs-root/AppRun library /work/library.ndjson

FROM scratch AS artifact
COPY --from=smoke /work/osu-player-x86_64.AppImage /
COPY --from=smoke /work/osu-player-x86_64.AppImage.sha256 /
COPY --from=smoke /work/osu-player-x86_64.AppImage.manifest.json /
