# Usage

[README](../README.md) · [Discord and Last.fm](INTEGRATIONS.md)

Examples use `osu-player`; substitute `./osu-player-x86_64.AppImage` for an AppImage
or `./bin/osu-player` for a source build.

## Open a library

> [!IMPORTANT]
> Close lazer before indexing or refreshing. The player uses an offline database
> copy; live Realm snapshots are unsupported. It never launches the game or writes
> to the original database/content store.

```bash
osu-player tui                                      # ~/.local/share/osu
osu-player tui --osu-dir /path/to/osu-data
osu-player tui --index-timeout 900                    # default: 300 seconds
osu-player tui --no-cache
osu-player tui artifacts/library.ndjson              # existing export
```

- **Source builds:** run `make indexer` first; packages include the adapter.
- **Custom adapter:** pass `--indexer /absolute/path/to/executable`. It must accept
  `--osu-dir DIRECTORY`, not be a DLL or shell command string.
- **Builds:** no build/restore runs inside the UI.
- **Timeout:** accepts 1–86400 seconds.

## Terminal controls

- **Startup:** no autoplay.
- **Terminal:** interactive, with a valid `TERM` entry; UTF-8 such as `LANG=C.UTF-8`
  is recommended. Monochrome terminals work.
- **Size:** at least 50 columns × 14 rows. Smaller terminals show a resize message
  without stopping audio.
- **Limitations:** no mouse input; complex emoji may render imperfectly.
- **Cleanup:** quit and handled signals restore terminal settings.

| Key | Action |
|---|---|
| `j` / `k`, Down / Up | Move selection |
| PageDown / PageUp | Page through results |
| `g` / `G`, Home / End | First/last result |
| Enter | Play selection |
| Space | Pause/resume; start selection if nothing is loaded |
| `n` / `p` | Next/previous in queue |
| Left / Right | Seek backward/forward five seconds |
| `+` / `-` | Volume in 5% steps |
| `s` | Toggle shuffle |
| `R` | Cycle repeat off → one → all |
| `x` | Stop and rewind |
| `r` | Refresh in background |
| `d` | Toggle/setup Discord |
| `l` | Open/setup Last.fm |
| `/` | Edit search |
| Esc | Clear filter outside search mode |
| `q` | Quit outside search mode |

### Search

- Filters update as you type; Enter applies the filter, then Enter again plays.
- Esc cancels the edit; Backspace removes one UTF-8 codepoint; Ctrl-U clears the query.
- Matching is ASCII-case-insensitive and supports exact Unicode text.
- Filtering does not change the playback queue or interrupt audio.

### Queue behavior

- **EOF:** advances automatically. Repeat-one affects EOF, not manual next/previous.
- **Navigation:** manual next/previous starts playback even from pause/stop.
- **Shuffle:** preserves played history and the current track. Repeat-all reshuffles
  at a forward cycle boundary, avoiding an immediate duplicate when possible.
- **Failed tracks:** skipped without automatic retries during that session;
  explicit selection retries them.
- **Device failures:** stop playback. The TUI stays open after errors or queue completion.

## Refresh and cache

After imports/removals, close lazer and press `r`. There is no automatic library
watcher. Integration setup does not require reindexing.

### What refresh preserves

- **Source:** directory mode runs the adapter; export mode reloads the file.
- **Availability:** one refresh at a time; the stale library stays usable during indexing.
- **Failure recovery:** failed refreshes retain the last usable library/cache.
- **Playback:** unchanged current tracks keep playing; removed/replaced tracks pause
  without autoplay.
- **UI state:** search, surviving selection/queue entries, and preferences are retained
  where possible.

### Cache rules

- **Location:** `$XDG_CACHE_HOME/osu-music-player/`, defaulting to
  `~/.cache/osu-music-player/` for unset/relative XDG paths. Each canonical source
  directory gets a private cache.
- **Reuse:** unchanged Realm metadata can reuse the cache; corrupt caches trigger
  reindexing. `r` forces refresh even if Realm is unchanged.
- **Failures:** reported without preventing use of a successfully indexed library.
- **Safety:** cache/temp locations inside the source library are rejected.
- **Cleanup:** quit/timeouts clean up adapter processes and temporary copies.

## Preferences and private data

| Data | Default location |
|---|---|
| Preferences | `~/.config/osu-player/settings.json` |
| Last.fm credentials | `~/.config/osu-player/lastfm/credentials.json` |
| Last.fm listening outbox | `~/.config/osu-player/lastfm/scrobbles.json` |
| Library cache | `~/.cache/osu-music-player/` |

Config files use an absolute `$XDG_CONFIG_HOME` when set.

- **Saved across libraries/builds:** volume, shuffle, repeat, integration enablement,
  Discord Application ID, and applied search.
- **Never restored:** track, position, or queue.
- **Defaults:** 70% volume, shuffle/repeat/integrations off, empty search.
- **Scope:** only the TUI saves preferences; multiple instances are last-writer-wins.

> [!NOTE]
> Malformed/newer settings are preserved with a warning; persistence is disabled for
> that session. To reset preferences, close all instances and remove only
> `settings.json`—no library/cache deletion is needed.

See [integration privacy](INTEGRATIONS.md#privacy-and-storage) before handling
credentials or listening-history files.

## AppImage without FUSE

```bash
./osu-player-x86_64.AppImage --appimage-extract-and-run tui
# Or extract once to avoid decompression on every launch:
./osu-player-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun tui
```

- **Relocation:** keep the extracted directory together. The AppImage itself can
  otherwise be moved/renamed without its source tree.
- **Host requirements:** glibc and the dynamic loader are host-provided. Alpine/musl,
  Windows, and macOS are unsupported.
- **Verification:** use `sha256sum -c` on adjacent checksum files. Checksums detect
  corruption but are not publisher signatures.

## CLI commands

```bash
osu-player probe HASH [/path/to/osu-data]
osu-player play HASH [/path/to/osu-data]
osu-player library artifacts/library.ndjson [SEARCH]
osu-player queue artifacts/library.ndjson
```

- **Hash:** `HASH` is a content-store audio SHA-256.
- **Audio device:** not needed for `probe`/`library`.
- **Input:** `library -` accepts NDJSON on stdin; `queue` needs a file because stdin
  is reserved for controls.
- **Integrations/settings:** CLI playback does not publish Discord/Last.fm updates
  or save preferences.

Type commands followed by Enter:

| Command | Action |
|---|---|
| `pause` / `p`, `resume` / `r` | Pause/resume |
| `stop` | Pause and rewind |
| `seek 45.5` | Seek to seconds |
| `volume 0.5` | Set volume (0–1) |
| `status` | Position, duration, volume |
| `quit` / `q` | Exit |
| `next` / `n`, `previous` / `prev` | Queue navigation |
| `list [SEARCH]` | Up to 20 matching library indices |
| `track 7` | Play library entry (1-based, unaffected by shuffle) |
| `shuffle on` / `shuffle off` | Queue ordering |
| `repeat off` / `repeat one` / `repeat all` | Repeat mode |

Single-track playback ends at EOF; queue mode exits at the end with repeat off.
Decoder/output errors are reported; all-broken queues terminate instead of looping.

### Export a library

Close lazer first and keep build logs out of NDJSON:

```bash
make indexer
mkdir -p artifacts
.tools/dotnet10/dotnet tools/osu-indexer/bin/Release/net10.0/osu-indexer.dll \
  --osu-dir "$HOME/.local/share/osu" > artifacts/library.ndjson.tmp &&
  mv artifacts/library.ndjson.tmp artifacts/library.ndjson
```

Use `dotnet` instead of the private SDK path with a system SDK. The temporary file
prevents a failed export from replacing a usable one.
