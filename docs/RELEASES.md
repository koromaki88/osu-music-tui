# Local release validation

[Changelog](../CHANGELOG.md) · [Packaging](PACKAGING.md) · [Release checklist](../DEVELOPMENT.md#release-checklist)

These are local unsigned packages, not publication approval. All package validation
used synthetic libraries/audio and fake services: no game launch, real-library scan,
installation, publication, commit, or tag. Public distribution remains blocked on
application-license selection and dependency/source-compliance review.

## 0.4.0 — Last.fm

| Format | Artifact | Size | Result |
|---|---|---|---|
| AppImage | `artifacts/dist/osu-player-x86_64.AppImage` | 101.2 MiB | Passed `make test-appimage` |
| Arch 0.4.0-1 | `artifacts/arch/osu-music-player-0.4.0-1-x86_64.pkg.tar.zst` | 4.4 MiB | Passed `make test-arch-package` with private SDK / `--nodeps` |

Native `make test` and synthetic `make test-indexer` passed. Both formats passed
packaged indexing, dummy playback/PTY/settings, Discord fake IPC, Last.fm setup, and
isolated real-libcurl HTTPS authorization/Now Playing/scrobbling/restart, including
certificate/hostname rejection. AppImage additionally passed relocation, no installed
.NET/tools on PATH, WAV/MP3/Ogg playback, and minimal-container CA fallback selection;
its x86-64/glibc floor is 2.36. Final checksums were independently verified.

Limitations: Arch installation/dependency resolution was not tested (`--nodeps` was
needed because pacman-managed .NET packages were absent). AppImage FUSE mounting
was not tested; extraction/run was validated. CMake/Ninja remain unverified.
The user confirmed native live Discord rendering and Last.fm authorization, Now
Playing, and scrobble submission; automated tests never contacted those services.

Successful logs under `artifacts/`: `tests.log`, `test-indexer.log`, `test-appimage.log`,
`arch-package.log`, `test-arch-package.log`. Focused Last.fm ASan/UBSan suites passed:
`lastfm-sanitizer.log` and `lastfm-scrobble-sanitizer.log`.

Earlier attempts timed out or exposed fixture-only TLS-close and cleared-PATH bugs.
Preserved logs: `test-appimage-timeout.log`, `test-appimage-tls-fixture-failure.log`,
`test-appimage-fixture-path-failure.log`, `test-arch-package-timeout.log`, and
`arch-package-timeout.log`. Final successful tests supersede those failed candidates.

Previous AppImage/checksum/manifest are retained under
`artifacts/releases/0.3.0/appimage/`; older versioned Arch packages remain untouched.
Validation records are maintained after builds and may not be embedded in them.

## 0.3.0 — Discord

Both formats passed native/synthetic and package validation before Last.fm and later
branding changes. AppImage: about 100.2 MiB, x86-64/glibc 2.36; Arch 0.3.0-1: about
4.4 MiB. Coverage included indexing, dummy playback/PTY/settings, and Discord setup/fake
IPC. Arch used the private SDK / `--nodeps`; installation/dependency resolution and
AppImage FUSE mounting were untested. These packages do not contain Last.fm.

The prior 0.2.0 AppImage/checksum/manifest were preserved and checksum-verified under
`artifacts/releases/0.2.0/appimage/`. Generic test logs are overwritten by later runs;
they are not immutable per-release evidence.

## Historical performance evidence

An isolated two-second Discord worker probe measured 79 voluntary context switches /
4.193 ms CPU before idle-wakeup changes versus 1 / 0.025 ms after. Logs:
`artifacts/discord-idle-before.log` and `artifacts/discord-idle-after.log`.
These are worker-harness measurements, not whole-player CPU/memory guarantees;
whole-player profiling remains deferred.
