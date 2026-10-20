# Local release validation

[Changelog](../CHANGELOG.md) · [Packaging](PACKAGING.md) · [Release checklist](../DEVELOPMENT.md#release-checklist)

> [!IMPORTANT]
> These are local unsigned packages, not publication approval. Public distribution
> remains blocked on application-license selection and dependency/source-compliance review.

All package validation used synthetic libraries/audio and fake services only.
It did not launch the game, scan a real library, install or publish packages,
or create commits/tags.

## 0.4.1 — UI, controls, and loading efficiency

Both formats were rebuilt and validated with synthetic fixtures. Native `make test`
and `make test-indexer` passed before packaging, then passed again in package builds.

| Format | Artifact | Size | Result |
|---|---|---|---|
| AppImage | `artifacts/dist/osu-player-x86_64.AppImage` | 101.3 MiB | Passed `make test-appimage` |
| Arch 0.4.1-1 | `artifacts/arch/osu-music-player-0.4.1-1-x86_64.pkg.tar.zst` | 4.4 MiB | Passed `make test-arch-package` with private SDK / `--nodeps` |

Coverage includes synthetic indexing (optional BPM/import date and older schemas),
dummy playback, responsive help/details and duration probing, seek controls and
shuffle-history navigation, settings, Discord fake IPC, and isolated Last.fm HTTPS
authorization/Now Playing/scrobbling/restart. AppImage additionally passed relocation,
no installed .NET/tools on PATH, WAV/MP3/Ogg playback, and minimal-container indexing
and CA fallback selection. Its x86-64/glibc floor remains 2.36.

- **Integrity:** both adjacent SHA-256 files were independently verified; hashes and
  sizes are recorded in `artifacts/releases/0.4.1/verified-artifacts.json`.
- **Logs:** `artifacts/releases/0.4.1/logs/` retains native, synthetic indexer,
  AppImage, Arch build, and extracted-Arch validation output.
- **Changes:** `artifacts/releases/0.4.1/release-notes.md`; copyable temporary copy at
  `/tmp/opencode/osu-player-0.4.1-changes.md`.
- **Previous artifacts:** 0.4.0 AppImage/checksum/manifest and package logs preserved
  under `artifacts/releases/0.4.0/`; older versioned Arch packages retained.

Arch installation/dependency resolution remains untested (`--nodeps` was used).
AppImage FUSE mounting and CMake/Ninja remain unverified. No game launch, real-library
scan, installation, or publication occurred. This record was added after packaging
and is not embedded in the already-built packages.

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

Previous AppImage/checksum/manifest are retained under
`artifacts/releases/0.3.0/appimage/`; older Arch packages remain untouched.
Validation records are maintained after builds and may not be embedded in them.

## 0.3.0 — Discord

Both formats passed native/synthetic and package validation before Last.fm and later
branding changes. These packages do **not** contain Last.fm. AppImage was about
100.2 MiB (x86-64/glibc 2.36); Arch 0.3.0-1 was about 4.4 MiB.

Coverage included indexing, dummy playback/PTY/settings, and Discord setup/fake IPC.
Arch used the private SDK / `--nodeps`; installation/dependency resolution and
AppImage FUSE mounting were untested.

The 0.2.0 AppImage/checksum/manifest were preserved and checksum-verified under
`artifacts/releases/0.2.0/appimage/`. Generic test logs are overwritten by later runs;
they are not immutable per-release evidence.

## Historical performance evidence

Isolated two-second Discord worker probe around idle-wakeup changes:

| Measurement | Before | After |
|---|---|---|
| Voluntary context switches | 79 | 1 |
| CPU time | 4.193 ms | 0.025 ms |

Logs: `artifacts/discord-idle-before.log` and `artifacts/discord-idle-after.log`.
These are worker-harness measurements, not whole-player CPU/memory guarantees;
whole-player profiling remains deferred.
