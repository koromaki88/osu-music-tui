# Changelog

## Unreleased

- Restore enabled Last.fm in the background without automatically opening its menu
  on launch; setup remains available with `l`.
- Export optional beatmap BPM and set import date during indexing; load missing
  track-details duration with a cancellable background local-file probe.
- Added scrollable in-app help (`?`) and selected-track details (`i`), with
  Unicode-aware reflow on resize, persistent playback controls/status, metadata
  snapshots, and recent track-error context. No extra indexing.
- Reorganized documentation into a concise quick start and focused usage,
  integration, architecture, packaging, and release-validation guides.

## 0.4.0

- Packaged optional Last.fm authorization, Now Playing, and durable scrobbling;
  native live submission was user-confirmed. Added isolated TLS/package regressions.
- AppImage uses host CA certificates when available, with a bundled fallback for
  minimal systems. Explicit `SSL_CERT_FILE` is respected; TLS remains verified.
- Hardened Last.fm private-file operations with validated directory descriptors,
  descriptor-relative atomic writes, hard-link rejection, and close-on-exec files.
- Bounded retry waits across clock changes, rejected malformed numeric API errors,
  removed repeated queue-head payload copies, and made outbox duplicate checks on
  load linear on average (including duplicate receipt validation).
- Hardened Last.fm scrobble replies against unrelated playback changes and made
  local disconnect survive immediate shutdown/re-enable. Added controller-to-fake-
  service coverage for pause/seek accounting, replay, and shutdown eligibility.
- Added Last.fm scrobble submission at the listening threshold, a private durable
  account-associated outbox, FIFO retries/backoff, revoked-session recovery, bounded
  duplicate receipts, pending/unsaved status, and isolated persistence/delivery tests.
  Ambiguous replies or crashes can still cause duplicates on retry; not exactly-once.
- Request Last.fm JSON format explicitly in the endpoint URL, accept bounded opaque
  server tokens/session keys instead of assuming hexadecimal API-key syntax, and
  distinguish JSON parsing from missing/invalid authorization response fields.
- Separate Last.fm credential-file, HTTPS/curl, signing, and response-format failures
  instead of hiding them behind a combined request/credential error.
- Distinguish Last.fm invalid-key, signature, permission, and suspended-key errors
  using safe numeric codes without exposing credentials or server response text.
- Changed the Discord cover label from `osu! player` to `osu! music player`.
- Added optional Last.fm personal API credential setup, browser authorization,
  private session storage, and background HTTPS Now Playing updates with fake-service
  and TUI regression tests.

## 0.3.0

- Added the shared playback-event and listening-time foundation for future Discord
  Rich Presence and Last.fm scrobbling. No Last.fm submissions yet.
- Added optional TUI Discord Rich Presence with a saved `d` toggle, configurable
  application ID, background Linux IPC, playback timers, pause/stop handling,
  reconnect/backoff, and fake-socket/PTY tests. Defaults off; live display user-verified.
- Removed periodic idle Discord worker polling and redundant progress metadata copies;
  state/socket notifications now wake the worker only when needed.
- Added an in-app Discord Application ID prompt when enabling without a valid saved
  ID, with validation, cancellation, and automatic persistence; playback continues.
- Discord presence now uses dynamic public osu! beatmap thumbnails for tracks with
  an online set ID, with the player asset as a small overlay; local tracks keep the
  player image. Discord fetches images without player downloads or extra polling.
- Discord presence uses the song title as its activity name and keeps the artist line,
  omitting the extra song-title line; the bottom cover label reads `osu! player`.
- Replaced the application icon's self-intersecting note with a centered eighth note.

### Release scope
- Last.fm authorization/submission and durable scrobble storage are not included.
- Local unsigned packages only; public distribution still requires application-license
  selection and dependency/source-compliance review. No installation or publication
  is implied by local package validation.

## 0.2.0

### Added
- TUI preferences persist across restarts: volume, shuffle, repeat, and applied search.
- Private, atomic XDG config writes, with nonfatal settings errors and preservation of
  invalid or unsupported settings files. Playback never starts automatically on restore.
- A dedicated development guide and explicit packaging/release workflow.

### Changed
- AppImage builds reuse one toolchain image and run compilation/tests in disposable
  containers, avoiding accumulating source-dependent Docker layers.
- Added targeted toolchain cleanup without globally pruning unrelated Docker data.
- README focuses on usage and basic developer setup; development details live separately.

### Validation scope
- Synthetic tests cover preferences, restarts, forced termination, malformed settings,
  config-write failures, and XDG path fallback, including packaged executables.
- Local unsigned packages only; public redistribution still requires license and
  dependency/source-compliance review.
- Discord RPC and Last.fm scrobbling are not included in this release.
