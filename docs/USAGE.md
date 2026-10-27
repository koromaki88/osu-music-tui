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

Source builds need `make indexer` first; packages include the adapter. No build/restore
runs inside the UI. For a custom adapter, pass `--indexer /absolute/path/to/executable`;
it must accept `--osu-dir DIRECTORY`, not be a DLL or shell command string. Indexing
timeout accepts 1–86400 seconds.

## Terminal controls

The UI starts without autoplay. Use an interactive terminal with a valid `TERM`
entry and preferably UTF-8, such as `LANG=C.UTF-8`. Monochrome terminals work.
The minimum size is 50 columns × 14 rows; smaller terminals show a resize message
without stopping audio.

Mouse input is unsupported, and complex emoji may render imperfectly. Quit and
handled signals restore terminal settings.

| Key | Action |
|---|---|
| `j` / `k`, Down / Up | Move selection |
| PageDown / PageUp | Page through results |
| `g` / `G`, Home / End | First/last result |
| `J` (uppercase) | Select and reveal the current track; if hidden, switch to full library and clear search |
| Enter | Play selection |
| Space | Pause/resume; start selection if nothing is loaded |
| `n` / `p` | Next/previous in queue |
| Left / Right, `h` / `l` | Seek backward/forward five seconds |
| `+` / `-` | Volume in 5% steps |
| `m` | Mute/unmute without changing the volume setting |
| `s` | Toggle shuffle |
| `R` | Cycle repeat off → one → all |
| `x` | Stop and rewind |
| `r` | Refresh in background |
| `d` | Toggle/setup Discord |
| `L` (uppercase) | Open/setup Last.fm |
| `O` (uppercase) | Toggle optional OBS browser overlay on loopback port 7270 |
| `?` / `i` | Open help / selected-track details |
| `f` / `F` | Toggle selected favorite / favorites-library view |
| `P` / `b` | Open playlists / browse full library |
| `u`, `<` / `>` | In a playlist: remove selection, move earlier/later |
| `/` | Edit search |
| Esc | Clear filter outside search mode |
| `q` | Quit outside search mode |

### Mute

Mute is session-only and does not pause playback, reset the track, or change the
saved volume. While muted, `+`/`-` adjust the chosen volume but output stays muted;
unmuting uses that level. The status line shows `(muted)` beside the percentage.
Like volume changes, mute affects newly queued audio (roughly 120 ms output buffering).
It also works in help/details/playlists panels, but not while entering text or
confirming deletion.

### Search

Search filters as you type. Enter applies the filter; press Enter again to play.
Esc cancels the edit, Backspace removes one UTF-8 codepoint, and Ctrl-U clears the query.
Matching is ASCII-case-insensitive and supports exact Unicode text. Filtering does
not change the playback queue or interrupt audio.

Outside panels and text entry, `J` reveals the current track without restarting it
or changing its queue, shuffle, repeat, or pause state. A visible track keeps the
current list/filter; a hidden track switches the browser to the unfiltered library.
With no current track, it only shows a notice.

### Help and track details

Press `?` for controls and integration guidance, or `i` for the **selected library
track**, which may differ from the playing track. Details show title/artist, original
metadata, mapper, known duration, BPM, date added, official beatmap link, local audio
path, and the most recently recorded playback error if it belongs to that track.
Unknown values are labeled. If duration is not already known, a background worker
probes only that audio file; loading/failure status appears without starting playback.
The panel does not index the library or open links.

BPM comes from the representative indexed beatmap, not audio beat detection; added
is the beatmap set's import date in UTC. Close lazer and press `r` after updating the
source-build indexer to populate these fields. Older exports must be regenerated;
missing source fields remain unknown. Refresh does not scan all audio for durations.

Panels replace the library area, leaving playback/status visible below. They use
the available width on small terminals and a centered width of up to 96 cells on
large ones. Unicode text wraps by terminal cells and reflows on resize; scroll
position is clamped to the available content. Below 50×14, resize to restore the panel.

- **Scroll:** `j`/`k`, Up/Down, PageUp/PageDown, `g`/`G`, or Home/End.
- **Playback:** Space, `n`/`p`, Left/Right or `h`/`l`, and `+`/`-` remain active. Other library actions,
  including Enter-to-play, are blocked while a panel is open.
- **Close:** Esc, `q`, or the panel's opening key. `?`/`i` can switch between panels.
  Outside panels/search, `q` quits as usual.

Details are an owned snapshot at opening, so refresh/removal cannot invalidate them.
Duration fills in when the probe finishes; reopen to update other metadata/errors.
Panel state is not saved. Search and
credential input take precedence: `?`/`i` typed there do not open panels. Text is
sanitized and bounded, including local paths; unusually long fields may be truncated.

### Queue behavior

- **EOF:** advances automatically. Repeat-one affects EOF, not manual next/previous.
- **Navigation:** manual next/previous starts playback even from pause/stop.
  `n`/`p` follow the active queue, including shuffled order/history, not the filtered
  library list. Moving forward again after previous retraces that queue without reshuffling.
- **Shuffle:** preserves played history and the current track. Repeat-all reshuffles
  at a forward cycle boundary, avoiding an immediate duplicate when possible.
- **Failed tracks:** skipped without automatic retries during that session;
  explicit selection retries them.
- **Device failures:** stop playback. The TUI stays open after errors or queue completion.

## Favorites and playlists

Press `f` to toggle the selected track's favorite marker, or `F` to browse favorites.
Press `F` again or `b` to return to the full library. Search still applies within
the current view; changing views never interrupts the playing track.

Press uppercase `P` to manage named playlists. The panel captures the library
selection as its add target; close it and select another track to change that target.
Playlist names are case-sensitive, can contain Unicode, and are limited to 128 bytes.

| Key in playlists panel | Action |
|---|---|
| `j` / `k`, Up / Down | Select playlist |
| PageUp / PageDown, `g` / `G`, Home / End | Page or jump through playlists |
| `c` | Create a playlist; type its name and press Enter |
| `a` | Add the captured track to the selected playlist (duplicates ignored) |
| Enter | Browse the selected playlist without starting playback |
| `r` | Rename; Ctrl-U clears, Enter saves, Esc cancels |
| `x` | Delete selected playlist; `y` confirms, any other key cancels |
| `b` | Close panel and browse the full library |
| Esc / `q` / `P` | Close panel |

In a playlist's track browser, `u` removes the selected member; `<`/`>` moves it
earlier/later. New tracks append in insertion order. The playlists panel reflows
on resize and keeps its selected list visible, including at 50×14. Name editing
captures ordinary keys rather than triggering playback shortcuts.

### List playback

Press Enter on a track to start its **entire list's queue**, independent of the
search filter. Next/previous, shuffle, and repeat stay inside that queue. Shuffle
off restores the playlist's saved order, not global library order. Favorites use
the order in which tracks were favorited.

Browsing, renaming, deleting, or editing a saved list does not mutate the active
queue. That queue is a snapshot until Enter starts a list again; its name appears
in the playback status. Space resumes the active queue even when browsing elsewhere.
Refresh retains surviving queued tracks and never adds unrelated tracks to a list
queue. Missing/changed tracks stay saved but are omitted when resolving a list;
restoring the same stable ID and audio hash makes them available again.

### Storage and recovery

Lists live in `~/.config/osu-player/collections/lists.json` (or under an absolute
`$XDG_CONFIG_HOME`). They are separate from preferences, Last.fm, and lazer's files.
Only track IDs/hashes, membership/order, and playlist names are stored—no audio paths
or credentials. This is private plaintext, not encrypted storage.

An owned background worker loads/saves the file. Edits become visible after an
atomic save succeeds; failures appear in the status line without replacing the
previous list. Normal exit flushes accepted edits; abrupt termination can lose edits
not yet saved. Directory/file modes are 0700/0600; unsafe types, links, malformed/newer
files, and detected external edits are preserved rather than overwritten. Only one
player instance can own collections; other instances keep ordinary playback usable
but report collections unavailable. Restart after fixing storage or external edits.

Limits are 128 playlists, 10,000 favorites, 10,000 members per playlist, 50,000 total
references, an 8 MiB file, and 64 pending edits. Capacity failures leave saved data
intact. No view, queue, or autoplay state is restored on launch—only the saved lists.

## OBS browser overlay

Press uppercase `O` in the normal library browser to enable the optional overlay.
It is **off by default**; enablement is remembered in preferences, but playback
and track state are never restored. The header shows `OBS: on` while enabled.
Close help/details/playlists or finish text entry before toggling it. `O` is not
added to the basic keybind footer; it is listed in `?` help.

In OBS, add a **Browser Source**, leave **Local file** unchecked, and set:

- **URL:** `http://127.0.0.1:7270/overlay`
- **Width / height:** start with **800 × 180** and adjust as desired.
- **Frame rate:** 15 FPS is sufficient for this non-animated display.

By default the card fills the browser/source width with a 180 px minimum height.
The page has a transparent canvas with a dark translucent card, Unicode title and
artist, playback time and pause state. The progress bar is hidden when duration is
unknown; use `http://127.0.0.1:7270/overlay?progress=0` to hide it always.
When available, the current beatmap-set cover appears as a square on the left of
the text. Missing/failed covers hide automatically. No external fonts, remote scripts,
GUI toolkit or extra application downloads are needed.
Browser-engine rendering is provided by OBS, not by the player.

The page polls twice per second without animations. Stopping/completing playback
shows “Nothing playing”; disabling or exiting the player shows “Player disconnected”.
It reconnects automatically when the player is available again. Restarting does not
autoplay. Browser-source graphics have not been visually verified in OBS here.

### Customize with overlay.json

The first successful enable creates `~/.config/osu-player/overlay.json` (or
`$XDG_CONFIG_HOME/osu-player/overlay.json` for an absolute XDG config directory).
Edit that file in any text editor:

```json
{
  "width": null,
  "height": 180,
  "font": "Noto Sans",
  "title_size": 32,
  "artist_size": 23,
  "background": "#101218",
  "background_opacity": 0.85,
  "text_color": "#ffffff",
  "accent_color": "#ff78ae",
  "corner_radius": 14,
  "show_progress": true,
  "show_time": true,
  "show_thumbnail": true,
  "thumbnail_size": 112
}
```

Edits apply automatically within a few seconds to already-open browser/OBS sources,
without restarting the player or refreshing OBS. Configuration checks run on the
overlay worker once per second while enabled; the page refreshes style every two
seconds. Invalid/partially saved edits preserve the entire last valid style and show
`OBS: style error`. Fix the file to recover automatically. On first load, invalid
config uses defaults. Existing files are never rewritten, even if malformed;
deleting the file restores defaults and the next enable recreates it. Missing keys
use defaults, not values from an earlier edit. An empty object `{}` restores defaults.

| Setting | Meaning / accepted values |
|---|---|
| `width` | `null` fills the source viewport (default); integer 200–3840 caps card width in px |
| `height` | Minimum card height, 0–2160 px; 0 fits content |
| `font` | Installed font family, up to 128 characters: letters, digits, spaces, hyphens, underscores |
| `title_size` / `artist_size` | Font size: 8–128 / 8–96 px |
| `background`, `text_color`, `accent_color` | Six-digit `#RRGGBB`; accent applies to progress |
| `background_opacity` | 0–1; 0 removes the card background |
| `corner_radius` | 0–128 px; 0 gives square corners |
| `show_progress`, `show_time` | JSON `true` or `false` |
| `show_thumbnail` | `true` by default; `false` hides artwork and suppresses cover URLs |
| `thumbnail_size` | Square side length, 32–256 px (default 112); cropped with object-fit cover |

The file must be an owned regular file of at most 8 KiB (no symlink/FIFO); unknown
options and wrong types are rejected. No raw CSS, scripts or external resource URLs
are accepted. Font names refer to fonts installed where OBS/browser runs.

**OBS sizing:** this config styles the page; it cannot change OBS's Browser Source
canvas dimensions. Changing that source's Width/Height changes the viewport and
layout. Resizing/transforming the source in the scene scales the rendered card as
a whole. `width: null` restores fluid full-width behavior; a numeric value is only
an optional cap. Keep the source tall enough to avoid clipping large text. You do
not need Custom CSS, but existing OBS CSS with `!important` can override config styles.

The fixed port **7270** binds to IPv4 loopback only; OBS must run on the same machine
and network namespace. Use the exact `127.0.0.1` URL, not `localhost` or a LAN address.
If the port is occupied, the overlay reports an error without interrupting playback;
free it, then toggle `O` off/on to retry. Two player instances cannot both own it.

**Privacy:** enabling shares display metadata and timing with local clients, including
other local users. There is no authentication. Cross-origin browser reads are blocked,
but this is not an isolation boundary against other local applications. No paths,
filenames, hashes, track/session IDs, credentials, audio, or library search are served.
With thumbnails enabled, the overlay also shares a public cover URL derived from
the online beatmap-set ID. OBS/browser fetches it directly over HTTPS from
`assets.ppy.sh` without a referrer; this reveals your IP and requested set to that
service. No local files are uploaded. Set `show_thumbnail` to `false` to avoid
these image requests (an already in-flight request may complete). Offline/local-only
tracks have no cover; missing artwork never blocks playback.
OBS recordings/streams may expose your listening activity. Press `O` again to close
the listener and connected clients; no server is started when disabled by default.

## Refresh and cache

After imports/removals, close lazer and press `r`. There is no automatic library
watcher. Integration setup does not require reindexing.

### What refresh preserves

Directory mode runs the adapter; export mode reloads the file. Only one refresh runs
at a time, and the stale library stays usable during indexing. A failed refresh
retains the last usable library/cache.

Unchanged current tracks keep playing; removed/replaced tracks pause without autoplay.
Search, surviving selection/queue entries, and preferences are retained where possible.

### Cache rules

Caches live in `$XDG_CACHE_HOME/osu-music-player/`, defaulting to
`~/.cache/osu-music-player/` for unset/relative XDG paths. Each canonical source
directory gets a private cache. Unchanged Realm metadata can reuse it; corrupt caches
trigger reindexing. Pressing `r` forces refresh even if Realm is unchanged.

Cache failures are reported without preventing use of a successfully indexed library.
Cache/temp locations inside the source library are rejected. Quit/timeouts clean up
adapter processes and temporary copies.

## Preferences and private data

| Data | Default location |
|---|---|
| Preferences | `~/.config/osu-player/settings.json` |
| Last.fm credentials | `~/.config/osu-player/lastfm/credentials.json` |
| Last.fm listening outbox | `~/.config/osu-player/lastfm/scrobbles.json` |
| Favorites and playlists | `~/.config/osu-player/collections/lists.json` |
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

Keep the extracted directory together. The AppImage itself can otherwise be
moved/renamed without its source tree. glibc and the dynamic loader are host-provided;
Alpine/musl, Windows, and macOS are unsupported.

Use `sha256sum -c` on adjacent checksum files. Checksums detect corruption but are
not publisher signatures.

## CLI commands

```bash
osu-player probe HASH [/path/to/osu-data]
osu-player play HASH [/path/to/osu-data]
osu-player library artifacts/library.ndjson [SEARCH]
osu-player queue artifacts/library.ndjson
```

`HASH` is a content-store audio SHA-256. `probe`/`library` need no audio device.
`library -` accepts NDJSON on stdin; `queue` needs a file because stdin is reserved
for controls. CLI playback does not publish Discord/Last.fm updates or save preferences.

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
