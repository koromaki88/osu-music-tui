# Architecture

[Development](../DEVELOPMENT.md) · [Usage](USAGE.md) · [Integrations](INTEGRATIONS.md)

## Components and ownership

The player uses C++20 with RAII wrappers around json-c, FFmpeg, SDL2, ncursesw, and
other C APIs. `PlayerController` owns the library, injected audio backend, queue
indices, and playback monitor.

Controller mutation, audio pumping, and ncurses stay on the control/UI thread.
Owned workers handle indexing/cache loading, Discord IPC, and Last.fm disk/HTTPS.
Mailbox locks are short-held and never cover network or disk I/O; networking and
consumer callbacks never run in the controller.

FFmpeg streams/resamples into a small SDL output queue instead of whole-song PCM.
Pump regularly; volume affects newly queued samples (roughly 120 ms plus device latency).

## Read-only indexing

The C# adapter targets .NET 10 LTS, with Realm .NET 20.1.0 pinned by NuGet locks.
Lazer must be closed: copying an actively written Realm is not a supported snapshot.
Only a private copy is opened dynamically and read-only; the source is never
migrated or given locks/sidecars. Only the game version is read from `game.ini`;
credentials are not exported.

### Export contract

NDJSON starts with a schema header, followed by tracks and optional warnings.
Missing audio/broken filename mappings produce warnings; fatal/schema errors exit
nonzero without a partial library. Duration stays unknown until audio probing;
gameplay length is not used.

The player validates the complete export, deduplicates by beatmap set/audio hash,
and sorts by display artist/title.

### Process and cache lifecycle

The adapter is spawned directly without a shell, with private disk-backed output,
diagnostics, and snapshots. Source Realm metadata is checked before/after indexing.
Output is bounded to **512 MiB NDJSON** and **1 MiB diagnostics**. Quit/signals/timeouts
terminate the process group, reap the direct child, and remove temporary files.

- **Development discovery:** resolve the Release DLL relative to the player, using
  the private SDK when present, otherwise `dotnet` on PATH.
- **Packaged discovery:** resolve `../lib/osu-player/indexer/osu-indexer` relative
  to the player executable.

Cache headers record canonical source path, version, count, Realm identity/size,
and nanosecond timestamps. Private updates fsync a same-directory staging file,
then atomically rename it. Cache/temp paths inside the source library are rejected.

## Library replacement, settings, and terminal text

Refresh matches stable ID plus audio hash to preserve the loaded stream without
reload/seek/pause. Surviving shuffled entries retain order; new entries append randomly.
Removed/replaced current tracks pause without autoplay, failed-track suppression
resets, and selection is restored by stable ID where possible.

Settings use bounded versioned JSON and private atomic writes, preserving invalid/newer
files. Only applied search edits are saved; playback state is never restored.
Terminal metadata is control/escape-sanitized and clipped by cell width. Full emoji
grapheme-cluster modeling is not implemented.

## Playback events and listening time

### Event contract

`take_playback_events()` returns an owned, occurrence-ordered batch. The TUI drains
once and fans out that batch to integrations. Copy snapshots before crossing threads
or retaining them across controller mutations. Event metadata excludes paths/filenames.

Events cover start, progress, pause/resume, successful seek, metadata changes,
listening eligibility, and session end with an explicit lifecycle reason. Session IDs
use a random controller namespace plus a counter, independent of shuffle RNG/wall time:

- Replay/repeat/stop-then-play create new sessions.
- Pause/resume/seek and surviving refresh preserve identity.
- Failed playback and loaded-but-paused recovery do not announce listens;
  recovery gets a session when playback actually starts.

Ended snapshots retain final position, duration, metadata, listening time, original
wall-start timestamp, and session ID. Adjacent progress coalesces; the journal holds
at most 128 events plus an `omitted` count. Consumers may resynchronize current state
after overflow, but must not invent missing listening history. Frontend `shutdown()`
settles the session on normal exit; abrupt termination cannot emit a final event.

### Listening accounting

`PlaybackClock` injects steady/wall clocks. Wall time records the original start;
only steady elapsed time and positive backend-consumed media movement earn credit.
Accounting preserves fractional time across irregular polls, caps credit by media
progress and available playing time, and never credits future silence after a jump.

Pause/seek rebase accounting; paused time/seek displacement do not count. Old sessions
settle before seek/stop/replacement/EOF. Failed seeks do not credit uncertain displacement.

> [!NOTE]
> Listening credit is a conservative estimate, not proof of human listening.

Eligibility fires once per session: known duration strictly over 30 seconds, with
credited listening reaching half duration or four minutes. A later-learned duration
may qualify accumulated listening; authorization/metadata/delivery are separate checks.

## Discord worker

`discord_presence.hpp/.cpp` owns a `std::jthread`, one latest snapshot, and status/config
mailboxes. IPC uses bounded little-endian frames, READY/matching ACKs, ping/pong,
partial nonblocking I/O, absolute deadlines, same-UID checks, and bounded reconnect
backoff. Discovery checks `discord-ipc-0` through `-9`; an explicit socket override
suppresses discovery. Peer error text is never displayed verbatim.

Meaningful changes trigger coalesced updates with a five-second normal throttle.
Progress alone generates no RPC traffic; Discord advances timestamps. `eventfd`
wakes for state/config/shutdown, avoiding periodic polling. Stop clears immediately;
disable/exit attempts a bounded clear before joining.

Payload metadata is UTF-8-sanitized and byte-bounded. Only numeric online IDs form
`https://assets.ppy.sh/beatmaps/{id}/covers/list@2x.jpg`; Discord fetches the image.

## Last.fm worker and durable delivery

### Transport

`lastfm_presence.cpp` owns authentication, Now Playing, event ingestion, scheduling,
and cancellation. `lastfm_client.cpp` signs/forms requests and performs verified HTTPS
to `https://ws.audioscrobbler.com/2.0/?format=json`, with no redirects, bounded replies,
deadlines, and cancellation. Credentials/signatures stay in POST bodies.

API keys use hexadecimal validation; server tokens/session keys use bounded opaque
validation. Diagnostics use fixed categories/numeric codes, never raw bodies or exception text.

### Outbox

`LastfmQueue` owns an exclusive nonblocking `flock`, account-associated FIFO entries,
and recent receipts. It saves before sending, retaining original timestamps/metadata.
Disabled/unauthorized events are not cached for later. Normal shutdown flushes
accepted events; unsaved events may be lost on abrupt termination.

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

ACKs must have consistent counts, ignored code, and original timestamp; corrected
server metadata is not reused. Lost replies/crashes before receipt persistence can
duplicate delivery. No expiry silently deletes backlog, and no missing history is reconstructed.

### Storage hardening and scheduling

`lastfm_storage::Directory` anchors operations to an owner-only directory descriptor
with `openat`/`renameat`/`unlinkat`. Random exclusive 0600 staging files, file/directory
fsync, directory identity checks, single-link regular files, and close-on-exec descriptors
limit path-replacement/link attacks—not root or malicious same-UID processes. Storage
remains plaintext.

Retry deadlines convert once per head/deadline to bounded monotonic waits. Queue heads
are copied only on submission; a bounded identity set detects duplicates on load.

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

Last.fm regressions cover controller-driven threshold/shutdown events, FIFO/restart/account
isolation, malformed ACKs/files, revocation, cancellation, unsafe links/directories,
extreme clocks, disconnect ordering, and concurrent playback revisions. HTTPS fixtures
exercise real libcurl signatures/authentication/Now Playing/scrobbles/restart and
certificate/hostname rejection without Last.fm contact or personal credentials.

Large-library/controller tests include 10,001-entry cases. Whole-player profiling
remains deferred; isolated worker measurements are not whole-player resource guarantees.

## References

- [Last.fm desktop auth](https://www.last.fm/api/desktopauth)
- [Last.fm scrobbling](https://www.last.fm/api/scrobbling)
- [Last.fm Now Playing](https://www.last.fm/api/show/track.updateNowPlaying)
- [Discord RPC](https://docs.discord.com/developers/topics/rpc)
