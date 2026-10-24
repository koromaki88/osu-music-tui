"""Exercise the real ncurses frontend with a PTY, generated audio, and dummy output."""

import argparse
import codecs
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import unicodedata
import wave


class Screen:
    """Small VT screen reader for assertions; handles ncurses' common output sequences."""

    def __init__(self, rows, columns):
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.mode = "normal"
        self.sequence = ""
        self.saved = (0, 0)
        self.last_character = " "
        self.resize(rows, columns)

    def resize(self, rows, columns):
        self.rows, self.columns = rows, columns
        self.cells = [[" "] * columns for _ in range(rows)]
        self.row = self.column = 0

    def put(self, character):
        if unicodedata.combining(character):
            return
        width = 2 if unicodedata.east_asian_width(character) in ("W", "F") else 1
        if self.column >= self.columns:
            self.row = min(self.rows - 1, self.row + 1)
            self.column = 0
        self.cells[self.row][self.column] = character
        if width == 2 and self.column + 1 < self.columns:
            self.cells[self.row][self.column + 1] = ""
        self.column += width
        self.last_character = character

    def control_sequence(self, final):
        if self.sequence.startswith(("?", ">", "=")):
            return  # Private terminal modes / device queries, not screen text.
        values = [int(value) if value.isdecimal() else 0 for value in self.sequence.split(";")]
        count = (values[0] or 1) if values else 1
        if final in ("H", "f"):
            self.row = max(0, min(self.rows - 1, (values[0] or 1) - 1))
            self.column = max(0, min(self.columns - 1, ((values[1] if len(values) > 1 else 1) or 1) - 1))
        elif final == "A":
            self.row = max(0, self.row - count)
        elif final == "B":
            self.row = min(self.rows - 1, self.row + count)
        elif final == "C":
            self.column = min(self.columns - 1, self.column + count)
        elif final == "D":
            self.column = max(0, self.column - count)
        elif final == "G":
            self.column = max(0, min(self.columns - 1, count - 1))
        elif final == "d":
            self.row = max(0, min(self.rows - 1, count - 1))
        elif final == "J":
            mode = values[0]
            if mode in (2, 3):
                self.cells = [[" "] * self.columns for _ in range(self.rows)]
            elif mode == 0:
                self.cells[self.row][self.column:] = [" "] * (self.columns - self.column)
                for row in range(self.row + 1, self.rows):
                    self.cells[row] = [" "] * self.columns
        elif final == "K":
            mode = values[0]
            start = 0 if mode in (1, 2) else self.column
            stop = self.columns if mode in (0, 2) else self.column + 1
            self.cells[self.row][start:stop] = [" "] * (stop - start)
        elif final == "X":
            stop = min(self.columns, self.column + count)
            self.cells[self.row][self.column:stop] = [" "] * (stop - self.column)
        elif final == "P":
            line = self.cells[self.row]
            del line[self.column:self.column + count]
            line.extend([" "] * (self.columns - len(line)))
        elif final == "@":
            line = self.cells[self.row]
            line[self.column:self.column] = [" "] * count
            del line[self.columns:]
        elif final == "b":
            for _ in range(count):
                self.put(self.last_character)

    def feed(self, data):
        for character in self.decoder.decode(data):
            if self.mode == "charset":
                self.mode = "normal"
            elif self.mode == "osc":
                if character == "\x07":
                    self.mode = "normal"
                elif character == "\x1b":
                    self.mode = "osc-escape"
            elif self.mode == "osc-escape":
                self.mode = "normal" if character == "\\" else "osc"
            elif self.mode == "escape":
                if character == "[":
                    self.sequence = ""
                    self.mode = "csi"
                elif character == "]":
                    self.mode = "osc"
                elif character in "()%":
                    self.mode = "charset"
                else:
                    if character == "7":
                        self.saved = (self.row, self.column)
                    elif character == "8":
                        self.row, self.column = self.saved
                    self.mode = "normal"
            elif self.mode == "csi":
                if "@" <= character <= "~":
                    self.control_sequence(character)
                    self.mode = "normal"
                else:
                    self.sequence += character
            elif character == "\x1b":
                self.mode = "escape"
            elif character == "\r":
                self.column = 0
            elif character == "\n":
                self.row = min(self.rows - 1, self.row + 1)
            elif character == "\b":
                self.column = max(0, self.column - 1)
            elif character == "\t":
                self.column = min(self.columns - 1, (self.column // 8 + 1) * 8)
            elif ord(character) >= 32:
                self.put(character)

    def text(self):
        return "\n".join("".join(row) for row in self.cells)


class Session:
    def __init__(self, executable, library=None, *, arguments=None, **environment):
        # Never read/write the developer's actual preferences, including callers
        # from other PTY integration suites. Explicit config paths test restarts.
        self.config = tempfile.TemporaryDirectory(prefix="osu-tui-config-")
        self.master, self.slave = pty.openpty()
        self.screen = Screen(24, 100)
        self.original_attributes = termios.tcgetattr(self.slave)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        self.process = subprocess.Popen(
            [executable, "tui", *(arguments if arguments is not None else [str(library)])],
            stdin=self.slave, stdout=self.slave,
            stderr=subprocess.PIPE, start_new_session=True,
            env=dict(os.environ, TERM="xterm-256color", LANG="C.UTF-8", LC_ALL="C.UTF-8",
                      SDL_AUDIODRIVER="dummy", XDG_CONFIG_HOME=self.config.name) | environment,
        )

    def drain(self, duration=0.05):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.master], [], [], min(0.02, max(0, deadline - time.monotonic())))
            if readable:
                self.screen.feed(os.read(self.master, 65536))

    def expect(self, *texts):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.drain()
            if all(text in self.screen.text() for text in texts):
                return
            if self.process.poll() is not None:
                break
        raise RuntimeError(f"TUI did not display {texts!r}; screen:\n{self.screen.text()}")

    def send(self, text):
        os.write(self.master, text.encode("utf-8"))
        self.drain(0.15)

    def resize(self, rows, columns):
        self.screen.resize(rows, columns)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.process.send_signal(signal.SIGWINCH)

    def finish(self, expected=0):
        deadline = time.monotonic() + 5
        while self.process.poll() is None and time.monotonic() < deadline:
            self.drain()
        if self.process.poll() is None:
            raise RuntimeError("TUI did not exit")
        self.drain()
        error = self.process.stderr.read().decode("utf-8", errors="replace")
        if self.process.returncode != expected:
            raise RuntimeError(f"unexpected TUI exit {self.process.returncode}: {error}")
        if error:
            raise RuntimeError(f"TUI leaked diagnostics over the screen: {error}")
        if termios.tcgetattr(self.slave) != self.original_attributes:
            raise RuntimeError("TUI failed to restore terminal attributes")

    def __enter__(self):
        return self

    def __exit__(self, *error):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.process.stderr.close()
        os.close(self.master)
        os.close(self.slave)
        self.config.cleanup()


def write_library(path, records):
    header = {"type": "header", "schema_version": 1}
    path.write_text("\n".join(json.dumps(record, ensure_ascii=False) for record in [header, *records]) + "\n",
                    encoding="utf-8")


def exercise_ui(executable, library):
    with Session(executable, library) as session:
        version = (Path(__file__).resolve().parent.parent / "VERSION").read_text().strip()
        session.expect(f"osu! music player v{version}", "Matches: 3", "State: stopped", "Alpha 日本")
        session.expect("Enter play  Space pause", "h/l seek  +/- volume")
        assert "Last.fm" not in "\n".join(session.screen.text().splitlines()[-2:]), "main footer should show basic controls only"
        session.send("j")
        session.expect(">   2")
        session.send("k")
        session.expect(">   1")
        session.send("\x1b[6~")  # PageDown
        session.expect(">   3")
        session.send("\x1bOH")   # Home
        session.expect(">   1")
        session.send("\n")
        session.expect("State: playing", "Now: Artist — Alpha 日本")
        session.send(" ")
        session.expect("State: paused")
        session.send("?")
        session.expect("Help", "BROWSE AND SEARCH", "State: paused")
        session.send("G")
        session.expect("PANELS", "q: close panel")
        session.resize(14, 50)
        session.expect("Help", "Esc/q close")
        session.send("g")
        session.expect("BROWSE AND SEARCH")
        session.resize(8, 30)
        session.expect("Terminal too small")
        session.resize(24, 100)
        session.expect("Help", "State: paused")
        session.send("q")  # Closes help, not the application.
        session.expect("Library (Enter plays")
        session.send("ji")  # Inspect selection, not the currently playing song.
        session.expect("Track details", "Title: Bêta", "Duration: 00:08", "State: paused")
        session.send("j\n")  # Panel input cannot select/play a different track.
        session.expect("State: paused", "Now: Artist — Alpha 日本")
        session.send("G")
        session.expect("select the track to retry.")
        session.resize(30, 160)
        session.send("g")
        session.expect("Track details", "Title: Bêta")
        session.send("\x1b")
        session.expect(">   2", "State: paused")
        session.resize(24, 100)
        session.send("k")
        session.send("\x1bOC")  # application-cursor Right
        session.expect("00:05")
        session.send("h")
        session.expect("00:00", "State: paused")
        session.send("l")
        session.expect("00:05", "State: paused")
        session.send("+sR")
        session.expect("Volume: 75%", "Shuffle: on", "Repeat: one")
        session.send("/zz_missing")
        session.expect("No matching tracks", "Matches: 0")
        session.send("?i")  # Literal search input, not panel shortcuts.
        session.expect("Search: zz_missing?i_")
        session.send("\x7f\x7f")
        session.send("q")  # Literal text in search mode, not quit.
        session.expect("zz_missingq")
        session.send("\x1b")
        session.expect("Matches: 3", "State: paused")
        session.send("/日本")
        session.expect("Matches: 1", "Search: 日本")
        session.send("\x7f")
        session.expect("Search: 日_")
        session.send("\x15ć")  # U+0107 must not be mistaken for KEY_BACKSPACE (263).
        session.expect("Search: ć_", "Matches: 0")
        session.send("\x15Bêta\n")
        session.expect("Search: Bêta", "Matches: 1", "State: paused")
        session.send("\n")
        session.expect("State: playing", "Now: Artist — Bêta")
        session.send("\x1b")  # Clear the committed filter outside search mode.
        session.expect("Matches: 3")
        session.send("sG\n")  # Disable shuffle so the broken entry is the queue tail.
        session.expect("State: failed", "Skipped track:")
        session.send("iG")
        session.expect("Track details", "Playback error:")
        session.send("\x1b")
        session.send("g\n")
        session.expect("State: playing", "Now: Artist — Alpha 日本")
        session.send("? ")
        session.expect("Help", "State: paused")
        session.send(" \x1b")
        session.expect("State: playing")
        session.resize(8, 30)
        session.expect("Terminal too small")
        session.resize(30, 120)
        session.expect("osu! music player", "Now: Artist — Alpha 日本")
        session.send("q")
        session.finish()


def exercise_queue_keys(executable, audio, root):
    library = root / "queue-controls.ndjson"
    write_library(library, [{"type": "track", "stable_id": str(index), "beatmap_set_id": str(index),
        "artist": "Controls", "title": f"Nav {index}", "audio_filename": "audio.wav",
        "audio_hash": hashlib.sha256(audio.read_bytes()).hexdigest(), "audio_path": str(audio)} for index in range(5)])
    with Session(executable, library) as session:
        session.expect("Library: 5", "State: stopped")
        session.send("\nn")
        session.expect("Now: Controls — Nav 1", "State: playing")
        session.send("p")
        session.expect("Now: Controls — Nav 0")
        session.send("sn")
        session.expect("Shuffle: on", "State: playing")
        def current():
            return re.search(r"Now: Controls — Nav ([0-4])", session.screen.text()).group(1)
        second = current()
        assert second != "0", "shuffle-next cannot repeat the current entry within a cycle"
        session.send("n")
        third = current()
        assert third not in ("0", second)
        session.send("p")
        session.expect(f"Now: Controls — Nav {second}")
        session.send("n")
        session.expect(f"Now: Controls — Nav {third}")
        session.send("?p")
        session.expect("Help", f"Now: Controls — Nav {second}")
        session.send(" h")
        session.expect("State: paused", "00:00")
        session.send("l")
        session.expect("State: paused", "00:05", "Help")
        session.send("in")
        session.expect("Track details", f"Now: Controls — Nav {third}", "State: playing")
        session.send("\x1bL")
        session.expect("Last.fm setup", "needs API credentials")
        session.send("dq")
        session.finish()


def exercise_details_metadata(executable, audio, root):
    library = root / "details.ndjson"
    fifo = root / "not-audio-fifo"
    os.mkfifo(fifo)
    records = [{"type": "track", "stable_id": str(index), "beatmap_set_id": str(index),
                "title": title, "artist": "Metadata", "audio_filename": "audio.wav",
                "audio_hash": hashlib.sha256(audio.read_bytes()).hexdigest(),
                "audio_path": str(path), "bpm": 174.5, "date_added": "2026-01-02T03:04:05Z"}
               for index, (title, path) in enumerate((("A audio", audio), ("B FIFO", fifo)))]
    write_library(library, records)
    before = hashlib.sha256(audio.read_bytes()).hexdigest()
    with Session(executable, library) as session:
        session.expect("Library: 2", "State: stopped")
        session.send("i")
        session.expect("Duration: 00:08", "BPM: 174.5", "Added: 2026-01-02", "State: stopped")
        session.send("\x1bji")
        session.expect("Title: B FIFO", "Unavailable (probe failed", "State: stopped")
        session.resize(14, 50)
        session.send("qki")
        session.resize(24, 100)
        session.expect("Title: A audio", "Duration: 00:08")
        session.send("q?iq")  # Cancel/switch rapidly; no stale result can replace another panel.
        session.expect("Library (Enter plays")
        session.send("q")
        session.finish()
    assert hashlib.sha256(audio.read_bytes()).hexdigest() == before


def exercise_lastfm_setup(executable, library, root):
    config = root / "lastfm-config"
    environment = {"XDG_CONFIG_HOME": str(config)}
    with Session(executable, library, **environment) as session:
        session.expect("Last.fm: off", "Matches: 3")
        session.send("\nL")
        session.expect("Last.fm setup", "needs API credentials", "State: playing")
        session.send("\nxyz\n")
        session.expect("Enter exactly 32 hexadecimal characters.")
        session.send("\x15" + "a" * 32 + "\n")
        session.expect("API secret: _", "State: playing")
        # Cancel before submitting credentials: this PTY test never contacts Last.fm.
        secret = "b123456789abcdef"
        session.send(secret)
        session.expect("API secret: " + "*" * len(secret) + "_")
        assert secret not in session.screen.text(), "API secret leaked on screen"
        session.resize(8, 30)
        session.expect("Terminal too small")
        session.send("\x1b")
        session.resize(24, 100)
        session.expect("Matches: 3", "State: playing")
        session.send("L")
        session.expect("Last.fm setup", "needs API credentials")
        session.send("d")
        session.expect("Last.fm: off", "Matches: 3")
        session.send("q")
        session.finish()
    settings = json.loads((config / "osu-player/settings.json").read_text())
    assert settings["lastfm"] == {"enabled": False}
    assert not (config / "osu-player/lastfm/credentials.json").exists()
    # Enabled but unconfigured must also launch into the library, not force setup.
    settings["lastfm"]["enabled"] = True
    (config / "osu-player/settings.json").write_text(json.dumps(settings))
    with Session(executable, library, **environment) as session:
        session.expect("Last.fm: needs credentials", "Library (Enter plays", "State: stopped")
        assert "Last.fm setup" not in session.screen.text()
        session.send("L")
        session.expect("Last.fm setup", "needs API credentials")
        session.send("d")
        session.expect("Last.fm: off", "Library (Enter plays")
        session.send("q")
        session.finish()
    with Session(executable, library, **environment) as session:
        session.expect("Last.fm: off", "State: stopped")
        session.send("q")
        session.finish()


def exercise_settings(executable, library, root):
    root = root / "settings-tests"
    root.mkdir()
    config = root / "config"
    path = config / "osu-player/settings.json"
    environment = {"XDG_CONFIG_HOME": str(config)}
    with Session(executable, library, **environment) as session:
        session.expect("Library: 3", "Volume: 70%", "Shuffle: off", "Repeat: off")
        session.send("--sRR/Bêta\n")
        session.expect("Volume: 60%", "Shuffle: on", "Repeat: all", "Search: Bêta", "Matches: 1")
        # Save immediately, not just at orderly exit.
        saved = json.loads(path.read_text())
        assert abs(saved["volume"] - 0.6) < 0.001 and saved["shuffle"] and saved["repeat"] == "all"
        assert saved["search"] == "Bêta"
        assert path.stat().st_mode & 0o777 == 0o600
        session.send("/uncommitted")
        session.process.send_signal(signal.SIGTERM)
        session.finish(128 + signal.SIGTERM)
    with Session(executable, library, **environment) as session:
        session.expect("Volume: 60%", "Shuffle: on", "Repeat: all", "Search: Bêta", "Matches: 1",
                       "State: stopped", "Now: No track selected")
        session.send("\x1b")
        session.expect("Matches: 3")
        session.send("q")
        session.finish()
    assert json.loads(path.read_text())["search"] == ""
    assert [entry for entry in path.parent.iterdir() if entry.name != "collections"] == [path], "staging files leaked"

    # Invalid, oversized, and newer-schema files must not crash or be overwritten.
    for contents in ('{broken', '{"version":1,"volume":2}', '{"version":1,"volume":NaN}',
                     '{"version":1,"shuffle":"yes"}',
                     '{"version":1,"repeat":"bad"}', '{"version":2}', 'x' * 4097,
                     '{"version":1} trailing'):
        path.write_text(contents)
        with Session(executable, library, **environment) as session:
            session.expect("Library: 3", "Volume: 70%", "Settings:")
            session.send("+sRq")
            session.finish()
        assert path.read_text() == contents

    for volume in (0, 1):
        path.write_text(json.dumps({"version": 1, "volume": volume}))
        with Session(executable, library, **environment) as session:
            session.expect("Library: 3", f"Volume: {volume * 100}%")
            session.send("q")
            session.finish()

    path.write_text('{"version":1}')
    with Session(executable, library, **environment) as session:
        session.expect("Library: 3")
        session.send("+s")
        session.expect("Volume: 75%", "Shuffle: on")
        session.process.kill()
        session.process.wait(timeout=5)
    with Session(executable, library, **environment) as session:
        session.expect("Library: 3", "Volume: 75%", "Shuffle: on", "State: stopped")
        session.send("q")
        session.finish()

    # Missing optional properties use defaults; missing file isn't an error.
    path.write_text('{"version":1,"shuffle":true}')
    with Session(executable, library, **environment) as session:
        session.expect("Library: 3", "Volume: 70%", "Shuffle: on", "Repeat: off")
        session.send("q")
        session.finish()
    path.unlink()
    # A readable settings file with a blocked staging directory is tested using
    # /proc: path resolution works, but creating an app config directory cannot.
    with Session(executable, library, XDG_CONFIG_HOME="/proc") as session:
        session.expect("Library: 3")
        session.send("+")
        session.expect("Volume: 75%", "Settings not saved:")
        session.send("q")
        session.finish()

    # Relative XDG paths are ignored per the base-directory specification.
    home = root / "home"
    home.mkdir()
    with Session(executable, library, XDG_CONFIG_HOME="relative", HOME=str(home)) as session:
        session.expect("Library: 3")
        session.send("-q")
        session.finish()
    assert abs(json.loads((home / ".config/osu-player/settings.json").read_text())["volume"] - 0.65) < 0.001


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-binary", default="./bin/osu-player")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="osu-tui-test-") as directory:
        root = Path(directory)
        audio = root / "generated-audio"  # Extensionless WAV; no copyrighted content.
        with wave.open(str(audio), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(22050)
            output.writeframes(b"\0\0" * 22050 * 8)
        content_hash = hashlib.sha256(audio.read_bytes()).hexdigest()
        records = []
        for index, title in enumerate(("Alpha 日本", "Bêta", "Z Broken\x1b[2J\nRow")):
            records.append({"type": "track", "stable_id": str(index), "beatmap_set_id": str(index),
                            "title": title, "artist": "Artist", "audio_filename": "audio.wav",
                            "audio_hash": content_hash,
                            "audio_path": str(audio if index < 2 else root / "missing")})
        library = root / "library.ndjson"
        write_library(library, records)
        exercise_ui(options.player_binary, library)
        exercise_queue_keys(options.player_binary, audio, root)
        exercise_details_metadata(options.player_binary, audio, root)
        exercise_settings(options.player_binary, library, root)
        exercise_lastfm_setup(options.player_binary, library, root)

        empty = root / "empty.ndjson"
        write_library(empty, [])
        with Session(options.player_binary, empty, TERM="vt100") as session:
            session.expect("Library is empty", "State: empty")
            session.send("i")
            session.expect("Track details", "No track selected.")
            session.send("\x1b?")
            session.expect("Help", "State: empty")
            session.send("\x1b")
            session.send("\n npsRx")
            session.expect("State: empty")
            session.send("q")
            session.finish()

        with Session(options.player_binary, library,
                     SDL_AUDIODRIVER="osu-player-nonexistent-driver") as session:
            # The screen can render before the background loader publishes tracks.
            session.expect("Library: 3", "State: stopped")
            session.send("\n")
            session.expect("Audio error:", "State: failed")
            session.send("q")
            session.finish()

        with Session(options.player_binary, library) as session:
            session.expect("osu! music player")
            session.process.send_signal(signal.SIGTERM)
            session.finish(128 + signal.SIGTERM)

        with Session(options.player_binary, library) as session:
            session.expect("osu! music player")
            session.process.send_signal(signal.SIGINT)
            session.finish(128 + signal.SIGINT)

        piped = subprocess.run([options.player_binary, "tui", str(library)], input="q",
                               text=True, capture_output=True, timeout=10)
        if piped.returncode != 1 or "interactive terminal" not in piped.stderr:
            raise RuntimeError("non-TTY input/output must be rejected cleanly")
    print("TUI PTY tests passed: keys, UTF-8 search, responsive help/details panels, resize, empty library, device error, terminal cleanup, settings persistence, Last.fm setup/masking/cancel (no network)")


if __name__ == "__main__":
    main()
