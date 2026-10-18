# osu!(lazer) TUI Music Player

Early C++20 implementation of the plan in [`PLAN.md`](PLAN.md). The program currently
contains read-only storage/audio experiments and the fixture-driven library domain layer;
it is not yet a TUI player.

## Current progress

- [x] Validate SHA-256 hashes and resolve lazer content-store paths
- [x] Inspect extensionless audio through an RAII-wrapped C API
- [x] Load a versioned NDJSON library contract
- [x] Validate, deduplicate, sort, and search tracks
- [x] Read `client.realm` through the read-only adapter
- [ ] Play audio
- [ ] Add the ncurses TUI

## Current commands

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
loader, and checks that source file hashes, sizes, timestamps, and directory entries are
unchanged. It hashes the whole data directory, so it can be slow for large libraries.

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

The intended CMake build, once CMake and Ninja are installed, is:

```bash
cmake -S . -B build -G Ninja -DOSU_PLAYER_ENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The implementation only reads files. It does not write to `client.realm` or lazer's
content store.
