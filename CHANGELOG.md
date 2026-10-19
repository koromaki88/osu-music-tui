# Changelog

## Unreleased

- Added the shared playback-event and listening-time foundation for future Discord
  Rich Presence and Last.fm scrobbling. No Last.fm submissions yet.
- Added optional TUI Discord Rich Presence with a saved `d` toggle, configurable
  application ID, background Linux IPC, playback timers, pause/stop handling,
  reconnect/backoff, and fake-socket/PTY tests. Defaults off; live Discord unverified.
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
