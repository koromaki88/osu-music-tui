# osu!(lazer) TUI Music Player

A terminal music player for an existing osu!(lazer) library, with search, streaming
playback, shuffle, repeat, and saved preferences. It reads your library without modifying
the original database or content files. **Close lazer before indexing or refreshing.**

For contributor workflows, packaging, detailed testing, and implementation notes, see
[DEVELOPMENT.md](DEVELOPMENT.md). Architecture and milestones are in [PLAN.md](PLAN.md).

## Standalone application

The distributable is **one executable file**, including the C++ player, self-contained
.NET 10 indexer, Realm, FFmpeg, SDL, ncurses, json-c, and native application dependencies.
End users do not need the source directory, a compiler, an SDK, or a separate .NET install.

After obtaining `osu-player-x86_64.AppImage`, close lazer and run it from a terminal:

```bash
chmod +x osu-player-x86_64.AppImage
./osu-player-x86_64.AppImage tui
```

You can move/rename this file or keep it in `~/.local/bin/`. Cache files remain in the
normal XDG location.

The default release build uses a Debian 12 baseline: **Linux x86-64 with glibc 2.36 or
newer**, `/bin/sh`, a UTF-8 locale, and working host audio services/drivers are required.
This is not an Alpine/musl build or a Windows/macOS executable. glibc and the dynamic
loader are deliberately not bundled. Consult the generated manifest for the measured
minimum glibc version of a particular build; older distributions are not supported yet.

Normal mounted execution needs a working FUSE setup and a suitable `fusermount` helper.
If those are unavailable (for example, in some containers), no installation is necessary;
use the built-in fallback:

```bash
./osu-player-x86_64.AppImage --appimage-extract-and-run tui --osu-dir /path/to/osu-data
```

That fallback decompresses to temporary storage each launch, so startup is slower than
normal mounted execution. For repeated use without FUSE, extract once and run the folder:

```bash
./osu-player-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun tui --osu-dir /path/to/osu-data
```

Only the AppImage is required to run. The checksum and manifest are useful for verifying
downloads and auditing the included dependencies. Verify with `sha256sum -c` from its
directory. A checksum detects corruption; it is not a publisher signature.

**Release caveats:** the current package is a local unsigned build, not a published
download. Public redistribution still needs an application-license decision and
dependency/source-compliance review; see [packaging/THIRD_PARTY.md](packaging/THIRD_PARTY.md).
Existing packages may lag behind the development build; check the changes included in
your package.

## Native Arch Linux package

Arch x86-64 users can choose a normal **pacman package** instead of the AppImage.
The current build is approximately **4.4 MiB** (12 MiB installed), excluding system
dependencies. It installs `osu-player` in `/usr/bin` and the adapter in
`/usr/lib/osu-player/indexer`. No FUSE, Docker, wrapper, or development directory
is needed to run it.

Install the generated local package, then close lazer and start the player:

```bash
sudo pacman -U artifacts/arch/osu-music-player-0.2.0-1-x86_64.pkg.tar.zst
osu-player tui
```

Pacman resolves missing runtime dependencies from your configured repositories:
FFmpeg, SDL2 (provided by `sdl2-compat` on current Arch), ncurses, json-c, and
`dotnet-runtime-10.0` (also provided by current Arch's `dotnet-runtime`), plus their
dependencies. Unlike the AppImage, this package **does require the system .NET 10
runtime**, but no SDK is needed by end users.
It does not bundle copies of system libraries. Explicit FFmpeg SONAME dependencies
prevent silently using an incompatible FFmpeg major version.

This is a native **rolling-release Arch** build, not a portable Linux binary.
Use a fully updated system; rebuild after incompatible library/ABI upgrades.
The package is local and unsigned, and is not published to the AUR or an Arch repository.
The same application-license/redistribution review noted above remains necessary before
public distribution. To uninstall: `sudo pacman -R osu-music-player`.

## Build from source

C++ dependencies: GCC/Clang with C++20, Make (or CMake), `pkg-config`, `json-c`, SDL2, `ncursesw`,
and FFmpeg development libraries (`libavformat`, `libavcodec`, `libavutil`,
`libswresample`; FFmpeg 5+). Tests also require Python 3.11+ and the `ffmpeg` executable
with MP3 (`libmp3lame`) and Ogg Vorbis (`libvorbis`) encoders.

Build and run tests:

```bash
make
make test
```

Directory-mode browsing also needs a .NET 10 SDK to build the indexer. Use your system
SDK or install a private Linux x86-64 SDK with `python3 tools/install_dotnet.py`.

```bash
make indexer
# Close lazer before starting:
./bin/osu-player tui
```

Make prefers `.tools/dotnet10/dotnet` when present, otherwise `dotnet` on PATH.
Override it with `make DOTNET=/path/to/dotnet indexer`. The SDK installer verifies its
download and refuses to overwrite an existing SDK. Packaged users do not need this step.
`make test` saves output to `artifacts/tests.log`. See [DEVELOPMENT.md](DEVELOPMENT.md)
for advanced tests, sanitizer/CMake builds, and package creation.

## CLI playback and inspection

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
| `resume` / `r` | Resume (or restart a stopped selection) |
| `stop` | Pause and rewind |
| `seek 45.5` | Seek to 45.5 seconds |
| `volume 0.5` | Set volume in the range 0–1 |
| `status` | Show position, duration, and volume |
| `quit` / `q` | Exit and release audio resources |

Playback ends automatically at EOF. Decoder/output failures report an error and exit
nonzero. No audio device is needed for `probe` or `library`. Playback uses FFmpeg to
decode/resample into a small SDL2 output queue. Volume changes affect
newly queued samples (normally within about 120 ms, plus device latency).

## Play a library queue

Use an NDJSON export from the indexer, rather than entering individual audio hashes:

```bash
./bin/osu-player queue artifacts/library.ndjson
```

To create that export from a source build (close lazer first):

```bash
make indexer
mkdir -p artifacts
.tools/dotnet10/dotnet tools/osu-indexer/bin/Release/net10.0/osu-indexer.dll \
  --osu-dir "$HOME/.local/share/osu" > artifacts/library.ndjson.tmp &&
  mv artifacts/library.ndjson.tmp artifacts/library.ndjson
```

Use `dotnet` instead of `.tools/dotnet10/dotnet` with a system SDK. The temporary-file/rename
pattern keeps a failed indexing attempt from replacing a usable export. These are local
metadata files; `artifacts/` is gitignored. `queue` accepts a file, not `-`: stdin is reserved
for interactive controls.

The existing transport commands also support:

| Command | Action |
|---|---|
| `next` / `n` | Start the next queue entry |
| `previous` / `prev` | Start the previous entry (restart at the head) |
| `list [SEARCH]` | Print up to 20 matching tracks with their library indices |
| `track 7` | Play library entry 7 (1-based; indices do not change with shuffle) |
| `shuffle on` / `shuffle off` | Change queue ordering without interrupting the current song |
| `repeat off` / `repeat one` / `repeat all` | Stop at end, repeat current on EOF, or cycle the queue |

Queue behavior:

- EOF advances automatically. The CLI exits when the queue ends with repeat off.
- Repeat-one applies only to automatic EOF, not manual next/previous.
- Manual navigation starts playback, even from a paused/stopped selection.
- Enabling shuffle preserves played history/current song and randomizes the remaining
  suffix. Disabling it restores sorted library order. Repeat-all reshuffles at a forward
  cycle boundary and avoids an immediate duplicate when there are multiple entries.
- Track failures are reported and skipped in the navigation direction. Failed entries
  are not retried automatically during the session, including across repeat-all cycles.
  Explicit selection retries that entry; restarting a failed/completed queue clears the
  failure history. An all-broken queue terminates instead of looping.
- Output-device failures stop immediately rather than attempting the same device for
  every song.

## Terminal UI

Open the default osu! directory, or use an exported library file:

```bash
./bin/osu-player tui
./bin/osu-player tui artifacts/library.ndjson
```

The UI starts **without autoplay**. It requires interactive stdin/stdout, an installed
`TERM` entry, and preferably a UTF-8 locale (for example `LANG=C.UTF-8`). The minimum usable
size is 50 columns × 14 rows; smaller terminals show a resize message while playback
continues. Monochrome terminals work without colors.

| Key | Action |
|---|---|
| `j` / `k`, Down / Up | Move selection |
| PageDown / PageUp, `g` / `G`, Home / End | Page or jump to the first/last result |
| Enter | Play selection |
| Space | Pause/resume; start selection if nothing is loaded |
| `n` / `p` | Next/previous in the playback queue |
| Left / Right | Seek backward/forward five seconds |
| `+` / `-` | Volume in 5% steps |
| `s` | Toggle shuffle |
| `R` | Cycle repeat off → one → all |
| `x` | Stop and rewind |
| `r` | Refresh in the background (reload the export, or rerun the indexer) |
| `d` | Toggle Discord Rich Presence; prompt for an Application ID if needed |
| `/` | Edit the search filter |
| Esc | Clear the filter outside search mode |
| `q` | Quit outside search mode |

Search updates results as you type. Enter applies the filter (press Enter again to play
the selection); Esc cancels the edit and restores the previous filter. Backspace removes
one UTF-8 codepoint, and Ctrl-U clears the query. Search is ASCII-case-insensitive and can
match Unicode text exactly. Filtering does **not** change the playback queue, interrupt
audio, or hide the now-playing information.

The UI stays open after queue completion or playback errors, so you can choose another
track or retry a device. Errors appear in a status line instead of overwriting the screen.
Complex emoji may not display perfectly. Quit, Ctrl-C, SIGTERM, and SIGHUP restore terminal
settings. Mouse support and indexing while lazer is running are not implemented.

### Saved preferences

The TUI automatically saves volume, shuffle, repeat mode, the Discord toggle, and the
last applied search filter to `$XDG_CONFIG_HOME/osu-player/settings.json`, or
`$HOME/.config/osu-player/settings.json` when the XDG path is unset, empty, or relative.
Preferences are shared across libraries and builds (development, AppImage, native package).
Playback never starts automatically; the current track, position, and queue are not restored.
Search edits are saved only when applied with Enter; Esc cancels an edit.

Settings are stored separately from your osu! directory. Missing settings use defaults
(70% volume, shuffle/repeat off, empty search). Invalid or unsupported files
are preserved and defaults are used with an on-screen warning; persistence is disabled
for that session. Save errors are also shown without interrupting playback. Multiple
instances use last-writer-wins rather than merging preferences.

To reset preferences, close all player instances and remove the settings file. No library
cache or osu! content needs to be deleted. Only the TUI saves these preferences; CLI
playback commands keep their existing behavior.

### Optional Discord Rich Presence (development builds)

Discord presence is **off by default** and currently available only in the TUI.
Start the desktop Discord client, run the TUI, and press `d`. If no valid Application ID
is saved, an in-app prompt appears—no manual config editing is needed.

Create your own application in the [Discord Developer Portal](https://discord.com/developers/applications)
and enter its **Application ID**, not a bot token or client secret. Press Enter to
validate, save, and enable presence; Esc cancels without saving the draft. Backspace
edits and Ctrl-U clears the input. Invalid input stays in the prompt and never connects.
Playback continues while you edit. An already-enabled configuration with a missing or
invalid ID also prompts at startup. Cancelling leaves Discord off.

The Application ID and toggle are saved under `discord.application_id` and
`discord.enabled` in the normal settings file; later presses of `d` simply toggle
presence. Optionally upload a Rich Presence image named `osu_player` to your Discord
application. No project-wide application ID is bundled yet.

Presence shares the title, artist, playback timer, paused state, and an official beatmap
link when available. Local paths, hashes, and library/session IDs are not sent.
Updates run in a background local IPC worker, coalesced to at most one normal update
per five seconds; stopping clears activity promptly. Disabling or exiting attempts to
clear presence before disconnecting. Connection failures do not stop playback.

The title bar reports connection status. Enable activity sharing in Discord if needed.
Sandboxed Discord installations may need an accessible IPC socket; an absolute
`OSU_PLAYER_DISCORD_SOCKET` path overrides normal discovery. Fake-socket tests pass,
but live Discord rendering has not been verified. Existing 0.2.0 packages do not contain
this feature. Last.fm authorization and scrobbling are not implemented yet.

### Background indexing and cache

You can now browse directly from a lazer data directory instead of exporting by hand.
**Close lazer before starting this mode or pressing `r`.** Background indexing means the
player stays responsive; it does **not** make copying a live Realm database safe.

```bash
make indexer
./bin/osu-player tui
# For slow storage / larger libraries:
./bin/osu-player tui --osu-dir /path/to/osu-data --index-timeout 900
# Bypass all cache reads/writes:
./bin/osu-player tui --osu-dir /path/to/osu-data --no-cache
```

Without `--osu-dir`, `tui` uses `$HOME/.local/share/osu`. An explicit `--osu-dir`
overrides this; `tui FILE.ndjson` still opens an exported library instead.

Source builds require `make indexer` before using directory mode; packages include the
adapter. To override it, pass `--indexer /absolute/path/to/osu-indexer` (an executable
or wrapper accepting `--osu-dir DIRECTORY`, not a shell command string or a DLL).
No build/restore runs inside the UI. `--index-timeout` accepts 1–86400 seconds, default 300.

Cache files live under `$XDG_CACHE_HOME/osu-music-player/`, or
`$HOME/.cache/osu-music-player/` if XDG_CACHE_HOME is unset/relative. Each canonical osu!
directory gets a separate private cache. An unchanged Realm can reuse the cache.
A stale cache is shown while indexing runs. A missing database or failed refresh leaves
the last usable library/cache intact. Corrupt/truncated caches trigger reindexing.

Pressing `r` always forces a refresh, even with an unchanged Realm. Only one refresh runs
at a time. For `tui FILE`, `r` simply reloads that export asynchronously, without an adapter
or cache. An empty export replaces the displayed library with an empty one.
Warnings and failures appear in the status line; cache-write errors do not prevent using
a successfully indexed library.

Refresh preserves search, selection where possible, playback preferences, and surviving
queue entries. An unchanged current track keeps playing without interruption. If it was
removed or replaced, playback pauses without autoplay. Repaired tracks can be retried.

The cache does not watch imports or hash the audio tree; use `r` after closing lazer
when content-store changes matter. Cache/temp locations inside the osu! directory are
rejected. Refresh timeouts and quitting clean up the indexer and its temporary files.

## Library safety

**Close lazer before indexing.** The adapter takes an offline copy; copying a database
while the game writes to it is not a supported live snapshot. The game is never launched
by this project.

The adapter opens only its private temporary copy with Realm, in dynamic read-only mode.
It never migrates the database or writes Realm locks/sidecars in the source directory.
Missing audio and broken filename mappings emit warning records. An incompatible schema
or fatal error exits nonzero, writes diagnostics to stderr, and emits no partial library.
The game version is the only setting read from `game.ini`; credentials are not exported.
Duration is left unknown until the actual audio is probed (beatmap gameplay length is not
necessarily song duration). Temporary copies are removed after use.

The implementation writes only its own caches and temporary files. It does not write to
`client.realm` or lazer's content store.
