# Packaging

[Development policy and release checklist](../DEVELOPMENT.md#packaging-policy) · [Validation records](RELEASES.md)

**Obtain packaging approval first.** These commands build local unsigned candidates;
they do not install or publish them. Public distribution is blocked on application
licensing and dependency/source-compliance review. See [dependency notices](../packaging/THIRD_PARTY.md).

## AppImage

Requires Make, Python 3.11+, and a running local Docker engine (or Podman). No host
native compiler or .NET installation is needed.

```bash
make test-appimage
# Alternative engine:
make test-appimage CONTAINER_ENGINE=podman
```

The first build downloads a pinned Debian 12 base, verified .NET SDK, distro
dependencies, NuGet packages, and verified appimagetool 1.9.1. Current pins are SDK
10.0.402 and runtime 10.0.13; update deliberately for security releases. The baseline
is x86-64/glibc 2.36. glibc/loader and host audio services/drivers are not bundled.

Sources arrive via an allowlisted read-only snapshot excluding the game, library,
SDKs, caches, and artifacts. Only the Dockerfile/SDK installer enter toolchain layers.
Compilation, restores, and tests run in disposable containers. Validation covers
relocation, no installed .NET/tools on PATH, synthetic indexing, generated-audio/PTY
playback, settings, fake Discord, and isolated Last.fm HTTPS. A minimal Debian smoke
test checks standalone indexing and CA fallback selection. Optional .NET LTTng is omitted.

Outputs:

```text
artifacts/dist/osu-player-x86_64.AppImage
artifacts/dist/osu-player-x86_64.AppImage.sha256
artifacts/dist/osu-player-x86_64.AppImage.manifest.json
```

Outputs replace prior files only after tests, checksum, and baseline checks pass.
Preserve previous candidates before building. Embedded notices/manifest record native
library hashes/packages and runtime/glibc versions; collected licenses alone do not
satisfy redistribution obligations. AppRun prefers host CA roots, respecting explicit
`SSL_CERT_FILE`, with a hashed bundled fallback. TLS verification stays enabled.

`make appimage` also builds with container checks; `make test-appimage` is the logged
entry point. `make appimage-local` uses host development dependencies and may raise
the glibc floor: it is not the portable release path. `make release` produces only an
optimized C++ binary at `artifacts/release/osu-player`, separate from ordinary/sanitized
objects; it still needs native libraries and an indexer.

### Container storage

The reusable image is `osu-player-appimage-toolchain:net10-debian12`. Disposable
containers remove temporary source/build/restore/extraction trees; no persistent Docker
volumes are created. Source-only builds do not accumulate compilation/package layers.
Builds still need temporary disk space and restore time. The local engine must access
the checkout; Docker uses host UID/GID and Podman uses `keep-id`.

Inspect with `docker system df` and `docker image ls`. With separate cleanup approval,
`make clean-appimage-toolchain` removes only the project's tagged image. It does not
purge global build caches, unrelated data, artifacts, or private `.tools/` SDKs.
Never blindly run `docker system prune -a --volumes`.

## Native Arch package

Use a fully updated Arch x86-64 system and run as a normal user. Install development
dependencies explicitly; the builder never uses sudo or changes installed packages:

```bash
sudo pacman -Syu --needed base-devel python dotnet-sdk-10.0 ffmpeg json-c sdl2-compat ncurses curl openssl ca-certificates
# After packaging approval:
make test-arch-package
```

The builder invokes `makepkg`, builds an optimized player and framework-dependent
linux-x64 indexer, and runs native/synthetic tests. Initial restore uses the network
and committed `packages.arch.lock.json`. Runtime libraries/.NET are system-provided;
explicit FFmpeg SONAME dependencies guard against major-version mismatches. Rebuild
after incompatible ABI upgrades.

`artifacts/arch/` contains the package, checksum, allowlisted working-tree source
snapshot (including uncommitted sources), and checksum-filled PKGBUILD. The tracked
`packaging/arch/PKGBUILD` is a template; use Make targets rather than building it directly.
Installed paths are `/usr/bin/osu-player` and `/usr/lib/osu-player/indexer`.

`make test-arch-package` builds, then tests extracted metadata/dependencies/ABI, indexer
discovery, dummy playback/PTY, settings, fake Discord, and isolated Last.fm HTTPS. It
does not install the package or access a real library. `make arch-package` builds
with build-time tests but omits extracted-package validation.

For controlled developer validation only:

```bash
DOTNET_ROOT="$PWD/.tools/dotnet10" make test-arch-package ARCH_PACKAGE_FLAGS=--nodeps
```

This bypasses makepkg dependency checks when pacman-managed .NET packages are absent.
Verify native dependencies separately. It does **not** validate installation or pacman
dependency resolution and is not the normal release-environment setup.
