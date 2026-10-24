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

Library construction retains the first record for each set/audio pair. Its sort
comparator folds ASCII bytes
directly without allocating strings per comparison; unsigned-byte Unicode ordering
and stable-ID tie breaking match the original folded-string ordering.

Optional `Beatmap.BPM` and `BeatmapSet.DateAdded` export as positive finite BPM and
UTC import timestamp. Missing fields stay null. Deduplication retains the same
representative beatmap used for each track's other metadata; BPM is not audio-derived
and may differ across difficulties.

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

Help/details reuse the library viewport rather than creating separate blocking
ncurses loops. `tui_panels.hpp/.cpp` owns panel state/content and pure playlist input
handling; `tui_command.cpp` renders and dispatches the returned intents to playback
or the collection worker. An owned details snapshot avoids dangling library
references across refresh. Panel input scrolls independently; only explicitly allowed
transport controls pass through. `wrap_terminal_text` sanitizes and wraps by cell
width with an 8,192-codepoint budget per field. Wrapped lines are cached until width
changes; height changes only clamp scrolling. Playback pumping and integration workers
continue normally. PTY tests cover minimum/wide/tiny layouts, selection preservation,
empty details, panel switching, playback controls, and error context.

`track_details_probe.hpp` owns one latest-request worker for missing duration. Closing
or replacing a panel cancels the request; generation checks reject stale results.
The worker opens regular local files read-only and rejects FIFOs/devices. FFmpeg uses
an anchored descriptor path, file-only protocol whitelist, cooperative cancellation,
and a five-second interrupt deadline. This is not a hard deadline for uninterruptible
kernel/filesystem I/O. Unknown/error results remain explicit; refresh does not probe
the whole library and probe results are not persisted in the library cache.

## Collections and scoped queues

`collections.hpp` exposes the store API, snapshots and centralized capacity limits.
`collections.cpp` owns one sleeping worker and a bounded FIFO edit mailbox.
`collections_model.cpp` handles edits, validation and reference lookup;
`collections_file.hpp/.cpp` is the internal JSON/private-file boundary.
Immutable shared snapshots reach the UI only after a successful save. Private storage is descriptor-anchored, exclusively
locked, bounded, UTF-8/version validated, and atomically replaced with file/directory
fsync. Changes become visible only after saving; failures retain the previous
snapshot. Unchanged serialized edits retain the existing snapshot and skip file
replacement/fsync, but still read/check for external edits. Direct edits validate
UTF-8 without re-parsing a serialized copy. UI snapshot polling is limited
to the existing 100 ms rendering cadence; no disk I/O runs on the control thread.

References pair stable ID with audio hash. Favorite markers binary-search a sorted
index of owned positions without allocating temporary keys; copying a snapshot
does not leave borrowed pointers. Resolution indexes only requested references with
call-local string views, scans the library, and preserves saved order while retaining
unavailable members in storage. `LibraryView` searches only its optional ordered
scope rather than searching the full library and filtering afterwards; this does
not change playback. `play_queue`
validates unique indices/selection before replacing the active queue; a separate
canonical order restores playlist order when shuffle is disabled. Refresh remaps
the scoped queue by identity without appending unrelated library entries. Failure
flags remain indexed by the full library, not by subset size. Saved edits affect
future queue snapshots rather than interrupting active playback.

Unit and PTY tests cover persistence, limits, locks/unsafe links, failed saves,
external/directory replacement, missing references, list editing, resize, no autoplay,
shuffle/history/repeat, and scope-preserving refresh.

## Optional OBS browser source

`BrowserOverlay` in `browser_overlay.hpp/.cpp` owns a sleeping worker and a bounded
latest-value mailbox. The frontend copies only display title/artist, position,
duration, playing state and a public cover URL derived from a positive online set ID
from the existing playback snapshot at most twice per
second while enabled. There is no event backlog, indexing, playback
mutation or socket I/O on the UI thread for this service. No new native dependency
is required: JSON uses the existing json-c library, HTTP uses Linux nonblocking sockets.

The worker binds only `127.0.0.1:7270` on opt-in, serves embedded HTML and a JSON
state endpoint, and closes sockets when disabled or destroyed. HTTP work is bounded
to eight clients, a 4 KiB request budget, two seconds per connection, and 4 KiB per
display text field. Slow readers/writers cannot block other clients or shutdown;
an enabled worker polls at most every 100 ms while waiting, a disabled worker sleeps
on a condition variable. Bind failure sleeps until reconfiguration rather than retrying.

Exact Host, Origin (when present), and Fetch Metadata checks reject cross-origin
requests and DNS rebinding. No CORS, arbitrary file/URL routing or mutating endpoints
are provided. Responses use no-store, nosniff and a restrictive CSP; metadata enters
the DOM through `textContent`, never HTML. This is unauthenticated loopback sharing,
not protection against other local processes/users. The page polls every 500 ms,
times out stalled fetches and clears stale data on disconnect; progress is not animated.

Thumbnails use a fixed `https://assets.ppy.sh/beatmaps/<positive-id>/covers/list@2x.jpg`
pattern, with a matching browser-side URL check and CSP `img-src` restricted to
that host. The browser, not the player, fetches public artwork without a referrer.
There is no proxy, local-file artwork access, upload or server-side image fetch.
The square image is flex-aligned left of the text; missing/error/idle/disconnected
states hide it. `show_thumbnail: false` suppresses URLs and requests; size is bounded
to 32–256 px. Synthetic tests verify URL derivation, disabling and privacy without
requesting real artwork; actual image rendering/failure behavior needs a visual check.

`overlay_config.hpp/.cpp` reads/validates an owned regular `overlay.json` on the
worker once per second while enabled (8 KiB limit, no-follow/nonblocking open).
First successful enable creates private defaults with exclusive creation and never
overwrites existing data. Defaults and a whitelisted normalized style JSON are
served through `/style`, not arbitrary config contents. Invalid edits preserve the
last valid style transactionally and surface a persistent status warning. Numeric
ranges, strict booleans, #RRGGBB colors and plain font-family names prevent arbitrary
CSS/script/URL input. The page applies values through individual DOM style properties
every two seconds; `width: null` clears the optional cap and restores fluid width.

`tests/overlay_integration.py` covers default-off, port conflict/retry, persistence
without autoplay, metadata/privacy, HTTP boundary checks, pause/seek/idle transitions,
slow clients and shutdown against the real TUI with synthetic audio. Config unit/integration tests cover creation,
hot reload, rollback, restart, defaults, bounds and unsafe file types. Actual OBS/CEF
rendering remains a manual visual check. Routine tests require loopback port 7270
to be free and never stop any pre-existing service to obtain it.

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
