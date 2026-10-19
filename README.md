# osu! music player

A terminal music player for an existing osu!(lazer) library. Browse and search your
music, stream playback, shuffle, repeat, and optionally connect Discord or Last.fm.

**Close lazer before indexing or refreshing.** The player reads your library without
modifying the original database or content files. Playback never starts automatically.

## Run

### AppImage

```bash
chmod +x osu-player-x86_64.AppImage
./osu-player-x86_64.AppImage tui
```

Requires Linux x86-64, glibc 2.36+, a UTF-8 locale, and working host audio. No separate
.NET installation is needed. Without FUSE, use `--appimage-extract-and-run tui`.

### Arch Linux

```bash
sudo pacman -U artifacts/arch/osu-music-player-0.4.0-1-x86_64.pkg.tar.zst
osu-player tui
```

The native package uses system libraries and the .NET 10 runtime. Use an up-to-date
Arch system. Current packages are local, unsigned builds, not published downloads;
public distribution is pending licensing review.

The default library is `~/.local/share/osu`. For another location:

```bash
osu-player tui --osu-dir /path/to/osu-data
```

## Main controls

| Key | Action |
|---|---|
| Up / Down, `k` / `j` | Select a track |
| Enter | Play selection |
| Space | Pause/resume |
| `n` / `p` | Next/previous |
| Left / Right | Seek five seconds |
| `+` / `-` | Adjust volume |
| `/` | Search |
| `s` / `R` | Toggle shuffle / cycle repeat |
| `r` | Refresh library (close lazer first) |
| `d` / `l` | Discord / Last.fm setup |
| `q` | Quit |

## Build from source

Install a C++20 compiler, Make, pkg-config, FFmpeg development libraries, SDL2,
ncursesw, json-c, libcurl 7.85+, OpenSSL libcrypto, and CA certificates. Directory
indexing also needs the .NET 10 SDK. Tests require Python 3.11+, the OpenSSL CLI,
and FFmpeg with MP3/Vorbis encoders.

```bash
make
make indexer
make test
./bin/osu-player tui
```

To install a private SDK: `python3 tools/install_dotnet.py`. Make prefers
`.tools/dotnet10/dotnet`; override with `DOTNET=/path/to/dotnet`.

## Documentation

- [Usage](docs/USAGE.md): controls, library refresh, saved data, and CLI commands.
- [Integrations](docs/INTEGRATIONS.md): Discord and Last.fm setup, privacy, troubleshooting.
- [Development](DEVELOPMENT.md): workflow, tests, and release checklist.
- [Architecture](docs/ARCHITECTURE.md): indexing, playback events, and background workers.
- [Packaging](docs/PACKAGING.md): building and validating each format.
- [Release validation](docs/RELEASES.md) · [Changelog](CHANGELOG.md) · [Dependency notices](packaging/THIRD_PARTY.md)
