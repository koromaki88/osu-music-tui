# osu!(lazer) TUI Music Player

Early C++20 implementation of the plan in [`PLAN.md`](PLAN.md). The program currently
contains read-only storage/audio experiments and the fixture-driven library domain layer;
it is not yet a TUI player.

## Current progress

- [x] Validate SHA-256 hashes and resolve lazer content-store paths
- [x] Inspect extensionless audio through an RAII-wrapped C API
- [x] Load a versioned NDJSON library contract
- [x] Validate, deduplicate, sort, and search tracks
- [ ] Read `client.realm` through the read-only adapter
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
