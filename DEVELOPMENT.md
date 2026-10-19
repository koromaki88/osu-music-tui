# Development workflow

The [README](README.md) covers usage, setup, and basic build commands. This document
covers the ongoing development and release workflow.

## Routine development

1. Implement a focused change and add relevant tests.
2. Run `make test`, plus relevant synthetic indexer tests (`make test-indexer`) when
   indexer behavior changes. Test targets display output, save logs under `artifacts/`,
   and preserve failure status.
3. Update relevant documentation and report validation results. State whether packages
   were rebuilt; old artifacts may not contain the latest source changes.

Do not launch osu! or scan the real library for routine validation. Use synthetic
fixtures. Ordinary features, fixes, refactors, and documentation changes do not require
an AppImage or Arch package build.

## When to package

**Default: develop and test locally; do not package after every change.**

| Situation | Standard action |
|---|---|
| Routine feature, bug fix, refactor, or documentation change | Native/relevant synthetic tests only |
| Several completed features / milestone | Summarize changes and propose a release; wait for approval |
| Explicit package request or approved release milestone | Build and validate the requested format(s) |
| Packaging, runtime/dependency/ABI, indexer discovery, install-layout, or portability change | Recommend targeted package validation; obtain approval unless already requested |
| Urgent fix needed by packaged users | Propose an expedited package; build when approved |

## Release candidate checklist

1. Run native tests and relevant synthetic indexer tests. Do not launch osu! or scan
   the real library.
2. Review the version/package revision and summarize changes since the previous
   package. An Arch packaging-only rebuild needs an appropriate package revision.
3. Run `make test-appimage` and/or `make test-arch-package` **only for the requested or
   affected format**. Each target builds and validates its package; a separate build
   beforehand is unnecessary. Retain logs and checksums/manifests where generated.
4. Report output paths and validation results. Never call a failed or unvalidated
   candidate a verified release.

Packaging does not authorize installation, publication, commits, tags, pushes, removal
of old artifacts, or cleanup of unrelated Docker data. Those require separate approval.
Public distribution remains blocked on application-license selection and dependency/
source-compliance review; see [packaging/THIRD_PARTY.md](packaging/THIRD_PARTY.md).

### Local 0.3.0 release validation

Both formats were rebuilt and validated for the Discord milestone:

- `make test` and `make test-indexer` passed with synthetic fixtures.
- `make test-appimage` passed: version 0.3.0, x86-64/glibc 2.36 baseline,
  relocation/extract-and-run, no installed .NET, minimal-container smoke test,
  synthetic indexing, WAV/MP3/Ogg dummy playback, PTY/settings, and packaged Discord
  setup/persistence/fake IPC. Output: `artifacts/dist/osu-player-x86_64.AppImage`
  (about 100.2 MiB), with adjacent checksum and manifest; SHA-256 verified.
- `DOTNET_ROOT="$PWD/.tools/dotnet10" make test-arch-package ARCH_PACKAGE_FLAGS=--nodeps`
  passed: build-time native/synthetic tests plus extracted-package metadata/ABI,
  packaged indexer, default path, dummy playback/PTY/settings, and Discord setup/fake
  IPC. Output: `artifacts/arch/osu-music-player-0.3.0-1-x86_64.pkg.tar.zst`
  (about 4.4 MiB), with verified adjacent SHA-256. Host pacman .NET packages were
  absent: dependency checks were skipped and installation/dependency resolution
  remain untested.
- Logs: `artifacts/tests.log`, `artifacts/test-indexer.log`,
  `artifacts/test-appimage.log`, `artifacts/arch-package.log`, and
  `artifacts/test-arch-package.log`.
- The previous AppImage/checksum/manifest were preserved and checksum-verified under
  `artifacts/releases/0.2.0/appimage/`; older versioned Arch artifacts were retained.

No game launch, real-library scan, installation, publication, commit, or tag was part
of this validation. FUSE mounting remains untested; extraction/run was validated.
Live Discord display was reported working by the user; packaged regression tests
use isolated fake sockets only. Last.fm is not included. These are local unsigned
packages, not public-distribution approval. This validation record was added after
packaging and is not embedded in the already-built packages.

### Local 0.4.0 release validation

- Native `make test` and synthetic `make test-indexer` passed, including the
  isolated real-libcurl HTTPS fixture (no Last.fm contact).
- `DOTNET_ROOT="$PWD/.tools/dotnet10" make test-arch-package ARCH_PACKAGE_FLAGS=--nodeps`
  passed: build-time native/synthetic tests, extracted-package version/dependency/ABI
  metadata, packaged indexer, dummy playback/PTY/settings, Discord fake IPC, Last.fm
  setup, and isolated HTTPS authorization/Now Playing/scrobbling/restart/TLS rejection.
  Output: `artifacts/arch/osu-music-player-0.4.0-1-x86_64.pkg.tar.zst` (about 4.4 MiB),
  with adjacent verified SHA-256. Dependency checks were skipped because the host
  lacks pacman-managed .NET packages; installation/dependency resolution remain untested.
- `make test-appimage` passed: 0.4.0 x86-64/glibc 2.36 baseline, relocation and
  extract-and-run, synthetic indexing, no installed .NET/tools on PATH, WAV/MP3/Ogg
  dummy playback, PTY/settings, Discord fake IPC, Last.fm setup and isolated HTTPS
  authorization/Now Playing/scrobbling/restart/certificate/hostname rejection.
  The minimal Debian smoke test also passed, including bundled CA fallback selection.
  Output: `artifacts/dist/osu-player-x86_64.AppImage` (about 101.2 MiB), with adjacent
  checksum and manifest. Both final package checksums were independently rechecked.
  FUSE mounting remains untested; extraction/run was validated.
- The first attempts timed out; later attempts exposed fixture-only clean TLS-close
  handling and cleared-PATH fixture-generator bugs, now corrected. Failure logs
  are retained as `artifacts/test-appimage-timeout.log`,
  `artifacts/test-appimage-tls-fixture-failure.log`,
  `artifacts/test-appimage-fixture-path-failure.log`,
  `artifacts/test-arch-package-timeout.log`, and `artifacts/arch-package-timeout.log`.
- Previous AppImage artifacts are preserved under `artifacts/releases/0.3.0/appimage/`;
  older versioned Arch packages remain untouched.
- Successful logs: `artifacts/tests.log`, `artifacts/test-indexer.log`,
  `artifacts/test-appimage.log`, `artifacts/arch-package.log`, and
  `artifacts/test-arch-package.log`.

No game launch, real-library scan, installation, publication, commit, or tag is part
of this validation. Public distribution remains blocked on licensing/compliance.
This validation record is maintained after builds and may not be embedded in them.

## Last.fm development implementation

The native source build now includes optional personal API credentials, browser
authorization, private atomic credential persistence, Now Playing, and scrobbling. `l` opens
setup, `d` in that panel disables, and `x` removes local session authorization.
No project credentials are bundled. The credential file is separate from preferences;
it is plaintext with owner-only permissions, not encrypted or keyring-backed.

An owned sleeping worker handles disk and HTTPS work without holding the UI mailbox
mutex during I/O. Requests use a fixed HTTPS endpoint, certificate/hostname verification,
no redirects, bounded response size, connect/total deadlines, and cancellation on
disable, replacement, playback changes, and shutdown. Generic status messages omit
server diagnostics and secrets. Last.fm and Discord receive the same drained event
batch. Now Playing uses its latest-state snapshot; scrobbling consumes explicit
`listening_eligible` events and never reconstructs omitted sessions from snapshots.

Local/transport failures use typed, fixed diagnostic categories and numeric errno,
curl, or HTTP codes. Arbitrary exception text remains redacted. Regression tests
cover credential modes, malformed responses, HTTP 503, and simulated curl DNS,
timeout, and certificate failures separately from Last.fm API errors.
The endpoint now explicitly includes `?format=json`; credentials/signatures remain
in the POST body. Server-issued tokens/session keys use a separate bounded
URL-safe validator rather than API-key validation. Synthetic tests cover XML,
non-object JSON, missing/wrong-type/unsafe tokens, and nonhex opaque tokens/session
storage. Response diagnostics identify the failing stage without showing values.
After those changes the user confirmed live authorization and Now Playing work.

`make test` passes, including deterministic signature/form/metadata tests, private
file safety tests, fake authorization/session restoration/revocation, ignored/failed
Now Playing, cancellation, and TUI masking/cancel/resize/disable/restart tests. Tests
do not contact Last.fm, launch osu!, or scan a real library. The user has confirmed
live authorization, Now Playing, and scrobble submission working in the native build.
The focused Last.fm ASan/UBSan suite also passes; its log is
`artifacts/lastfm-sanitizer.log`. The complete native-suite log is `artifacts/tests.log`.

`tests/lastfm_integration.py` also exercises real libcurl HTTPS through an isolated
loopback CONNECT proxy serving only synthetic Last.fm responses. Ephemeral fixture
certificates check trusted requests, untrusted roots, and hostname mismatch without
contacting Last.fm or using personal credentials. It verifies signed authorization,
Now Playing, threshold submission, durable receipts, and restart without autoplay
or duplicate submission. Native and extracted-package tests run the same fixture.
`SSL_CERT_FILE` explicitly selects the trust bundle. AppImage launch prefers current
host roots and otherwise supplies its hashed bundled CA fallback; the minimal-image
smoke test checks fallback selection. Verification is never disabled.

### Durable scrobbling

`LastfmQueue` owns an exclusive nonblocking `flock`, a bounded private JSON outbox,
and recent completed-session receipts. Each valid event is bound to the authorized
account at ingestion. The worker persists it atomically with file/directory fsync
**before** submitting; no disk or HTTPS work runs under the UI mailbox mutex.
Only explicit timing-eligible events with valid structured metadata, known duration
over 30 seconds, sufficient credited listening, and a valid original start timestamp
are admitted. Events received while disabled/unauthorized are not cached for later.

`scrobbles.json` lives beside credentials in the private Last.fm config directory.
It stores at most 1,024 pending plays, 4,096 bounded account/session receipts, and
persisted retry deadlines/failure counts, capped at 8 MiB. It excludes credentials,
paths, audio hashes, library IDs, and invented album/chosen-by-user metadata. It does
contain plaintext listening history and local playback-session identifiers.
Unsafe, unsupported, or malformed files block delivery without being overwritten;
full queues retain existing entries and count unsaved new plays. A second player
may use Now Playing but cannot capture/drain scrobbles while the outbox is locked.

Pending plays drain FIFO per normalized account, one at a time, even after playback
stops. Switching accounts never reassigns old plays. Disabling/disconnecting cancels
network work but retains the outbox; normal shutdown flushes accepted mailbox
events before joining. HTTP requests are spaced by at least 200 ms. Transient API
11/16/29, transport failures, and uncertain/malformed responses retain the entry,
with exponential retry delay from 30 seconds to 30 minutes. Other permanent API
rejections and valid ignored responses become terminal receipts, not retries.
API 9 preserves the entry and requires reauthorization; credential/permission errors
pause delivery pending repair. JSON acknowledgements must have consistent accepted/
ignored counts, ignored code, and the original timestamp; string/numeric attributes
and a single object or one-item array are supported. Returned metadata corrections
are never reused.

Receipts suppress normal duplicate events/restarts but cannot guarantee exactly-once
delivery after a lost server reply or a crash between acceptance and receipt fsync.
Retries preserve the original timestamp/metadata. A crash before mailbox persistence
can lose recent eligibility events; no lost journal history is invented to compensate.
No automatic expiry silently deletes queued plays; server filtering is reported.

Private-file hardening uses a shared internal `lastfm_storage::Directory`: open the
owner-only directory once with `O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC`, then use `openat`,
`renameat`, and `unlinkat` relative to that descriptor. Atomic writes use random
exclusive 0600 staging files, file fsync, and directory fsync, and verify the named
directory still matches the held inode before commit. Private files and lock files
must have one link, preventing hard-linked aliases; all owned descriptors are
close-on-exec. This limits path-replacement attacks, not a malicious process with
the same UID or root access. Credentials/history remain plaintext, not encrypted.

Persisted retry deadlines are converted once per head/deadline to monotonic waits,
clamped to at most 30 minutes on load; clock changes do not continually rebase them.
The scheduler only copies a payload when selecting a submission, not on each wake.
Outbox-load duplicate detection uses a bounded identity set instead of repeated
linear scans and rejects duplicate receipts too. Invalid/oversized/non-integer API
error codes and noncanonical ignored codes cannot become terminal receipts or revoke
credentials. Tests exercise replaced directories/targets, hard links, descriptor
flags, hostile error types/ranges, and extreme retry-clock arithmetic. No whole-player
CPU profiling or claimed CPU/memory improvement is part of this hardening pass.

`tests/lastfm_scrobble_test.cpp` covers timing/metadata gating, timestamp preservation,
queue restart/FIFO/account isolation, exclusive locks, limits, malformed/newer files,
symlinks/FIFOs/modes, private content, disk-write failure, accepted/ignored/rejected
responses, transient and rate-limit retries, malformed ACKs/timeouts, revocation,
duplicate receipts, disable cancellation, and final-event persistence at shutdown.
Controller-to-fake-service coverage now drives `PlayerController` with an owned
fake audio backend and fake playback clock through pause, seeks, resume, threshold,
stop/replay, and shutdown-settled eligibility. No eligibility flag is fabricated in
that path. Concurrency regressions verify that unrelated playback revisions cannot
discard scrobble errors (especially revoked sessions), and that disconnect is an
ordered local action that survives immediate quit or re-enable. Disconnect requests
are processed before a later load and are flushed at normal worker shutdown.
It runs in `make test` and is registered in CMake. The native suite and focused
scrobble ASan/UBSan suite pass; logs are `artifacts/tests.log` and
`artifacts/lastfm-scrobble-sanitizer.log`. CMake/Ninja remain unavailable here, so
the updated CMake registration is unverified. No package builds were run.

New native dependencies are libcurl >=7.85, OpenSSL libcrypto, and CA certificates.
Build declarations were updated, but AppImage/Arch packages were **not rebuilt**;
their existing 0.3.0 artifacts do not contain Last.fm. Targeted validation of both
formats is recommended before release, especially TLS CA discovery in the AppImage.
Obtain packaging approval first. CMake validation remains pending if tools are absent.

Protocol references: <https://www.last.fm/api/desktopauth>,
<https://www.last.fm/api/scrobbling>, and
<https://www.last.fm/api/show/track.updateNowPlaying>.

## Test commands and logs

| Target | Scope | Saved log |
|---|---|---|
| `make test` | Native unit, Python safety, dummy-audio CLI, TUI PTY, and refresh tests | `artifacts/tests.log` |
| `make test-indexer` | Synthetic Realm fixtures | `artifacts/test-indexer.log` |
| `make test-integration` | **Opt-in real-library verification** | `artifacts/test-integration.log` |
| `make test-appimage` | Build and validate the AppImage | `artifacts/test-appimage.log` |
| `make arch-package` | Build the native Arch package and run its build-time tests | `artifacts/arch-package.log` |
| `make test-arch-package` | Build and validate the extracted Arch package | `artifacts/test-arch-package.log` |

Targets display stdout/stderr, including build failures, and replace their previous log.
Bash `pipefail` and `tee` preserve failure status. Override the log directory with
`make test TEST_LOG_DIR=/path/to/logs`. Logs and generated artifacts are gitignored.
`make test` includes fast packaging safety checks but does not build packages.

### Synthetic coverage

- Audio tests generate short WAV, MP3, and Ogg files and use SDL's dummy output; no
  audible playback or game launch is needed.
- Controller tests use `tests/fake_audio_player.hpp` without audio hardware, FFmpeg,
  Realm, terminal, or real audio files. Coverage includes a 10,001-entry all-broken queue,
  seeded shuffle, repeat, load/play/decode failures, paused seek recovery, fatal device
  errors, and large-library replacement. CLI tests separately verify backend integration
  and automatic EOF advancement.
- `tests/tui_model_test.cpp` covers filtering/selection with 10,001 entries, viewport
  bounds, UTF-8 clipping, and control-character sanitization without ncurses.
- `tests/tui_integration.py` uses PTYs, generated audio, and isolated config directories
  for key input, Unicode editing, resize, empty/monochrome screens, device errors,
  settings persistence, and terminal cleanup on quit/signals.
- `tests/refresh_integration.py` uses a disposable fake adapter and PTY for refresh during
  playing/paused audio, retained filters/selections, cache isolation, stale/corrupt caches,
  malformed output, source changes, missing databases, cache-write errors, timeout,
  cancellation, process reaping, snapshot cleanup, and export reloads.
- Python safety tests cover quick/full verification helpers and incremental validation
  of a 10,001-record export without real game data.

### Sanitizers and CMake

Run `make clean` before switching between regular and sanitizer Make builds:

```bash
make clean
make SANITIZE=1
make SANITIZE=1 test
# Restore regular binaries afterward:
make clean
make
```

The alternative CMake configuration is below. It remains unverified in this environment
because CMake/Ninja were unavailable during implementation. Unlike Make's test targets,
direct CTest invocation does not automatically save a log.

```bash
cmake -S . -B build -G Ninja -DOSU_PLAYER_ENABLE_SANITIZERS=ON
cmake --build build
bash -o pipefail -c 'ctest --test-dir build --output-on-failure 2>&1 | tee build/ctest.log'
```

### Opt-in real-library verification

This is not routine validation: obtain explicit authorization, and **close lazer first**.
The project never launches the game. Build the indexer separately before piping NDJSON;
build logs must not be mixed with adapter stdout.

```bash
make indexer
make test-integration
# Relocated data directory:
make test-integration OSU_DIR=/path/to/osu-data
# Slower storage / larger databases:
make test-integration INTEGRATION_TIMEOUT=900
# Expensive: hash every source file before and after indexing:
make test-integration INTEGRATION_VERIFY=full INTEGRATION_TIMEOUT=900
```

Quick mode checks Realm/config control files and root directory entries, plus existence
of exported audio paths. It does not walk/hash the songs tree and is not proof every
audio file stayed unchanged. Full mode reads the entire library twice and can be costly.
The default 300-second timeout applies to each subprocess, not hashing or the whole test.
The test prints phase progress and at most five song titles; NDJSON uses disk-backed
temporary streams and incremental processing instead of multiple in-memory copies.

For an explicitly requested real-library inspection with the private SDK:

```bash
set -o pipefail
.tools/dotnet10/dotnet tools/osu-indexer/bin/Release/net10.0/osu-indexer.dll \
  --osu-dir "$HOME/.local/share/osu" | ./bin/osu-player library -
```

With a system SDK, replace `.tools/dotnet10/dotnet` with `dotnet`. Pass a search argument
after `-` to filter output.

## AppImage builds

After packaging approval, use Make, Python 3.11+, and a running local Docker engine:

```bash
make test-appimage
# Or use Podman:
make test-appimage CONTAINER_ENGINE=podman
```

No host native compiler or .NET installation is required. `make appimage` also builds
with the container-side checks; `make test-appimage` is the logged validation entry point.
The first build downloads the pinned Debian base, checksum-verified .NET SDK, distro
dependencies, NuGet packages, and checksum-verified appimagetool 1.9.1.

Only the Dockerfile and SDK installer enter the toolchain image build context. Sources
arrive as an explicitly allowlisted read-only snapshot, excluding the game, real library,
SDKs, caches, and local artifacts. Compilation, tests, NuGet restores, and AppImage
extraction run in disposable containers, not cached source-dependent image layers.
A second minimal Debian container verifies indexing from a synthetic Realm with no
installed .NET or application libraries. Tests also verify relocation and generated-audio
PTY playback with the packaged indexer and no `dotnet` on PATH.

Outputs:

- `artifacts/dist/osu-player-x86_64.AppImage`
- `artifacts/dist/osu-player-x86_64.AppImage.sha256`
- `artifacts/dist/osu-player-x86_64.AppImage.manifest.json`

Validated outputs replace the previous files only after tests, checksum verification,
and the x86-64/glibc 2.36 baseline check pass. Dependency notices, available license
texts, and a manifest are embedded. Review FFmpeg dependency/source obligations before
redistribution. Runtime 10.0.13, SDK 10.0.402, and base/tool digests are pinned; update
deliberately for security releases. Optional .NET LTTng tracing is omitted.

`make appimage-local` requires host development dependencies and .NET and may raise the
glibc requirement to that of the host; it is not the portable release path. `make release`
builds only an optimized C++ executable at `artifacts/release/osu-player`, separate from
regular/sanitized objects. That executable still needs native libraries and an indexer.

### Container storage and cleanup

The workflow retains one reusable image, `osu-player-appimage-toolchain:net10-debian12`,
plus the small Debian base. Source-only edits do not accumulate compilation/test/package
layers. Disposable containers remove temporary sources, NuGet caches, compiled files,
and extracted images on exit; no persistent Docker volumes are created. This trades
restore/compile time for bounded storage across source-only builds. The toolchain and
temporary working space still need disk space during a build.

Inspect usage with `docker system df` and `docker image ls`. When cleanup is authorized:

```bash
make clean-appimage-toolchain
```

This removes only the project's tagged toolchain image, not release artifacts, unrelated
images/containers/volumes, or global build cache. Docker/BuildKit can retain toolchain
cache after dependency/SDK upgrades and historical layers from older workflows; this
target does not promise to purge them. Review old data separately. Do not blindly use
`docker system prune -a --volumes`.

Local `.tools/` SDKs are separate from Docker storage and remain until explicitly removed.
`make clean` removes normal C++ outputs only. Bind mounts require a local engine able to
access the checkout. Docker runs builds as the host UID/GID; Podman additionally uses
`keep-id`. Outputs are not left root-owned, and containers are removed on success/failure.

## Native Arch builds

Install development dependencies explicitly; the builder never runs sudo or changes
installed packages:

```bash
sudo pacman -Syu --needed base-devel python dotnet-sdk-10.0 ffmpeg json-c sdl2-compat ncurses
# After packaging approval:
make test-arch-package
```

Run as a normal user, not root. No Docker is used. `make arch-package` invokes `makepkg`
to check dependencies, build an optimized player/framework-dependent linux-x64 indexer,
run ordinary and synthetic adapter tests, and create the package. First restore needs
network access and uses committed `packages.arch.lock.json`.

Outputs in `artifacts/arch/` include the `.pkg.tar.zst`, SHA-256 file, source snapshot,
and checksum-filled `PKGBUILD`. The snapshot includes allowlisted working-tree sources,
not only committed files, and excludes game/library/SDK/cache/build outputs. The tracked
`packaging/arch/PKGBUILD` is a template: use the Make targets to prepare the archive and
checksum rather than invoking the template directly.

`make test-arch-package` additionally extracts the package and checks metadata, indexer
discovery, default osu directory, and generated-audio playback/terminal controls. It
neither installs the package nor accesses the real library. Explicit FFmpeg SONAME
dependencies guard against incompatible major versions; rebuild after ABI upgrades.

For controlled developer validation with a private .NET SDK instead of pacman-installed
.NET packages, `ARCH_PACKAGE_FLAGS=--nodeps` skips makepkg dependency checks. This is not
the normal release-environment setup: verify native dependencies separately and set
`DOTNET_ROOT` to the private runtime for extracted-indexer tests. It does not validate
pacman installation or dependency resolution and never changes installed host packages.

## Implementation notes

- C++20 code wraps json-c and other C APIs with RAII. `PlayerController` owns its library
  and injected `AudioPlayer`; its queue stores library indices and has no terminal or
  decoder implementation dependencies. Call `update()` on the same control thread.
  Per-track diagnostics are bounded to the last 32, with an omitted count for larger batches.
- FFmpeg streams into a small SDL2 output queue rather than allocating whole-song PCM.
  Queue refill runs on the control thread and must be pumped regularly.
- The indexer targets .NET 10 LTS and pins Realm .NET 20.1.0 with NuGet locks. Make prefers
  `.tools/dotnet10/dotnet`, then PATH; `DOTNET=/path/to/dotnet` overrides it. The private
  SDK bootstrap verifies SHA-512 and refuses overwrite. Old `.tools/dotnet/` installs
  are untouched and no longer automatically selected. Arch uses the maintained system
  .NET 10 runtime; AppImage bundles its runtime.
- Indexing and cache loading run on an owned worker; ncurses, controller mutation, and
  audio pumping stay on the UI thread. The adapter is spawned directly without a shell,
  using private disk-backed stdout/stderr/snapshots. Source Realm metadata is checked
  before/after indexing, and the complete export is validated before cache replacement.
- Development indexer discovery uses the Release DLL under `tools/osu-indexer/` and the
  private SDK when available, otherwise `dotnet` on PATH. Paths are relative to the
  development executable, not the shell's current directory. Packaged discovery resolves
  the executable at `../lib/osu-player/indexer/osu-indexer` relative to the player.
- Library replacement matches stable ID and audio hash to preserve the loaded stream
  without reload/seek/pause. Surviving shuffled entries retain order; new entries append
  randomly. Replaced/removed current tracks pause and clear selection without autoplay.
  Failed-track suppression resets. Selection is restored by stable ID where still matched.
- Terminal text neutralizes metadata controls/escape sequences and clips by cell width;
  complex emoji grapheme clusters are not fully modeled.
- Cache headers record canonical source path, version, count, Realm identity/size, and
  nanosecond timestamps. Cache files are private (0600); updates fsync a same-directory
  staging file and atomically rename it. Source-library cache/temp paths are rejected.
  Quit/signals/timeouts terminate the adapter process group, reap the direct child, and
  remove its temporary tree. Limits are 512 MiB NDJSON and 1 MiB diagnostics.
- Settings use bounded versioned JSON, private staging, fsync, and atomic replacement;
  invalid/newer files are preserved. Only applied search edits are saved. Preference
  writes stay outside the osu! data directory.

See [PLAN.md](PLAN.md) for architecture, milestones, and historical validation notes.

### NDJSON contract inspection

The input begins with a schema header followed by `track` and optional `warning` records.
Tracks are validated, deduplicated by beatmap set and audio hash, then sorted by display
artist/title. Inspect the synthetic fixture without touching real game data:

```bash
./bin/osu-player library tests/fixtures/library.ndjson
./bin/osu-player library tests/fixtures/library.ndjson triangles
```

## Discord / Last.fm integration foundation

Step 1 is implemented in `playback_events.hpp/.cpp` and `PlayerController`. It performs
no network/socket/disk operations and does not connect to Discord or submit scrobbles.
The separate Discord worker described below now consumes the foundation in the TUI.
Existing 0.2.0 packages do not include these post-release changes or later icon edits.

### Event contract

- `PlayerController::take_playback_events()` drains a value-owned batch in occurrence
  order. The TUI drains once per loop and publishes an owned current snapshot to Discord;
  a future Last.fm consumer must receive the same batch rather than draining it again.
  Neither networking nor consumer callbacks belong inside the controller. Event metadata
  intentionally excludes filesystem paths and filenames.
- Events cover successful start, progress, pause/resume, successful seek, metadata change,
  timing eligibility, and session end with an explicit reason (EOF, skip, replacement,
  stop, decoder/output failure, refresh removal, or frontend shutdown).
- Each successful new play has a random per-controller namespace plus a session counter,
  independent of shuffle's seeded RNG and wall time. Repeat-one, repeat-all wrapping,
  manual restart, and play after stop get new IDs. Pause/resume, seeking, preference
  changes, and refresh retaining stable ID + audio hash preserve the session.
- Failed loads/plays and loaded-but-paused recovery replacements are not announced as
  listens. A recovered paused selection gets a session only when playback actually starts.
- `playback_snapshot()` exposes the current session on the control thread; copy it before
  crossing threads or retaining it across mutations. Ended-event snapshots retain final
  position, duration, metadata, listening time, original wall start timestamp, and session
  ID even after the next load resets the audio backend.
- Adjacent progress events coalesce. The journal retains at most 128 events and reports
  the dropped count via `omitted`. A consumer must resynchronize from a current snapshot
  after overflow; it must not infer missing listens or promise lossless scrobbling from
  a dropped history. This journal is not the future durable submission queue.
- Frontends call `shutdown()` on normal quit (including handled TUI signals) to settle
  and end the active session without rewinding. Abrupt process termination cannot emit
  a final event; crash recovery/durable eligible-submission storage belongs to a later step.

### Listening-time rules

`PlaybackClock` injects steady and wall clocks for deterministic tests. Wall time records
the original playback start; only steady elapsed time and positive backend-consumed media
progress contribute to listening. Clock time with no media movement earns nothing.
Elapsed allowance spans bursty SDL device reports and irregular polling, preserving
submillisecond fractions; credit never exceeds observed forward media movement or
available playing time. Excess media jumps cannot credit future silence.

Pause/resume and successful seeks rebase accounting; paused time and the seek displacement
do not count. Before seek/stop/replacement/EOF the old session is settled before position
is reset. This is a conservative playback estimate, not proof a human heard the output;
playback speed changes are not supported. Failed seeks do not count uncertain displacement.

The monitor announces timing eligibility once per session when known audio duration is
strictly greater than 30 seconds and credited listening reaches half its duration or four
minutes, whichever is earlier. Unknown/invalid duration cannot qualify speculatively; a
duration learned later can qualify prior accumulated listening. Metadata validity,
authorization, API policy, duplicate handling, and delivery remain separate checks.
Reference: [Last.fm scrobbling rules](https://www.last.fm/api/scrobbling).

`tests/playback_events_test.cpp` exercises these rules using a fake backend/clock, including
EOF repeat, error recovery, refresh identity/metadata changes, unknown durations, timing
boundaries, clock corrections, buffered consumption, owned snapshots, and journal overflow.
It runs under `make test` and is registered in CMake as `playback-events`.

Validation: the full native and ASan/UBSan suites pass, including the fake-clock tests
and existing audio/TUI/refresh integration tests. Logs are `artifacts/tests.log` and
`artifacts/sanitizers/tests.log`. Regular development binaries were restored afterward.
CMake registration is updated but remains unverified in this environment.

### Discord presence

`discord_presence.hpp/.cpp` owns a `std::jthread` for Linux Unix-domain IPC. Configuration,
status, and latest snapshot use a short-held mutex; no socket operations run on the
control thread. The mailbox retains one snapshot, not a growing work queue. Presence
uses structured Unicode metadata and optional official beatmap links, not paths/hashes
or internal IDs. Metadata is UTF-8 sanitized and byte-bounded before transmission.

Settings version 1 accepts an optional `discord` object with `enabled` and
`application_id`; older files default off. There is no bundled project application ID.
Users supply their own application ID, optionally with the `osu_player` image asset.
Pressing `d` to enable without a valid ID opens a nonblocking TUI input prompt; an
enabled-but-invalid saved configuration also prompts at startup. Enter validates the
draft before saving/enabling; Esc cancels and leaves Discord off, without replacing
the saved ID. Backspace and Ctrl-U edit the draft. Search and transport keys are not
dispatched while editing; audio pumping and library refresh continue normally.
No OAuth, bot/account token, client secret, HTTP request, or Discord SDK is involved.
Only the TUI currently configures/publishes presence; CLI playback is unchanged.

The worker implements little-endian bounded frames, READY, matching request ACKs,
ping/pong, partial nonblocking I/O, absolute deadlines, same-UID socket/peer checks,
and bounded exponential reconnect backoff. It discovers `discord-ipc-0` through `-9`
under Discord's documented environment roots and `/tmp`, or uses the absolute
`OSU_PLAYER_DISCORD_SOCKET` override. Explicit test paths never discover real clients.
Errors use fixed local text, never untrusted peer messages over ncurses or stderr.

New sessions, metadata/duration changes, pause/resume, seeks, and journal overflow
trigger coalesced updates. Plain progress does not generate RPC traffic; timestamps
advance on Discord. The default five-second throttle uses the latest position when
sending. A Linux `eventfd` wakes the connected worker for state/configuration changes
or shutdown; otherwise it blocks on the IPC socket, with a timer only when a throttled
update is pending. Routine progress updates refresh only position/sample time; owned
metadata is copied only on meaningful state changes and actual sends. Stop clears
immediately; disable/exit attempts a bounded best-effort clear
and closes the socket. An in-flight request is interruptible at short poll intervals;
destruction joins the worker. Abrupt process termination cannot guarantee a clear.

For positive online beatmapset IDs, `assets.large_image` is the fixed HTTPS URL
`https://assets.ppy.sh/beatmaps/{id}/covers/list@2x.jpg`; `small_image` retains the
optional `osu_player` application asset. Missing/nonpositive IDs use `osu_player`
as the large image with no overlay or beatmap link. The cover's `large_text` is fixed
to `osu! music player` rather than repeating combined artist/song metadata.
Activity `name` uses the sanitized Unicode-preferred song title instead of the default
application name. Activity `state` retains the artist (prefixed with `Paused —` when paused). The
separate `details` song-title line is also omitted to avoid redundant text.
Art changes follow the existing coalescing/throttling and metadata-change detection,
including while paused. Only the numeric online ID is
used to build the URL; local paths and arbitrary URLs never enter the payload.
Discord fetches/proxies the image, so this adds no player HTTP client, API credentials,
image cache/decoder, extra thread, or polling. There is no remote-availability probe or
automatic fallback for deleted/unreachable covers; Discord controls display/caching.
References: [Discord activity asset images](https://docs.discord.com/developers/events/gateway-events#activity-object-activity-asset-image)
and osu-web's `Beatmapset::coverPath` / `coverURL` / `coverSizes`.

`tests/discord_presence_test.cpp` checks pure rendering and provides a line-controlled
worker harness. `tests/discord_integration.py` drives isolated fake sockets and a PTY,
covering opt-in/invalid IDs, Unicode/privacy, dynamic thumbnail changes and local-art
fallback, ping/pong, fragmented/malformed/oversized
frames, ACK/READY timeouts, reconnect, pause/seek/stop, normal shutdown, saved toggles,
malformed config preservation, Application ID setup/validation/cancellation/restarts
and uninterrupted playback while editing, and a `/proc`-based idle-worker
wakeup regression check. Both run in `make test`
and are registered in CMake. The full native and ASan/UBSan suites pass; logs are
`artifacts/tests.log` and `artifacts/sanitizers/tests.log`. Regular binaries were rebuilt
and retested afterward. CMake/Ninja remain unavailable, so CMake is unverified.
Live Discord rendering remains unverified; no packages were rebuilt for this step.
Protocol reference: [Discord RPC](https://docs.discord.com/developers/topics/rpc).

The idle-worker optimization's isolated two-second fake-server probe measured 79
voluntary context switches / 4.193 ms CPU before, versus 1 / 0.025 ms after. Logs:
`artifacts/discord-idle-before.log` and `artifacts/discord-idle-after.log`. These are
worker-harness measurements, not whole-player CPU or memory guarantees. Thread and
allocator retained memory may remain even after idle wakeups and copies are removed.

### Next steps (not implemented)

1. Last.fm browser authorization, securely stored session credentials, and Now Playing.
2. Durable account-associated eligible-scrobble queue, HTTPS transport, API response
   handling, bounded retries/backoff, and explicit disconnect/queue controls.

Both integrations should default off. Keep consumer/network failures isolated from
playback and redact credentials from diagnostics. No real service requests or package
rebuilds are part of routine implementation validation.
