# osu!(lazer) TUI Music Player

Early C++20 implementation of the plan in [`PLAN.md`](PLAN.md). The program currently
contains a read-only Realm indexer, a library domain layer, and streaming audio playback
with line-based controls. The ncurses TUI is not implemented yet.

## Current progress

- [x] Validate SHA-256 hashes and resolve lazer content-store paths
- [x] Inspect extensionless audio through an RAII-wrapped C API
- [x] Load a versioned NDJSON library contract
- [x] Validate, deduplicate, sort, and search tracks
- [x] Read `client.realm` through the read-only adapter
- [x] Play audio (pause/resume, stop, seek, volume, EOF handling)
- [ ] Add the ncurses TUI

## Current commands

C++ dependencies: GCC/Clang with C++20, Make (or CMake), `pkg-config`, `json-c`, SDL2,
and FFmpeg development libraries (`libavformat`, `libavcodec`, `libavutil`,
`libswresample`; FFmpeg 5+). Tests also require Python 3.11+ and the `ffmpeg` executable
with MP3 (`libmp3lame`) and Ogg Vorbis (`libvorbis`) encoders.

Build and run tests with the Make fallback available in this VM:

```bash
make
make test
```

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
| `resume` / `r` | Resume (or restart a finished source) |
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

The adapter requires .NET 8 and pins Realm .NET 20.1.0 with NuGet lock files. This VM has a
local SDK in `.tools/dotnet`; Make uses it automatically, otherwise it uses `dotnet` from
`PATH`. You can override the executable with `make DOTNET=/path/to/dotnet indexer`.

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
.tools/dotnet/dotnet tools/osu-indexer/bin/Release/net8.0/osu-indexer.dll \
  --osu-dir "$HOME/.local/share/osu" | ./bin/osu-player library -
```

With a system SDK, replace `.tools/dotnet/dotnet` with `dotnet`. Pass a search argument
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

Run `make clean` before switching between regular and sanitizer Make builds.

The intended CMake build, once CMake and Ninja are installed, is:

```bash
cmake -S . -B build -G Ninja -DOSU_PLAYER_ENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The implementation only reads files. It does not write to `client.realm` or lazer's
content store.
