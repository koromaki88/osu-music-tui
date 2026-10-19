# Local release validation

[Changelog](../CHANGELOG.md) · [Packaging](PACKAGING.md) · [Release checklist](../DEVELOPMENT.md#release-checklist)

> [!IMPORTANT]
> These are local unsigned packages, not publication approval. Public distribution
> remains blocked on application-license selection and dependency/source-compliance review.

- **Fixtures:** synthetic libraries/audio and fake services only.
- **Excluded:** game launch, real-library scan, installation, publication, commits, and tags.

## 0.4.0 — Last.fm

| Format | Artifact | Size | Result |
|---|---|---|---|
| AppImage | `artifacts/dist/osu-player-x86_64.AppImage` | 101.2 MiB | Passed `make test-appimage` |
| Arch 0.4.0-1 | `artifacts/arch/osu-music-player-0.4.0-1-x86_64.pkg.tar.zst` | 4.4 MiB | Passed `make test-arch-package` with private SDK / `--nodeps` |

### Passed checks

- **Native:** `make test` and synthetic `make test-indexer`.
- **Both packages:** indexing, dummy playback/PTY/settings, Discord fake IPC, Last.fm
  setup, and isolated real-libcurl HTTPS authorization/Now Playing/scrobbling/restart,
  including certificate/hostname rejection.
- **AppImage:** relocation, no installed .NET/tools on PATH, WAV/MP3/Ogg playback,
  and minimal-container CA fallback selection. Baseline: x86-64/glibc 2.36.
- **Integrity:** final checksums independently verified.

### Limitations and live verification

- **Arch:** installation/dependency resolution untested; `--nodeps` was needed because
  pacman-managed .NET packages were absent.
- **AppImage:** FUSE mounting untested; extraction/run validated.
- **CMake/Ninja:** unverified.
- **User-confirmed native behavior:** live Discord rendering and Last.fm authorization,
  Now Playing, and scrobble submission. Automated tests never contacted those services.

### Evidence and retained artifacts

All logs below are under `artifacts/`:

| Evidence | Logs |
|---|---|
| Native/synthetic | `tests.log`, `test-indexer.log` |
| Packages | `test-appimage.log`, `arch-package.log`, `test-arch-package.log` |
| Passed Last.fm ASan/UBSan suites | `lastfm-sanitizer.log`, `lastfm-scrobble-sanitizer.log` |

Earlier attempts timed out or exposed fixture-only TLS-close and cleared-PATH bugs.
Final successful tests supersede those failed candidates. Preserved failure logs:

- `test-appimage-timeout.log`
- `test-appimage-tls-fixture-failure.log`
- `test-appimage-fixture-path-failure.log`
- `test-arch-package-timeout.log`
- `arch-package-timeout.log`

- **Previous AppImage/checksum/manifest:** `artifacts/releases/0.3.0/appimage/`.
- **Older Arch packages:** retained untouched.
- **Record timing:** maintained after builds; records may not be embedded in them.

## 0.3.0 — Discord

- **Result:** both formats passed native/synthetic and package validation before
  Last.fm and later branding changes. These packages do **not** contain Last.fm.
- **Artifacts:** AppImage about 100.2 MiB (x86-64/glibc 2.36); Arch 0.3.0-1 about 4.4 MiB.
- **Coverage:** indexing, dummy playback/PTY/settings, and Discord setup/fake IPC.
- **Limitations:** private SDK / `--nodeps` for Arch; installation/dependency resolution
  and AppImage FUSE mounting untested.

- **Previous artifacts:** 0.2.0 AppImage/checksum/manifest preserved and checksum-verified
  under `artifacts/releases/0.2.0/appimage/`.
- **Log retention:** generic test logs are overwritten by later runs; they are not
  immutable per-release evidence.

## Historical performance evidence

Isolated two-second Discord worker probe around idle-wakeup changes:

| Measurement | Before | After |
|---|---|---|
| Voluntary context switches | 79 | 1 |
| CPU time | 4.193 ms | 0.025 ms |

- **Logs:** `artifacts/discord-idle-before.log`, `artifacts/discord-idle-after.log`.
- **Scope:** worker-harness measurements, not whole-player CPU/memory guarantees.
  Whole-player profiling remains deferred.
