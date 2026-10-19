# Discord and Last.fm

[README](../README.md) · [Usage](USAGE.md)

Both integrations are optional, off by default, independent, and available only in
the TUI. Setup/network failures do not stop playback. Settings persist, but playback
never resumes automatically.

## Discord Rich Presence

1. Start the desktop Discord client and enable activity sharing.
2. Create an application in the [Developer Portal](https://discord.com/developers/applications).
3. Press `d` in the player and enter its **Application ID**. No bot token, client
   secret, or OAuth is needed. Enter saves/enables; Esc cancels.

Later presses of `d` toggle presence; the ID/toggle live in ordinary preferences.
Optionally upload an image named `osu_player` to your Discord application.

### What appears on Discord

Presence shows the title, artist, paused state/timer, `osu! music player` branding,
and an official beatmap button when available. Online beatmapsets use public osu!
thumbnails; otherwise the application image is used. Deleted/unavailable covers may
not display.

Discord fetches/proxies images from `assets.ppy.sh`; the player does not download
images or share local backgrounds.

### Connection and updates

The title bar shows connection status. Sandboxed clients may need an accessible
socket; `OSU_PLAYER_DISCORD_SOCKET=/absolute/path` overrides normal IPC discovery.
Updates are coalesced, normally at most once per five seconds. Stop clears promptly;
disable/exit makes a best-effort clear. Abrupt termination cannot guarantee it.

## Last.fm

### Authorize

1. Get your own [application API key and shared secret](https://www.last.fm/api/account/create).
   Never share the secret in chat or logs.
2. Press `l`, then Enter to enter your API key and masked shared secret.
3. Open the displayed authorization URL in a browser and approve access. Return to
   the player and press Enter to finish. Never enter your Last.fm password in the player.

Esc closes/cancels the panel while playback continues. Existing authorization remains
usable after upgrading; integration setup does not require a library refresh.

| Key in Last.fm panel | Action |
|---|---|
| `c` | Replace application credentials |
| `d` | Disable, retaining authorization |
| `x` | Disconnect locally by removing the saved session |

> [!NOTE]
> Disable/disconnect stops submissions but retains queued plays. They resume only
> when the same account is authorized and enabled. Revoke server-side access in
> your Last.fm account settings if needed.

### Listening and delivery

**Now Playing**

Now Playing sends artist/title and known audio duration once per playback session;
failed/ignored updates are not retried. Pause/stop prevents new updates but cannot
clear one already accepted; it expires on the service.

**Scrobble eligibility**

- Known audio duration **longer than 30 seconds**.
- Credited listening of **half its duration or four minutes**, whichever is sooner.
- Pauses and seeked-over audio do not count; each replay/repeat is a separate session.
- Disabled/unauthorized plays and missing history are not submitted retrospectively.

**Outbox and retries**

Eligible plays are saved **before transmission**, retaining their original
metadata/start timestamp and authorized account. The outbox holds **1,024 pending
plays**, delivered oldest first per account. Accounts never inherit each other's backlog.

Temporary failures back off from **30 seconds to 30 minutes** across restarts;
clock rollback cannot indefinitely extend the wait. Permanent rejections/valid ignored
replies are not retried, and revoked sessions require reauthorization.

The panel shows pending plays, unsaved-play warnings, and submission results. A
full/unsafe outbox retains old entries and counts new unsaved plays; only one player
can own it at a time. Fix storage problems, then disable/re-enable to retry.
Malformed/newer files are preserved, not automatically repaired or deleted.

> [!WARNING]
> Delivery is **not exactly-once**. Receipts suppress ordinary duplicates, but a lost
> reply or crash after server acceptance can cause a duplicate on retry. Abrupt
> termination can lose unpersisted eligibility events; normal shutdown flushes
> accepted worker events.

### Troubleshooting

The panel distinguishes `Credentials:`, `Signing:`, `HTTPS:`, `Response:`, and numeric
Last.fm API errors.

| Curl code | Meaning |
|---|---|
| 6 | DNS failure |
| 28 | Timeout |
| 60 | Certificate verification failure |

**Report only status text**, never credentials or authorization URLs.

`SSL_CERT_FILE` selects a trusted CA bundle. AppImage prefers current host roots,
with a bundled fallback; update/rebuild the AppImage to refresh that fallback.
HTTPS verification is always enabled—never disable it.

## Privacy and storage

### Local files

Credentials live in `$XDG_CONFIG_HOME/osu-player/lastfm/credentials.json`, defaulting
to `~/.config/osu-player/lastfm/credentials.json`. The directory is private (0700),
files are owner-only (0600), and unsafe owners/types/modes, symlinks, and hard links
are refused. Unsafe/malformed/newer credentials are not overwritten.

Adjacent `scrobbles.json` contains listening history/local playback-session identifiers,
but no credentials, paths, hashes, or library IDs. Receipts are bounded to 4,096 entries.

> [!WARNING]
> Credentials and listening history are **plaintext**, not encrypted or keyring-backed.
> Protect your account, backups, and terminal recordings.

### Shared data

| Service | Sent | Not sent |
|---|---|---|
| Discord | Structured song metadata, optional public beatmap links | Local paths, hashes, library/session IDs |
| Last.fm | Song metadata/duration, scrobble timestamps | Local paths, hashes, library/beatmap IDs, invented album metadata |

No project API credentials or Discord Application ID are bundled.
