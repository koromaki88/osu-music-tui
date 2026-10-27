# Local release validation

[Changelog](../CHANGELOG.md) · [Packaging](PACKAGING.md) · [Release checklist](../DEVELOPMENT.md#release-checklist)

> [!IMPORTANT]
> These are local unsigned packages, not publication approval. Public distribution
> remains blocked on application-license selection and dependency/source-compliance review.

All package validation used synthetic libraries/audio and fake services only.
It did not launch the game, scan a real library, install or publish packages,
or create commits/tags.

## 0.6.1 — independent mute and shared release version

Both formats were rebuilt and passed package validation. Native `make -j4 test`
and synthetic `make test-indexer` passed first and within the builds. Extracted UI
tests verify the `v0.6.1` header, mute indicator, adjustments while muted, panel
controls and preservation of saved volume without restoring mute on restart.
Controller tests cover backend silence, session/queue preservation, track changes,
refresh, paused unmute and a selected volume of zero.

| Artifact | Size | Result |
|---|---|---|
| `artifacts/dist/osu-player-x86_64.AppImage` | 101.3 MiB | Passed `make test-appimage` |
| `artifacts/arch/osu-music-player-0.6.1-1-x86_64.pkg.tar.zst` | 4.5 MiB | Passed `make test-arch-package` with private SDK / `--nodeps` |

- **AppImage SHA-256:** `2230ccd6ce7731ba903a5d1ccef13fc9231a1b82b993eac50f068036bc32ae9d`
- **Arch SHA-256:** `8ab836f56c57e074262afbdcd219d9b142cc0c9a7862a5652907b4a750f24230`
- **Evidence:** `artifacts/releases/0.6.1/verified-artifacts.json`, `logs/`, `arch-logs/`.
  Both exported checksums were independently verified; `.PKGINFO` and AppImage
  manifest versions match the shared `VERSION` file.
- **Release notes:** `artifacts/releases/0.6.1/release-notes.md`.
- **Previous artifacts:** 0.6.0 image/checksum/manifest preserved under
  `artifacts/releases/0.6.0/appimage/`, Arch recipe under `0.6.0/arch/`, and older
  versioned Arch packages/source snapshots left untouched.

Existing synthetic coverage also passed for collections, refresh, overlay config,
fake Discord and isolated Last.fm HTTPS. AppImage standalone minimal-container
indexing and CA fallback selection/gate passed. Its baseline remains x86-64/glibc
2.36 with .NET runtime 10.0.13. Profiling harnesses are not bundled in the packages.

Arch installation/pacman dependency resolution, AppImage FUSE mounting and CMake/Ninja
remain unverified. No game launch, real-library scan, installation or publication
occurred. Public distribution remains blocked on licensing/compliance review.
This record was added after building and is not embedded in the candidates.

## 0.6.0 — optional OBS overlay

Both formats were rebuilt and passed package validation. Native `make -j4 test`
and synthetic `make test-indexer` passed before packaging and within the builds.
Extracted-package tests now exercise the actual overlay worker in both formats,
including port conflict/retry, HTTP boundaries/privacy, playback/seek/idle, slow clients,
saved enablement without autoplay, live config/rollback and thumbnail URL suppression.
No real artwork or live integration service was contacted by these tests.

| Artifact | Size | Result |
|---|---|---|
| `artifacts/dist/osu-player-x86_64.AppImage` | 101.3 MiB | Passed `make test-appimage` after test-only notice assertion fix |
| `artifacts/arch/osu-music-player-0.6.0-1-x86_64.pkg.tar.zst` | 4.5 MiB | Passed `make test-arch-package` with private SDK / `--nodeps` |

- **AppImage SHA-256:** `edf1e1837a8027bb6916b3254d288434aa2725432a51def79a94ce660a38cbd0`
- **Arch SHA-256:** `eb82a625a312b91760feba6cdb28141ecbc4351d8ac21c14c7b7497e3b6bb736`
- **Evidence:** `artifacts/releases/0.6.0/verified-artifacts.json`, `logs/`, `arch-logs/`.
  Both exported checksums were independently verified. AppImage baseline remains
  x86-64/glibc 2.36 with .NET runtime 10.0.13; extracted CA fallback gate passed.
- **Initial failure:** `logs/test-appimage-collections-notice-failure.log` is retained.
  A transient collections warning was overwritten by library-load status; the test
  now checks the persistent playlists panel. No production behavior changed. Arch
  passed with the earlier test assertion; the successful AppImage retry includes
  the test-only correction.
- **Preserved:** previous AppImage/checksum/manifest under
  `artifacts/releases/0.5.1/appimage/`, previous Arch recipe under `0.5.1/arch/`,
  and older versioned Arch packages/source snapshots left in place.
- **Release notes:** `artifacts/releases/0.6.0/release-notes.md`; copyable notes at
  `/tmp/opencode/osu-player-0.6.0-release-notes.md`. Requested 0.5.1 notes were written
  first to `/tmp/opencode/osu-player-0.5.1-release-notes.md`.

Actual OBS/CEF rendering/font availability remains a manual check. Arch installation
and pacman dependency resolution, AppImage FUSE mounting and CMake/Ninja remain
unverified. No game launch, real-library scan, installation, publication or commit/tag
occurred. Public distribution remains blocked on licensing/compliance review. This
record was added after building and is not embedded in the packages.

## 0.5.1 — jump to current track

Both formats were rebuilt with uppercase `J` navigation and passed their package
validation. Native `make -j4 test` and synthetic `make test-indexer` passed before
packaging and again within the builds. Extracted-package collections tests cover
jumping within favorites, revealing tracks hidden by search or saved-list edits,
paused-state/queue preservation, and isolation from panels/search entry.

| Artifact | Size | Result |
|---|---|---|
| `artifacts/dist/osu-player-x86_64.AppImage` | 101.3 MiB | Passed `make test-appimage` |
| `artifacts/arch/osu-music-player-0.5.1-1-x86_64.pkg.tar.zst` | 4.5 MiB | Passed `make test-arch-package` with private SDK / `--nodeps` |

Both exported SHA-256 files were independently verified:

- **AppImage:** `6a93c30ac99de2398cfcea5895794c1d24a47cef37f549ede8b7497fbd264fa3`
- **Arch:** `121731649906c6bebb1e65b50ae0f8a6aa0376ae32ac94b6377997b99ba202d1`
- **Evidence:** `artifacts/releases/0.5.1/verified-artifacts.json`, `logs/`, and
  `arch-logs/`; release notes in `artifacts/releases/0.5.1/release-notes.md`.
- **Preserved:** 0.5.0 AppImage/sidecars under `artifacts/releases/0.5.0/appimage/`,
  previous Arch recipe under `artifacts/releases/0.5.0/arch/`, and older versioned
  Arch packages/source archives left in place.

Package coverage also includes synthetic indexing, dummy audio, panels/settings,
fake Discord and isolated Last.fm HTTPS. AppImage relocation, standalone minimal
container indexing, CA fallback selection and extracted fallback trust gate passed;
the baseline remains x86-64/glibc 2.36 with .NET runtime 10.0.13. No new public
incident-chain verification was performed for this patch.

Arch installation/pacman dependency resolution remains untested (private SDK and
`--nodeps`); FUSE mounting and CMake/Ninja remain unverified. No game launch,
real-library scan, installation or publication occurred. These are local unsigned
candidates, still subject to licensing/compliance review. This record was added
after building and is not embedded in the packages.

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
