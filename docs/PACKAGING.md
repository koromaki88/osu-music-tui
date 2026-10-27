# Packaging

[Development policy and release checklist](../DEVELOPMENT.md#packaging-policy) · [Validation records](RELEASES.md)

> [!IMPORTANT]
> Obtain packaging approval first. These commands build local unsigned candidates;
> they do not install or publish them. Public distribution is blocked on application
> licensing and dependency/source-compliance review. See [dependency notices](../packaging/THIRD_PARTY.md).

## AppImage

Requires Make, Python 3.11+, and a running local Docker engine (or Podman). No host
native compiler or .NET installation is needed.

```bash
make test-appimage
# Alternative engine:
make test-appimage CONTAINER_ENGINE=podman
```

### Toolchain and validation

| Component | Pin / baseline |
|---|---|
| Base system | Pinned Debian 12 image |
| .NET SDK | 10.0.402, checksum-verified |
| .NET runtime | 10.0.13 |
| appimagetool | 1.9.1, checksum-verified |
| Target | x86-64 / glibc 2.36 |

The first build downloads the base, SDK, distro dependencies, NuGet packages, and
appimagetool. Update pins deliberately for security releases. glibc/loader and audio
services/drivers are host-provided, not bundled. Optional .NET LTTng tracing is omitted.

Sources arrive via an allowlisted read-only snapshot excluding the game, library,
SDKs, caches, and artifacts. Only the Dockerfile/SDK installer enter toolchain layers;
compilation, restores, and tests run in disposable containers.

Validation covers relocation, no installed .NET/tools on PATH, synthetic indexing,
generated-audio/PTY playback, settings, fake Discord, and isolated Last.fm HTTPS.
A minimal-image smoke test checks standalone indexing and CA fallback selection.

### Outputs

```text
artifacts/dist/osu-music-player-0.6.1-x86_64.AppImage
artifacts/dist/osu-music-player-0.6.1-x86_64.AppImage.sha256
artifacts/dist/osu-music-player-0.6.1-x86_64.AppImage.manifest.json
```

The filename uses `osu-music-player-<VERSION>-x86_64.AppImage`, reading the release
number from the root `VERSION` file. Different versions coexist; rebuilding the same
version replaces its files only after tests, checksum, and baseline checks pass.
Preserve previous candidates before building. Embedded notices/manifest record native
library hashes/packages and runtime/glibc versions; collected licenses alone do not
satisfy redistribution obligations.

AppRun prefers host CA roots, respecting explicit `SSL_CERT_FILE`, with a hashed
bundled fallback. TLS verification stays enabled.

The fallback must pass `tools/check_ca_bundle.py`, an offline gate rejecting known
TLS trust anchors implicated in the October 2026 eMudhra compromise. The manifest
records its policy and certificate count alongside the bundle hash; extraction
tests repeat this check. This is not a general revocation checker. Keep host CA
packages updated, including any explicit `SSL_CERT_FILE` override. The Dockerfile's
`CA_SECURITY_REFRESH` argument invalidates the cached certificate-install layer for
deliberate security refreshes; future incidents still need policy review and updates.

### Other build targets

| Target | Purpose / limitation |
|---|---|
| `make test-appimage` | Logged build-and-validation entry point |
| `make appimage` | Container build with checks |
| `make appimage-local` | Host dependencies; may raise glibc floor—not the portable release path |
| `make release` | Optimized C++ binary only at `artifacts/release/osu-player`, separate from ordinary/sanitized objects; still needs native libraries/indexer |

### Container storage

The reusable image is `osu-player-appimage-toolchain:net10-debian12`. Disposable
containers remove source/build/restore/extraction trees; no persistent Docker volumes
are created. Source-only builds do not accumulate compilation/package layers, but
still need temporary disk space and restore time.

The local engine must reach the checkout. Docker uses host UID/GID; Podman uses `keep-id`.

- **Inspect:** `docker system df` and `docker image ls`.
- **Authorized cleanup:** `make clean-appimage-toolchain` removes only the project's
  tagged image—not global build caches, unrelated data, artifacts, or private `.tools/` SDKs.

> [!WARNING]
> Never blindly run `docker system prune -a --volumes`.

## Native Arch package

Use a fully updated Arch x86-64 system and run as a normal user. Install development
dependencies explicitly; the builder never uses sudo or changes installed packages:

```bash
sudo pacman -Syu --needed base-devel python dotnet-sdk-10.0 ffmpeg json-c sdl2-compat ncurses curl openssl ca-certificates
# After packaging approval:
make test-arch-package
```

### Build and outputs

The builder invokes `makepkg`, builds an optimized player/framework-dependent
linux-x64 indexer, and runs native/synthetic tests. Initial restore uses the network
and committed `packages.arch.lock.json`. Runtime libraries/.NET are system-provided;
explicit FFmpeg SONAME dependencies guard against major-version mismatches. Rebuild
after incompatible ABI upgrades.

- **Artifacts:** package, checksum, allowlisted working-tree source snapshot (including
  uncommitted sources), and checksum-filled PKGBUILD under `artifacts/arch/`.
- **Template:** `packaging/arch/PKGBUILD` is not directly buildable; use Make targets.
- **Installed paths:** `/usr/bin/osu-player` and `/usr/lib/osu-player/indexer`.

### Validation

- **`make test-arch-package`:** builds, then checks extracted metadata/dependencies/ABI,
  indexer discovery, dummy playback/PTY, settings, fake Discord, and isolated Last.fm HTTPS.
- **`make arch-package`:** builds with build-time tests, but omits extracted-package validation.
- **Safety:** neither installs the package nor accesses a real library.

### Private SDK: developer validation only

```bash
DOTNET_ROOT="$PWD/.tools/dotnet10" make test-arch-package ARCH_PACKAGE_FLAGS=--nodeps
```

> [!WARNING]
> `--nodeps` bypasses makepkg checks when pacman-managed .NET packages are absent.
> Verify native dependencies separately. This does **not** validate installation or
> pacman dependency resolution and is not the normal release-environment setup.
