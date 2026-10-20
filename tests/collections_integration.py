"""Favorites/playlists end-to-end with synthetic audio, isolated config and dummy SDL."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time
import wave

from tui_integration import Session, write_library


def saved(path, predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if path.exists():
            data = json.loads(path.read_text())
            if predicate(data):
                return data
        time.sleep(.02)
    raise AssertionError(f"collections save did not settle: {path}")


def exercise(executable, root):
    audio = root / "fixture.wav"
    with wave.open(str(audio), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(22050)
        output.writeframes(b"\0\0" * 22050 * 60)
    digest = hashlib.sha256(audio.read_bytes()).hexdigest()
    records = [{"type": "track", "stable_id": str(i), "beatmap_set_id": str(i),
                "artist": "Collections", "title": title, "audio_filename": "fixture.wav",
                "audio_hash": digest, "audio_path": str(audio)} for i, title in enumerate(("Alpha", "Beta", "Gamma"))]
    library = root / "library.ndjson"
    write_library(library, records)
    config = root / "config"
    path = config / "osu-player/collections/lists.json"
    environment = {"XDG_CONFIG_HOME": str(config), "OSU_PLAYER_DISCORD_SOCKET": str(root / "absent.sock")}
    with Session(executable, library, **environment) as session:
        session.expect("Library: 3", "State: stopped")
        session.send("F")
        session.expect("View: Favorites", "Matches: 0", "State: stopped")
        session.send("Ff")
        saved(path, lambda data: len(data["favorites"]) == 1)
        session.expect("[fav]", "State: stopped")
        session.send("jf")
        saved(path, lambda data: len(data["favorites"]) == 2)
        session.send("Fg\n")
        session.expect("View: Favorites", "Queue: Favorites", "Now: Collections — Alpha", "State: playing")
        session.send("n")
        session.expect("Now: Collections — Beta")
        session.send("psn")
        session.expect("Shuffle: on", "Now: Collections — Beta")
        session.send("ps")
        session.expect("Shuffle: off", "Now: Collections — Alpha")
        session.send("br")
        session.expect("Library reloaded.", "Matches: 3", "Queue: Favorites")
        session.send("n")
        session.expect("Now: Collections — Beta")
        session.send("gPcMix 日本\n")
        saved(path, lambda data: len(data["playlists"]) == 1)
        session.expect("Playlists", "Mix 日本", "State: playing")
        session.send("a")
        saved(path, lambda data: len(data["playlists"][0]["tracks"]) == 1)
        session.send("qjPa")
        saved(path, lambda data: len(data["playlists"][0]["tracks"]) == 2)
        session.send("\n")
        session.expect("View: Playlist: Mix 日本", "Matches: 2", "Queue: Favorites", "Now: Collections — Beta")
        session.send("g\n")
        session.expect("Queue: Playlist: Mix 日本", "Now: Collections — Alpha")
        session.send("n")
        session.expect("Now: Collections — Beta")
        session.send("G<")
        saved(path, lambda data: data["playlists"][0]["tracks"][0]["id"] == "1")
        session.send("\n")
        session.expect("Now: Collections — Beta")
        session.send("n")
        session.expect("Now: Collections — Alpha")
        session.send("p")
        session.expect("Now: Collections — Beta")
        session.send("u")
        saved(path, lambda data: len(data["playlists"][0]["tracks"]) == 1)
        session.expect("Matches: 1", "Now: Collections — Beta")
        session.send("n")
        session.expect("Now: Collections — Alpha")
        session.send("p")
        session.expect("Now: Collections — Beta")  # Saved-list edits do not mutate active queue.
        session.send("\nRRn")
        session.expect("Repeat: all", "Now: Collections — Alpha", "Matches: 1")
        session.send("Pr")
        session.resize(14, 50)
        session.expect("Name:")
        session.send("\x15Renamed 日本\n")
        saved(path, lambda data: data["playlists"][0]["name"] == "Renamed 日本")
        session.resize(8, 30)
        session.expect("Terminal too small")
        session.resize(24, 100)
        session.send("q")
        session.expect("View: Playlist: Renamed 日本", "State: playing")
        session.send("Px")
        session.expect("Delete playlist?")
        session.send("n")
        assert len(json.loads(path.read_text())["playlists"]) == 1
        session.send("xy")
        saved(path, lambda data: not data["playlists"])
        session.send("q")
        session.expect("Matches: 0", "Now: Collections — Alpha")
        session.send("b")
        session.expect("Matches: 3")
        write_library(library, [records[0], records[2]])
        session.send("r")
        session.expect("Library: 2", "Library reloaded.")
        session.send("F")
        session.expect("View: Favorites", "Matches: 1")
        assert len(json.loads(path.read_text())["favorites"]) == 2, "missing saved tracks must not be deleted"
        write_library(library, records)
        session.send("r")
        session.expect("Library: 3", "Matches: 2")
        session.send("q")
        session.finish()
    assert path.stat().st_mode & 0o777 == 0o600
    assert path.parent.stat().st_mode & 0o777 == 0o700
    assert set(json.loads(path.read_text())) == {"version", "favorites", "playlists"}
    assert str(audio) not in path.read_text(), "saved lists must not contain local audio paths"
    with Session(executable, library, **environment) as session:
        session.expect("State: stopped", "Now: No track selected", "Matches: 3")
        session.send("F")
        session.expect("View: Favorites", "Matches: 2")
        session.send("q")
        session.finish()
    # Unsupported data must remain untouched, with ordinary playback still usable.
    path.write_text('{"version":99}')
    with Session(executable, library, **environment) as session:
        session.expect("State: stopped", "Collections unavailable")
        session.send("f\n")
        session.expect("State: playing")
        session.send("q")
        session.finish()
    assert path.read_text() == '{"version":99}'
    assert hashlib.sha256(audio.read_bytes()).hexdigest() == digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-binary", default=str(Path(__file__).resolve().parent.parent / "bin/osu-player"))
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="osu-collections-pty-", dir="/tmp/opencode" if Path("/tmp/opencode").is_dir() else None) as temporary:
        exercise(options.player_binary, Path(temporary))
    print("Favorites/playlists PTY tests passed: persistent edits, responsive UI, dedicated queues, shuffle/history, reorder, rename/delete confirmation, refresh/missing tracks and no autoplay (synthetic only)")


if __name__ == "__main__":
    main()
