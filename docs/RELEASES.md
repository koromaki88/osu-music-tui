# Local release validation

[Changelog](../CHANGELOG.md) · [Packaging](PACKAGING.md) · [Release checklist](../DEVELOPMENT.md#release-checklist)

> [!IMPORTANT]
> These are local unsigned packages, not publication approval. Public distribution
> remains blocked on application-license selection and dependency/source-compliance review.

All package validation used synthetic libraries/audio and fake services only.
It did not launch the game, scan a real library, install or publish packages,
or create commits/tags.

## 0.5.0 — collections, optimization, and certificate-trust refresh

Both formats were rebuilt and validated. The AppImage passed `make test-appimage`;
Arch 0.5.0-1 passed `make test-arch-package` using the private SDK and `--nodeps`.
Native `make -j4 test` and synthetic `make test-indexer` passed before each package
build and again within the respective builds. Arch installation/pacman dependency
resolution remains untested because pacman-managed .NET packages are absent.

| Artifact | Size | Result |
|---|---|---|
| `artifacts/dist/osu-player-x86_64.AppImage` | 101.3 MiB | Passed package validation and independent checksum verification |
| `artifacts/arch/osu-music-player-0.5.0-1-x86_64.pkg.tar.zst` | 4.5 MiB | Passed extracted-package validation and independent checksum verification; private SDK / `--nodeps` |

Coverage includes favorites/playlists persistence and scoped queues, responsive
panels, synthetic Realm indexing, relocation/no installed .NET, dummy WAV/MP3/Ogg
playback, settings, Discord fake IPC, and isolated Last.fm HTTPS. Minimal-container
standalone indexing and CA fallback selection passed. The x86-64/glibc floor remains
2.36, with bundled .NET runtime 10.0.13.

The embedded 148-certificate fallback passed the incident-specific offline trust
gate. All 12 public eMudhra incident test chains were independently rejected for
untrusted issuers using host OpenSSL with only the extracted fallback as trust.
This is separate from packaged-libcurl HTTPS validation using synthetic certificates;
it is not evidence of universal revocation checking or protection from future CA
incidents. The archived 0.4.1 fallback failed the new gate as expected.

- **SHA-256:** `a8e3f06b165b02fe03538d736a3adbea7de118fa7cb0e3e8a0bb0c72aa66aa3e`
- **Arch SHA-256:** `c7f5f93629847f72f00d20793b75b1f372d01c07a3db4648782107049a4570eb`
- **Evidence:** `artifacts/releases/0.5.0/verified-artifacts.json`, `logs/`, and
  the retained public `incident-chains.zip` with its hash recorded in the evidence.
- **Arch evidence:** `artifacts/releases/0.5.0/arch-logs/` retains native/indexer,
  build/extracted-package tests, independent checksum and `.PKGINFO` records. The
  initial 200-second command timeout is retained in separate `*-timeout.log` files;
  the longer retry passed. Extracted tests include favorites/playlists and scoped
  queues in addition to indexer discovery, dummy playback, panels, settings,
  fake Discord and isolated Last.fm HTTPS. Arch uses system CA roots, not the
  AppImage fallback. FFmpeg dependencies match SONAMEs 63/63/61/7 on the build host.
- **Release notes:** `artifacts/releases/0.5.0/release-notes.md`.
- **Previous AppImage:** image/checksum/manifest preserved under
  `artifacts/releases/0.4.1/appimage/`; previous logs and Arch artifacts retained.

FUSE mounting and CMake/Ninja remain unverified. No game launch, real-library scan,
package installation, or publication occurred. This record was added after the build
and is not embedded in the candidate. Public distribution remains blocked on licensing
and dependency/source-compliance review.

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
