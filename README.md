# osu!(lazer) TUI Music Player

Early C++20 implementation of the plan in [`PLAN.md`](PLAN.md). The program currently
contains a read-only Realm indexer, a library domain layer, and streaming audio playback
with both line-based controls and an ncurses TUI.

## Current progress

- [x] Validate SHA-256 hashes and resolve lazer content-store paths
- [x] Inspect extensionless audio through an RAII-wrapped C API
- [x] Load a versioned NDJSON library contract
- [x] Validate, deduplicate, sort, and search tracks
- [x] Read `client.realm` through the read-only adapter
- [x] Play audio (pause/resume, stop, seek, volume, EOF handling)
- [x] Playback controller (queue, next/previous, shuffle, repeat, failed-track recovery)
- [x] Ncurses TUI (search, selection, progress, transport controls, resize handling)
- [x] Background indexing/refresh and XDG cache (offline Realm snapshots only)
- [x] Single-file Linux x86-64 AppImage with bundled indexer/.NET runtime

## Standalone application

The distributable is **one executable file**, including the C++ player, self-contained
.NET 10 indexer, Realm, FFmpeg, SDL, ncurses, json-c, and native application dependencies.
End users do not need the source directory, a compiler, an SDK, or a separate .NET install.

After obtaining `osu-player-x86_64.AppImage`, close lazer and run it from a terminal:

```bash
chmod +x osu-player-x86_64.AppImage
./osu-player-x86_64.AppImage tui
```

You can move/rename this file or keep it in `~/.local/bin/`. The embedded indexer is
resolved relative to the executable **inside** the image; it never relies on the build
machine's `.tools/dotnet` or your current directory. Cache files remain in the normal XDG
location. `--indexer` still explicitly overrides the bundled adapter if requested.

The default release build uses a Debian 12 baseline: **Linux x86-64 with glibc 2.36 or
newer**, `/bin/sh`, a UTF-8 locale, and working host audio services/drivers are required.
This is not an Alpine/musl build or a Windows/macOS executable. glibc and the dynamic
loader are deliberately not bundled. Consult the generated manifest for the measured
minimum glibc version of a particular build; older distributions are not supported yet.

Normal mounted execution needs a working FUSE setup and a suitable `fusermount` helper.
If those are unavailable (for example, in some containers), no installation is necessary;
use the built-in fallback:

```bash
./osu-player-x86_64.AppImage --appimage-extract-and-run tui --osu-dir /path/to/osu-data
```

That fallback decompresses to temporary storage each launch, so startup is slower than
normal mounted execution. For repeated use without FUSE, extract once and run the folder:

```bash
./osu-player-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun tui --osu-dir /path/to/osu-data
```

### Build the standalone file

On a development machine with Make, Python 3.11+, and a running Docker engine:

```bash
make appimage
# Or use Podman:
make appimage CONTAINER_ENGINE=podman
```

No native compiler/.NET installation is needed on the host for this container build.
The first build downloads the pinned Debian base image, checksum-verified .NET 10 SDK, distro build dependencies,
NuGet dependencies, and checksum-verified appimagetool 1.9.1. Later builds reuse one
toolchain image; compilation, tests, NuGet downloads, and AppImage extraction run in
disposable containers, **not cached Docker image layers**. The builder never launches
osu! or reads your real library. Only the Dockerfile and SDK installer enter the image
build context. Project sources arrive as an explicitly allowlisted, read-only snapshot,
excluding caches, `.tools/`, the game AppImage, and local artifacts. The final artifact is
smoke-tested in a separate disposable minimal Debian container
with **no installed .NET or application libraries**, using a synthetic Realm fixture.

Outputs:

- `artifacts/dist/osu-player-x86_64.AppImage`
- `artifacts/dist/osu-player-x86_64.AppImage.sha256`
- `artifacts/dist/osu-player-x86_64.AppImage.manifest.json`

Only the AppImage is required to run. The checksum and manifest are useful for verifying
downloads and auditing the included dependencies. Verify with `sha256sum -c` from its
directory. A checksum detects corruption; it is not a publisher signature.

`make appimage-local` builds against the current host instead (requires normal development
dependencies and .NET). That can produce a much newer glibc requirement, so it is **not**
the recommended portable release path. `make release` builds only an optimized C++
executable in `artifacts/release/`, separate from regular/sanitizer objects; that executable
alone still needs native libraries and an external indexer.

`make test-appimage` builds the image, runs the synthetic indexer tests, and verifies
relocation, actual packaged indexing with no `dotnet` on PATH, and generated-audio PTY
playback inside a disposable container on every build. No host
.NET installation is required. Its output is saved to `artifacts/test-appimage.log`. The default `make test`
includes lightweight packaging safety tests but does not download/build an AppImage.

### Build disk usage

Use `make test` for ordinary development; run `make appimage` when you actually need a
new distributable. The AppImage workflow keeps one reusable image named
`osu-player-appimage-toolchain:net10-debian12`, plus the small Debian base image.
Source edits no longer accumulate compilation/test/package layers. Temporary source
copies, NuGet caches, compiled files, and extracted images are removed when the build
container exits; no persistent Docker volumes are created. Outputs replace the previous
three files in `artifacts/dist/`, only after tests and checksum/baseline validation pass.
This trades some extra restore/compile time for bounded storage across source-only builds.
The toolchain and temporary working space still require disk space during a build.

Inspect Docker storage with `docker system df` and `docker image ls`. To remove **only
this project's reusable toolchain image**, use:

```bash
make clean-appimage-toolchain
```

The next AppImage build recreates it. This does not delete release artifacts, unrelated
images/containers/volumes, or globally prune Docker caches. Docker/BuildKit may retain
toolchain-build cache, particularly after SDK/dependency upgrades; this target does not
promise to purge that cache or old layers left by the earlier packaging workflow.
Review existing Docker data separately before any one-time cleanup. Avoid blindly
running `docker system prune -a --volumes`, which can remove other projects' data.

The local SDKs in `.tools/` are separate from Docker storage and remain until explicitly
removed. `make clean` removes normal C++ build outputs only. Docker bind mounts require
a local engine that can access this checkout's filesystem. Podman uses `keep-id` for
the unprivileged build; Docker builds also run as your host UID/GID, so outputs aren't
left owned by root. Successful and failed containers are automatically removed.

**Release caveats:** the current package is a local unsigned build, not a published
download. Dependency notices/available license texts and a manifest are embedded, but
public redistribution still needs a license/source-compliance review (especially the
system FFmpeg build) and an application-license decision; see
[`packaging/THIRD_PARTY.md`](packaging/THIRD_PARTY.md). .NET 10.0.13, SDK 10.0.402, and base/tool digests
are pinned; update them deliberately for security releases. Optional .NET LTTng tracing
is omitted. TUI volume, shuffle, repeat, and applied search preferences persist across restarts.

## Native Arch Linux package

Arch x86-64 users can choose a normal **pacman package** instead of the AppImage.
The current build is approximately **4.4 MiB** (12 MiB installed), excluding system
dependencies. It installs `osu-player` in `/usr/bin` and the adapter in
`/usr/lib/osu-player/indexer`. No FUSE, Docker, wrapper, or development directory
is needed to run it.

Install the generated local package, then close lazer and start the player:

```bash
sudo pacman -U artifacts/arch/osu-music-player-0.1.0-2-x86_64.pkg.tar.zst
osu-player tui
```

Pacman resolves missing runtime dependencies from your configured repositories:
FFmpeg, SDL2 (provided by `sdl2-compat` on current Arch), ncurses, json-c, and
`dotnet-runtime-10.0` (also provided by current Arch's `dotnet-runtime`), plus their
dependencies. Unlike the AppImage, this package **does require the system .NET 10
runtime**, but no SDK is needed by end users.
It does not bundle copies of system libraries. Explicit FFmpeg SONAME dependencies
prevent silently using an incompatible FFmpeg major version.

This is a native **rolling-release Arch** build, not a portable Linux binary.
Use a fully updated system; rebuild after incompatible library/ABI upgrades.
The package is local and unsigned, and is not published to the AUR or an Arch repository.
The same application-license/redistribution review noted above remains necessary before
public distribution. To uninstall: `sudo pacman -R osu-music-player`.

### Build on Arch

Install development dependencies explicitly (the builder never runs sudo or changes
your installed packages):

```bash
sudo pacman -Syu --needed base-devel python dotnet-sdk-10.0 ffmpeg json-c sdl2-compat ncurses
make arch-package
```

Run the build as your normal user, not root. `makepkg` checks package dependencies,
builds an optimized player and framework-dependent linux-x64 indexer, runs the ordinary
and synthetic adapter tests, and creates the package. NuGet restore needs network access
on the first build and uses a separate committed `packages.arch.lock.json`.

Outputs are under `artifacts/arch/`: the `.pkg.tar.zst`, its SHA-256 file, and a
source snapshot plus checksum-filled `PKGBUILD`. The snapshot uses an explicit file
allowlist; it includes working-tree sources, not just committed changes, but excludes
the game, real library, SDK, caches, and compiled outputs. The tracked
`packaging/arch/PKGBUILD` is a template; use `make arch-package` to prepare its source
archive and checksum rather than running that template directly.

`make test-arch-package` also extracts the package into a temporary directory and checks
its metadata, packaged indexer discovery, default osu directory, and generated-audio
playback/terminal controls. It does **not** install the package or access your real library.
Build/test output is displayed and saved to `artifacts/arch-package.log` and
`artifacts/test-arch-package.log`, with failures preserved. Docker is not used for either
Arch target. The AppImage workflow remains available unchanged.

## Current commands

C++ dependencies: GCC/Clang with C++20, Make (or CMake), `pkg-config`, `json-c`, SDL2, `ncursesw`,
and FFmpeg development libraries (`libavformat`, `libavcodec`, `libavutil`,
`libswresample`; FFmpeg 5+). Tests also require Python 3.11+ and the `ffmpeg` executable
with MP3 (`libmp3lame`) and Ogg Vorbis (`libvorbis`) encoders.

Build and run tests with the Make fallback available in this VM:

```bash
make
make test
```

Test commands automatically display output and save both stdout and stderr (including
build failures) to logs, replacing the previous log for that command:

- `make test` → `artifacts/tests.log`
- `make test-indexer` → `artifacts/test-indexer.log`
- `make test-integration` → `artifacts/test-integration.log`
- `make test-appimage` → `artifacts/test-appimage.log`

Failures still return a nonzero exit status. Override the directory with
`make test TEST_LOG_DIR=/path/to/logs`. Logs are local; `artifacts/` is gitignored.
Automatic logging uses Bash and `tee`.

Inspect an extensionless lazer audio file by its SHA-256 hash:

```bash
./bin/osu-player probe 47b895484e7751f3ab429694ff6dbf21e774ab023e4f6c5b481476f04ff22f0f
```

Pass a relocated data directory as the final argument:

```bash
./bin/osu-player probe HASH /path/to/osu-data
```

Play audio directly from the content store:

```bash
./bin/osu-player play 47b895484e7751f3ab429694ff6dbf21e774ab023e4f6c5b481476f04ff22f0f
# Optional final argument: /path/to/osu-data
```

Commands are line-based for now: type a command, then press Enter.

| Command | Action |
|---|---|
| `pause` / `p` | Pause |
| `resume` / `r` | Resume (or restart a stopped selection) |
| `stop` | Pause and rewind |
| `seek 45.5` | Seek to 45.5 seconds |
| `volume 0.5` | Set volume in the range 0–1 |
| `status` | Show position, duration, and volume |
| `quit` / `q` | Exit and release audio resources |

Playback ends automatically at EOF. Decoder/output failures report an error and exit
nonzero. No audio device is needed for `probe` or `library`. Playback uses FFmpeg to
decode/resample into a small SDL2 output queue, not a whole-song PCM allocation. Queue
refill runs on the control thread; it must be pumped regularly. Volume changes affect
newly queued samples (normally within about 120 ms, plus device latency).

## Play a library queue

Use an NDJSON export from the indexer, rather than entering individual audio hashes:

```bash
./bin/osu-player queue artifacts/library.ndjson
```

To create that export on this VM (close lazer first):

```bash
make indexer
mkdir -p artifacts
.tools/dotnet10/dotnet tools/osu-indexer/bin/Release/net10.0/osu-indexer.dll \
  --osu-dir "$HOME/.local/share/osu" > artifacts/library.ndjson.tmp &&
  mv artifacts/library.ndjson.tmp artifacts/library.ndjson
```

Use `dotnet` instead of `.tools/dotnet10/dotnet` with a system SDK. The temporary-file/rename
pattern keeps a failed indexing attempt from replacing a usable export. These are local
metadata files; `artifacts/` is gitignored. `queue` accepts a file, not `-`: stdin is reserved
for interactive controls.

The existing transport commands also support:

| Command | Action |
|---|---|
| `next` / `n` | Start the next queue entry |
| `previous` / `prev` | Start the previous entry (restart at the head) |
| `list [SEARCH]` | Print up to 20 matching tracks with their library indices |
| `track 7` | Play library entry 7 (1-based; indices do not change with shuffle) |
| `shuffle on` / `shuffle off` | Change queue ordering without interrupting the current song |
| `repeat off` / `repeat one` / `repeat all` | Stop at end, repeat current on EOF, or cycle the queue |

Queue behavior:

- EOF advances automatically. The CLI exits when the queue ends with repeat off.
- Repeat-one applies only to automatic EOF, not manual next/previous.
- Manual navigation starts playback, even from a paused/stopped selection.
- Enabling shuffle preserves played history/current song and randomizes the remaining
  suffix. Disabling it restores sorted library order. Repeat-all reshuffles at a forward
  cycle boundary and avoids an immediate duplicate when there are multiple entries.
- Track failures are reported and skipped in the navigation direction. Failed entries
  are not retried automatically during the session, including across repeat-all cycles.
  Explicit selection retries that entry; restarting a failed/completed queue clears the
  failure history. An all-broken queue terminates instead of looping.
- Output-device failures stop immediately rather than attempting the same device for
  every song. Pending per-track diagnostics are bounded to the last 32, with an omitted
  count for larger batches.

`PlayerController` in `include/osu_player/player_controller.hpp` owns its library and an
injected `AudioPlayer`. Its queue contains library indices; it has no terminal or decoder
implementation dependencies. Call `update()` regularly on the same control thread.

## Terminal UI

Use the same exported library file as the queue CLI:

```bash
./bin/osu-player tui artifacts/library.ndjson
```

The UI starts **without autoplay**. It requires interactive stdin/stdout, an installed
`TERM` entry, and preferably a UTF-8 locale (for example `LANG=C.UTF-8`). The minimum usable
size is 50 columns × 14 rows; smaller terminals show a resize message while playback
continues. Monochrome terminals work without colors.

| Key | Action |
|---|---|
| `j` / `k`, Down / Up | Move selection |
| PageDown / PageUp, `g` / `G`, Home / End | Page or jump to the first/last result |
| Enter | Play selection |
| Space | Pause/resume; start selection if nothing is loaded |
| `n` / `p` | Next/previous in the playback queue |
| Left / Right | Seek backward/forward five seconds |
| `+` / `-` | Volume in 5% steps |
| `s` | Toggle shuffle |
| `R` | Cycle repeat off → one → all |
| `x` | Stop and rewind |
| `r` | Refresh in the background (reload the export, or rerun the indexer) |
| `/` | Edit the search filter |
| Esc | Clear the filter outside search mode |
| `q` | Quit outside search mode |

Search updates results as you type. Enter applies the filter (press Enter again to play
the selection); Esc cancels the edit and restores the previous filter. Backspace removes
one UTF-8 codepoint, and Ctrl-U clears the query. Search is ASCII-case-insensitive and can
match Unicode text exactly. Filtering does **not** change the playback queue, interrupt
audio, or hide the now-playing information.

The UI stays open after queue completion or playback errors, so you can choose another
track or retry a device. Errors appear in a status line instead of overwriting the screen.
Metadata controls/escape sequences are neutralized and text is clipped by terminal cell
width; complex emoji grapheme clusters are not fully modeled. Quit, Ctrl-C, SIGTERM, and
SIGHUP restore terminal settings. Mouse support and indexing while lazer is running are
not implemented.

### Saved preferences

The TUI automatically saves volume, shuffle, repeat mode, and the last applied search
filter to `$XDG_CONFIG_HOME/osu-player/settings.json`, or
`$HOME/.config/osu-player/settings.json` when the XDG path is unset, empty, or relative.
Preferences are shared across libraries and builds (development, AppImage, native package).
Playback never starts automatically; the current track, position, and queue are not restored.
Search edits are saved only when applied with Enter; Esc cancels an edit.

Changes use a private, atomically replaced file, not the osu! directory. Missing settings
use defaults (70% volume, shuffle/repeat off, empty search). Invalid or unsupported files
are preserved and defaults are used with an on-screen warning; persistence is disabled
for that session. Save errors are also shown without interrupting playback. Multiple
instances use last-writer-wins rather than merging preferences.

To reset preferences, close all player instances and remove the settings file. No library
cache or osu! content needs to be deleted. Only the TUI saves these preferences; CLI
playback commands keep their existing behavior.

### Background indexing and cache

You can now browse directly from a lazer data directory instead of exporting by hand.
**Close lazer before starting this mode or pressing `r`.** Background indexing means the
player stays responsive; it does **not** make copying a live Realm database safe.

```bash
make indexer
./bin/osu-player tui
# For slow storage / larger libraries:
./bin/osu-player tui --osu-dir /path/to/osu-data --index-timeout 900
# Bypass all cache reads/writes:
./bin/osu-player tui --osu-dir /path/to/osu-data --no-cache
```

Without `--osu-dir`, `tui` uses `$HOME/.local/share/osu`. An explicit `--osu-dir`
overrides this; `tui FILE.ndjson` still opens an exported library instead.

The default adapter command uses the built Release DLL under `tools/osu-indexer/` and
the local `.tools/dotnet10/dotnet` if present, otherwise `dotnet` on PATH. Paths are resolved
relative to the player's development build location, not the shell's working directory.
For another installation, pass `--indexer /absolute/path/to/osu-indexer` (an executable
or wrapper accepting `--osu-dir DIRECTORY`, not a shell command string or a DLL).
No build/restore runs inside the UI. `--index-timeout` accepts 1–86400 seconds, default 300.

Cache files live under `$XDG_CACHE_HOME/osu-music-player/`, or
`$HOME/.cache/osu-music-player/` if XDG_CACHE_HOME is unset/relative. Each canonical osu!
directory gets a separate, private (0600) NDJSON file. Its header records the full source
path, cache version, track count, and Realm identity/size/nanosecond timestamps. Startup
loads a validated cache on the worker; an unchanged Realm needs no adapter process.
A stale cache is shown while indexing runs. A missing database or failed refresh leaves
the last usable library/cache intact. Corrupt/truncated caches trigger reindexing.

Pressing `r` always forces a refresh, even with an unchanged Realm. Only one refresh runs
at a time. For `tui FILE`, `r` simply reloads that export asynchronously, without an adapter
or cache. A valid header-only export intentionally replaces the library with an empty one.
Warnings and failures appear in the status line; cache-write errors do not prevent using
a successfully indexed library.

Refresh preserves the query, visible selection by stable ID when still matched, playback
preferences, and shuffled order of surviving entries (new entries append in random order).
If the current stable ID **and audio hash** survive, the loaded stream is untouched: no
reload, seek, pause, or restart. If removed or replaced, playback pauses and selection
clears without autoplay. Failed-track suppression resets so repaired audio can be retried.

The worker spawns the adapter directly, without a shell, with private disk-backed stdout,
stderr, and temporary snapshots. It checks Realm metadata before/after indexing, validates
the complete export, then fsyncs a same-directory temporary cache file and atomically
renames it. Cache/temp locations inside the osu! directory are rejected. Quit/signals and
timeouts terminate the adapter process group, reap the direct child, and clean its private temporary tree.
Output limits are 512 MiB for NDJSON and 1 MiB for diagnostics. Freshness does not hash the
audio tree or watch imports; use `r` after closing lazer when content-store changes matter.

Validate and display the fixture-driven library model:

```bash
./bin/osu-player library tests/fixtures/library.ndjson
./bin/osu-player library tests/fixtures/library.ndjson triangles
```

The NDJSON input starts with a schema header, followed by `track` and optional `warning`
records. Tracks are validated, deduplicated by beatmap set and audio hash, then sorted by
their display artist and title.

## Read the real lazer library

**Close lazer before indexing.** The adapter takes an offline copy; copying a database
while the game writes to it is not a supported live snapshot. The game is never launched
by this project.

The adapter targets .NET 10 LTS and pins Realm .NET 20.1.0 with NuGet lock files. Make
uses `.tools/dotnet10/dotnet` when present, otherwise `dotnet` from `PATH`. You can override
the executable with `make DOTNET=/path/to/dotnet indexer`. Building needs a .NET 10 SDK;
running the framework-dependent adapter needs only the .NET 10 runtime.

For a private Linux x86-64 SDK without a system install, run
`python3 tools/install_dotnet.py`. It downloads SDK 10.0.402, verifies its pinned SHA-512,
and installs under `.tools/dotnet10/`; it refuses to overwrite an existing SDK. Older
SDKs under `.tools/dotnet/` are left untouched and are no longer selected automatically.
The AppImage bundles runtime 10.0.13; Arch uses pacman's maintained .NET 10 runtime.

```bash
make indexer
make test-indexer
make test-integration
# For a relocated library:
make test-integration OSU_DIR=/path/to/osu-data
```

`test-indexer` creates synthetic Realm fixtures in a temporary directory. The opt-in
`test-integration` reads your existing library, feeds the adapter's NDJSON into the C++
loader, and by default verifies **Realm/config control files and root directory entries**.
It does **not** walk or hash the songs tree in quick mode. Exported audio paths are still
checked for existence, but quick verification is not proof that every audio file stayed
unchanged.

For large libraries:

```bash
# Default: quick verification, 300-second timeout per subprocess
make test-integration
# Increase timeout for slower disks or especially large databases
make test-integration INTEGRATION_TIMEOUT=900
# Optional expensive check: hash every source file twice
make test-integration INTEGRATION_VERIFY=full INTEGRATION_TIMEOUT=900
```

The timeout applies to each subprocess, not directory hashing or the entire test. The
test prints phase progress and at most five song titles. NDJSON is processed one record
at a time using disk-backed temporary streams rather than multiple in-memory copies.
Full verification can take a long time because it reads the whole library twice.

To display the real library using this VM's local SDK:

```bash
set -o pipefail
.tools/dotnet10/dotnet tools/osu-indexer/bin/Release/net10.0/osu-indexer.dll \
  --osu-dir "$HOME/.local/share/osu" | ./bin/osu-player library -
```

With a system SDK, replace `.tools/dotnet10/dotnet` with `dotnet`. Pass a search argument
after `-` to filter. Build separately: build logs must not be mixed with NDJSON stdout.

The adapter opens only its private temporary copy with Realm, in dynamic read-only mode.
It never migrates the database or writes Realm locks/sidecars in the source directory.
Missing audio and broken filename mappings emit warning records. An incompatible schema
or fatal error exits nonzero, writes diagnostics to stderr, and emits no partial library.
The game version is the only setting read from `game.ini`; credentials are not exported.
Duration is left unknown until the actual audio is probed (beatmap gameplay length is not
necessarily song duration). Temporary copies are removed after use.

For a sanitizer build:

```bash
make clean
make SANITIZE=1
make SANITIZE=1 test
```

`make test` includes synthetic audio transport and CLI tests with SDL's **dummy** output
device: they do not play audible audio or launch osu!. Tests generate their own short WAV,
MP3, and Ogg files in temporary directories. They also test quick/full verification and
incremental validation of a 10,001-record export without real game data.

Controller tests use `tests/fake_audio_player.hpp`: no audio hardware, FFmpeg, Realm,
terminal, or real files are needed to test queue logic. They include a 10,001-entry
all-broken queue, deterministic seeded shuffle, repeat modes, load/play/decode failures,
paused seek recovery, and fatal device failures. Dummy-device CLI tests separately
verify the real controller/backend integration and automatic EOF advancement.

`tests/tui_model_test.cpp` tests filtering/selection with 10,001 entries, viewport bounds,
UTF-8 clipping, and control-character sanitization without ncurses. `tests/tui_integration.py`
opens a pseudo-terminal with generated audio and dummy output to test real key input,
Unicode editing, resize, empty/monochrome screens, device failures, and terminal cleanup
on quit, SIGINT, and SIGTERM. These tests never launch osu! or play audible sound.

`tests/refresh_integration.py` uses a disposable fake adapter and PTY to test responsive
refresh during playing/paused audio, retained filters/selections, cache hits/isolation,
stale/corrupt/truncated caches, malformed output, concurrent source changes, missing
databases, cache-write errors, timeout, cancellation, process reaping, snapshot cleanup,
and export-file reloads. Controller tests also cover library replacement with 10,001+
entries without reloading the current audio. No real-library scan is needed for these tests.

Run `make clean` before switching between regular and sanitizer Make builds.

The intended CMake build, once CMake and Ninja are installed, is:

```bash
cmake -S . -B build -G Ninja -DOSU_PLAYER_ENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The implementation writes only its own caches and temporary files. It does not write to
`client.realm` or lazer's content store.
