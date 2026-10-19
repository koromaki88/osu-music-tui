# Changelog

## Unreleased

- Added the shared playback-event and listening-time foundation for future Discord
  Rich Presence and Last.fm scrobbling. No Last.fm submissions yet.
- Added optional TUI Discord Rich Presence with a saved `d` toggle, configurable
  application ID, background Linux IPC, playback timers, pause/stop handling,
  reconnect/backoff, and fake-socket/PTY tests. Defaults off; live Discord unverified.
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
