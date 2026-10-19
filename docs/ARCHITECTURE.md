# Architecture

[Development](../DEVELOPMENT.md) · [Usage](USAGE.md) · [Integrations](INTEGRATIONS.md)

## Components and ownership

- C++20 with RAII wrappers around json-c, FFmpeg, SDL2, ncursesw, and other C APIs.
- `PlayerController` owns the library, injected audio backend, queue indices, and
  playback monitor. Controller mutation, audio pumping, and ncurses stay on the
  control/UI thread. Networking and consumer callbacks never run in the controller.
- FFmpeg streams/resamples into a small SDL output queue instead of whole-song PCM.
  Pump regularly; volume affects newly queued samples (roughly 120 ms plus device latency).
- Owned workers handle indexing/cache loading, Discord IPC, and Last.fm disk/HTTPS.
  Mailbox locks are short-held and never cover network or disk I/O.

## Read-only indexing

- **Runtime:** C# adapter targeting .NET 10 LTS, with Realm .NET 20.1.0 pinned by NuGet locks.
- **Offline access:** lazer must be closed; copying an actively written Realm is not
  a supported snapshot. Only a private copy is opened dynamically and read-only.
- **Source safety:** no source migrations or source locks/sidecars. Only the game
  version is read from `game.ini`; credentials are not exported.

### Export contract

- **Format:** NDJSON schema header, then tracks and optional warnings.
- **Errors:** missing audio/broken filename mappings produce warnings; fatal/schema
  errors exit nonzero without a partial library.
- **Duration:** unknown until audio probing; gameplay length is not used.
- **Import:** validate the complete export, deduplicate by beatmap set/audio hash,
  and sort by display artist/title.

### Process and cache lifecycle

- Spawn directly without a shell, with private disk-backed output, diagnostics,
  and snapshots. Check source Realm metadata before/after indexing.
- Bound output to **512 MiB NDJSON** and **1 MiB diagnostics**.
- On quit/signals/timeouts, terminate the process group, reap the direct child,
  and remove temporary files.

- **Development discovery:** resolve the Release DLL relative to the player, using
  the private SDK when present, otherwise `dotnet` on PATH.
- **Packaged discovery:** resolve `../lib/osu-player/indexer/osu-indexer` relative
  to the player executable.

- **Cache headers:** canonical source path, version, count, Realm identity/size,
  and nanosecond timestamps.
- **Cache writes:** private same-directory staging file, fsync, then atomic rename.
  Cache/temp paths inside the source library are rejected.

## Library replacement, settings, and terminal text

- **Refresh identity:** stable ID plus audio hash preserves the loaded stream
  without reload/seek/pause.
- **Queue:** surviving shuffled entries retain order; new entries append randomly.
- **Replacement:** removed/replaced current tracks pause without autoplay;
  failed-track suppression resets. Restore selection by stable ID where possible.

- **Settings:** bounded versioned JSON, private atomic writes, preservation of
  invalid/newer files. Save applied search edits only; never restore playback state.
- **Terminal text:** control/escape-sanitized and clipped by cell width. Full emoji
  grapheme-cluster modeling is not implemented.

## Playback events and listening time

### Event contract

- **Drain once:** `take_playback_events()` returns an owned, occurrence-ordered batch;
  the TUI fans out the same batch to integrations.
- **Ownership:** copy snapshots before crossing threads or retaining them across
  controller mutations.
- **Privacy:** event metadata excludes filesystem paths/filenames.

- **Events:** start, progress, pause/resume, successful seek, metadata changes,
  listening eligibility, and session end with an explicit lifecycle reason.
- **Session IDs:** random controller namespace plus counter, independent of shuffle
  RNG/wall time.
  - Replay/repeat/stop-then-play create new sessions.
  - Pause/resume/seek and surviving refresh preserve identity.
  - Failed playback and loaded-but-paused recovery do not announce listens;
    recovery gets a session when playback actually starts.

- **Ended snapshots:** retain final position, duration, metadata, listening time,
  original wall-start timestamp, and session ID.
- **Journal:** adjacent progress coalesces; at most 128 events plus an `omitted` count.
  Resynchronize current state after overflow, but never invent missing listening history.
- **Shutdown:** frontend `shutdown()` settles the session on normal exit; abrupt
  termination cannot emit a final event.

### Listening accounting

- **Clocks:** `PlaybackClock` injects steady/wall clocks. Wall time records the original
  start; only steady elapsed time and positive backend-consumed media movement earn credit.
- **Bounds:** preserve fractional time across irregular polls; cap credit by media
  progress and available playing time. Never credit future silence after a jump.
- **Rebasing:** pause/seek rebase accounting; paused time/seek displacement do not count.
- **Settlement:** settle old sessions before seek/stop/replacement/EOF; failed seeks
  do not credit uncertain displacement.

> [!NOTE]
> Listening credit is a conservative estimate, not proof of human listening.

Eligibility fires once per session: known duration strictly over 30 seconds, with
credited listening reaching half duration or four minutes. A later-learned duration
may qualify accumulated listening; authorization/metadata/delivery are separate checks.

## Discord worker

- **Ownership:** `discord_presence.hpp/.cpp` owns a `std::jthread`, one latest snapshot,
  and status/config mailboxes.
- **Protocol:** bounded little-endian frames, READY/matching ACKs, ping/pong, partial
  nonblocking I/O, absolute deadlines, same-UID checks, and bounded reconnect backoff.
- **Discovery:** `discord-ipc-0` through `-9`; an explicit socket override suppresses discovery.
- **Diagnostics:** peer error text is never displayed verbatim.

- **Scheduling:** meaningful changes trigger coalesced updates with a five-second
  normal throttle. Progress alone generates no RPC traffic; Discord advances timestamps.
- **Idle behavior:** `eventfd` wakes for state/config/shutdown, avoiding periodic polling.
- **Clearing:** stop clears immediately; disable/exit attempts a bounded clear before joining.
- **Payload:** UTF-8-sanitized, byte-bounded metadata. Only numeric online IDs form
  `https://assets.ppy.sh/beatmaps/{id}/covers/list@2x.jpg`; Discord fetches the image.

## Last.fm worker and durable delivery

### Transport

- **Worker:** `lastfm_presence.cpp` owns authentication, Now Playing, event ingestion,
  scheduling, and cancellation.
- **Client:** `lastfm_client.cpp` signs/forms requests and performs verified HTTPS to
  `https://ws.audioscrobbler.com/2.0/?format=json`: no redirects, bounded replies,
  deadlines, and cancellation. Credentials/signatures stay in POST bodies.
- **Validation:** hexadecimal API keys; bounded opaque server tokens/session keys.
- **Diagnostics:** fixed categories/numeric codes, never raw bodies or exception text.

### Outbox

- **Ownership:** exclusive nonblocking `flock`, account-associated FIFO entries,
  and recent receipts.
- **Persistence:** save before sending; retain original timestamps/metadata.
- **Admission:** disabled/unauthorized events are not cached for later.
- **Shutdown:** flush accepted events normally; unsaved events may be lost on abrupt termination.

| Limit | Maximum |
|---|---|
| Pending plays | 1,024 |
| Receipts | 4,096 |
| Outbox file | 8 MiB |
| Incoming mailbox | 128 events |

### Delivery outcomes

Requests are spaced at least **200 ms**.

| Outcome | Handling |
|---|---|
| API 11/16/29, transport failure, uncertain reply | Retain entry; exponential 30-second–30-minute backoff |
| API 9 | Require reauthorization |
| Credential/permission failure | Pause delivery |
| Valid ignored reply or permanent rejection | Terminal receipt |

- ACKs must have consistent counts, ignored code, and original timestamp; corrected
  server metadata is not reused.
- Lost replies/crashes before receipt persistence can duplicate delivery.
- No expiry silently deletes backlog; no missing history is reconstructed.

### Storage hardening and scheduling

- **Anchoring:** `lastfm_storage::Directory` uses an owner-only directory descriptor
  with `openat`/`renameat`/`unlinkat`.
- **Writes:** random exclusive 0600 staging files, file/directory fsync, directory
  identity checks, single-link regular files, and close-on-exec descriptors.
- **Threat boundary:** limits path-replacement/link attacks, not root or malicious
  same-UID processes. Storage remains plaintext.
- **Retry clock:** convert once per head/deadline to bounded monotonic waits.
- **Copies/deduplication:** copy queue heads only on submission; detect duplicates
  on load with a bounded identity set.

## Test map

| Area | Main tests |
|---|---|
| Controller/queue | `tests/player_controller_test.cpp`, `tests/fake_audio_player.hpp` |
| Listening/session identity | `tests/playback_events_test.cpp` (fake clocks/backend) |
| Library/text | `tests/library_test.cpp`, `tests/tui_model_test.cpp` |
| Audio/CLI | `tests/audio_integration.py` (generated WAV/MP3/Ogg, dummy SDL) |
| UI/settings | `tests/tui_integration.py` (PTYs, isolated config) |
| Refresh/cache | `tests/refresh_integration.py` (fake adapter, failures/timeouts) |
| Realm adapter | `tests/osu-indexer-tests/` (synthetic schemas/files) |
| Discord | `tests/discord_presence_test.cpp`, `tests/discord_integration.py` (fake IPC, idle probe) |
| Last.fm protocol/storage | `tests/lastfm_test.cpp`, `tests/lastfm_scrobble_test.cpp` |
| Last.fm HTTPS | `tests/lastfm_integration.py` (loopback CONNECT proxy, ephemeral certificates) |
| Packages | `tests/appimage_integration.py`, `tests/arch_package_integration.py` |

- **Last.fm regressions:** controller-driven threshold/shutdown events, FIFO/restart/account
  isolation, malformed ACKs/files, revocation, cancellation, unsafe links/directories,
  extreme clocks, disconnect ordering, and concurrent playback revisions.
- **HTTPS fixtures:** real libcurl signatures/authentication/Now Playing/scrobbles/restart
  and certificate/hostname rejection—no Last.fm contact or personal credentials.
- **Scale:** large-library/controller tests include 10,001-entry cases.
- **Performance scope:** whole-player profiling is deferred; isolated worker
  measurements are not whole-player resource guarantees.

## References

- [Last.fm desktop auth](https://www.last.fm/api/desktopauth)
- [Last.fm scrobbling](https://www.last.fm/api/scrobbling)
- [Last.fm Now Playing](https://www.last.fm/api/show/track.updateNowPlaying)
- [Discord RPC](https://docs.discord.com/developers/topics/rpc)
