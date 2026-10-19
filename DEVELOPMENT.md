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
to `osu! player` rather than repeating combined artist/song metadata.
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
