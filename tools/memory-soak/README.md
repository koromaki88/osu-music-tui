# Contained playback / Last.fm memory investigation

One-off investigation, not part of the player or routine build. This directory
contains the harness and runner; local raw results live under
`artifacts/memory-soak/`. Remove those two directories together when the evidence
is no longer needed. No production source changes are required by this harness.

The runner generates two-second WAV/MP3/Ogg files, private fake credentials and an
isolated TLS fixture. Each mode runs in a fresh process. Each cycle loads a different
codec through the real audio backend, starts it with SDL dummy output, and pumps
one audio buffer. This stresses replacement/decoder allocation, not sustained
real-time decoding of an entire song. No library scan, game launch, personal
credentials, artwork requests or live Last.fm contact occurs.

Modes:

- `off`: audio replacement only, no Last.fm worker.
- `fake`: real Last.fm worker/persistence with an injected transport.
- `https`: real Last.fm worker/persistence and libcurl/OpenSSL against an isolated
  local CONNECT/TLS fixture using a generated trusted certificate. Verification and
  hostname checking remain enabled. The fixture validates request signatures and
  retains a count rather than growing request history.

For Last.fm modes, a synthetic eligible snapshot is sent for each cycle, producing
one Now Playing and one scrobble request. The fake listened time is deliberately
independent of the brief audio pumping. This accelerates thousands of session and
durable-receipt transitions; it does not exercise real listening-clock eligibility.
Each cycle waits for both replies and for pending work to drain. The default 4,500
cycles crosses the 4,096-receipt bound and represents 9,000 successful API requests
in each Last.fm mode, rather than an actual overnight wall-clock run.

CSV measurements use `/proc/self/statm` resident pages and glibc `mallinfo2()`:
arena in-use/free bytes and allocator mmap bytes. Arena in-use is an allocator
accounting metric, **not an exact count of application-owned live allocations**;
it includes runtime/library allocations and does not cover every mmap. RSS includes
code/shared pages and retained free heap. Samples are taken at cycle boundaries;
transient peak allocations are not captured. At completion the harness destroys
the worker/audio backend, then calls `malloc_trim(0)` as a diagnostic only. Trimming
is not a production fix and is never added to the application.

Separate ASan/UBSan/LeakSanitizer runs check shorter fake and HTTPS cases. Their RSS
and allocator numbers are not comparable with the ordinary run because sanitizer
allocation/quarantine behavior differs. System-library internals are not fully
instrumented; a clean run is not proof against all leaks or all real-world scenarios.

Build from the repository root:

```sh
g++ -std=c++20 -O2 -g -pthread -DOSU_PLAYER_VERSION='"0.6.0"' -Iinclude \
  $(pkg-config --cflags libavformat libavcodec libavutil libswresample sdl2 json-c libcurl libcrypto) \
  tools/memory-soak/soak.cpp src/audio_player.cpp src/audio_probe.cpp \
  src/lastfm_presence.cpp src/lastfm_client.cpp src/lastfm_queue.cpp \
  src/settings.cpp src/storage_path.cpp src/track.cpp \
  $(pkg-config --libs libavformat libavcodec libavutil libswresample sdl2 json-c libcurl libcrypto) \
  -o /tmp/opencode/lastfm-memory-soak
python3 tools/memory-soak/run.py --binary /tmp/opencode/lastfm-memory-soak \
  --output artifacts/memory-soak/round1 --cycles 4500
```

For sanitizer compilation replace `-O2` with
`-O1 -fsanitize=address,undefined -fno-omit-frame-pointer`, use a different output
binary, and pass `--cycles 200 --modes fake https` with a separate results directory.
The soak does not include ncurses, Discord, collection browsing, full library
replacement/refresh, long real songs, real Last.fm responses/retries or OBS. Those
require separate follow-up if indicated by the measurements.
