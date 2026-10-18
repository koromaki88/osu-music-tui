"""Background refresh/cache tests; only disposable sources and a fake adapter are used."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import tempfile
import time
import wave

from tui_integration import Session, write_library


ADAPTER = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).parent
assert sys.argv[1] == "--osu-dir" and len(sys.argv) == 3
assert Path(sys.argv[2]) in (root / "osu data ; literal", root / "other source", root / "home/.local/share/osu")
config = json.loads((root / "config.json").read_text())
# Simulate a private Realm snapshot left behind even on forced termination.
snapshot = Path(tempfile.mkdtemp(prefix="fake-realm-")) / "client.realm"
snapshot.write_text("synthetic private snapshot")
with (root / "calls").open("a") as output:
    output.write(str(os.getpid()) + "\n")
time.sleep(config.get("delay", 0))
if config.get("mutate"):
    (Path(sys.argv[2]) / "client.realm").write_text("changed during indexing")
if config.get("fail"):
    print('{"type":"header","schema_version":1}')
    print("synthetic index failure", file=sys.stderr)
    sys.exit(1)
if config.get("malformed"):
    print('{"type":"header","schema_version":99}')
else:
    sys.stdout.write((root / "export.ndjson").read_text())
'''


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def exercise(executable, root):
    source = root / "osu data ; literal"
    source.mkdir()
    realm = source / "client.realm"
    realm.write_bytes(b"synthetic database; never opened as Realm")
    original = realm.read_bytes()
    cache_home = root / "cache"
    staging = root / "staging"
    staging.mkdir()
    adapter = root / "fake indexer"
    adapter.write_text(ADAPTER)
    adapter.chmod(0o700)
    config = root / "config.json"

    def configure(**settings):
        config.write_text(json.dumps(settings))

    def calls():
        path = root / "calls"
        return path.read_text().splitlines() if path.exists() else []

    def no_temporaries():
        check(not list(cache_home.rglob("*.tmp.*")), "cache staging files were not cleaned up")
        check(not list(staging.iterdir()), "indexer staging directory was not cleaned up")

    audio = root / "audio"
    with wave.open(str(audio), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(22050)
        output.writeframes(b"\0\0" * 22050 * 30)
    records = [{"type": "track", "stable_id": str(index), "beatmap_set_id": str(index),
                "artist": "Artist", "title": title, "audio_hash": hashlib.sha256(audio.read_bytes()).hexdigest(),
                "audio_filename": "audio.wav", "audio_path": str(audio)}
               for index, title in enumerate(("Alpha", "Beta", "Gamma"))]
    export = root / "export.ndjson"
    write_library(export, records)
    configure()
    arguments = ["--osu-dir", str(source), "--indexer", str(adapter)]
    environment = {"XDG_CACHE_HOME": str(cache_home), "TMPDIR": str(staging)}

    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Library: 3", "Library indexed.", "State: stopped")
        check(len(calls()) == 1, "first start must index exactly once")
        cache, = cache_home.rglob("*.ndjson")
        check(cache.stat().st_mode & 0o777 == 0o600, "cache metadata must be private")
        session.send("\n ")
        session.expect("State: paused", "Now: Artist — Alpha")
        session.send("\x1bOC")
        session.expect("00:05")
        records[0]["title"] = "Revised Alpha"  # Moves selected/current track to the end.
        write_library(export, records)
        configure(delay=1.5)
        session.send("r")
        session.expect("Index: running")
        session.send("r")
        session.expect("Refresh already running.")
        session.send("+/alpha\n")
        session.expect("Matches: 1", "Volume: 75%", "State: paused", "Now: Artist — Alpha")
        session.expect("Library indexed.", "Now: Artist — Revised Alpha", "00:05", "State: paused")
        check(len(calls()) == 2, "duplicate refresh must not spawn another indexer")
        session.send("\x1b \x1bOC")
        session.expect("State: playing", "00:10")
        configure(delay=1.5)
        session.send("r")
        session.expect("Index: running", "State: playing")
        session.expect("Library indexed.", "State: playing", "00:11")

        good_cache = cache.read_bytes()
        configure(fail=True)
        session.send("r")
        session.expect("Refresh failed:", "synthetic index failure", "Library: 3", "State: playing")
        check(cache.read_bytes() == good_cache, "failed adapter replaced the last valid cache")
        configure(malformed=True)
        session.send("r")
        session.expect("Refresh failed:", "unsupported schema version", "Library: 3")
        check(cache.read_bytes() == good_cache, "invalid output replaced the last valid cache")
        configure()
        write_library(export, records[1:])
        session.send("r")
        session.expect("Library indexed.", "Library: 2", "State: stopped", "Now: No track selected")
        session.send("q")
        session.finish()
    no_temporaries()
    check(realm.read_bytes() == original, "refresh modified the original database")

    count = len(calls())
    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Loaded cached library.", "Library: 2")
        session.send("q")
        session.finish()
    check(len(calls()) == count, "unchanged source must use cache without starting an adapter")

    alias = root / "source alias"
    alias.symlink_to(source, target_is_directory=True)
    with Session(executable, arguments=["--osu-dir", str(alias), "--indexer", str(adapter)], **environment) as session:
        session.expect("Loaded cached library.", "Library: 2")
        session.send("q")
        session.finish()
    check(len(calls()) == count, "canonical source aliases should share their cache")

    # A stale cache remains usable even if a subsequent refresh fails.
    realm.write_bytes(original + b" changed")
    configure(delay=1, fail=True)
    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Library: 2", "Loaded stale cache")
        session.send("\n")
        session.expect("State: playing")
        session.expect("Refresh failed:", "Library: 2", "State: playing")
        session.send("q")
        session.finish()

    good_cache = cache.read_bytes()
    configure(delay=30)
    with Session(executable, arguments=[*arguments, "--index-timeout", "1"], **environment) as session:
        session.expect("Library: 2")
        session.expect("Refresh failed: indexer timed out", "Library: 2")
        session.send("q")
        session.finish()
    check(cache.read_bytes() == good_cache, "timeout replaced the last valid cache")
    no_temporaries()

    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Library: 2", "Loaded stale cache")
        started = time.monotonic()
        session.send("q")
        session.finish()
        check(time.monotonic() - started < 2, "quit waited for a slow adapter")
    for pid in calls():
        check(not Path(f"/proc/{pid}").exists(), "adapter was not terminated and reaped")
    check(cache.read_bytes() == good_cache, "cancelled refresh replaced the last valid cache")
    no_temporaries()

    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Library: 2", "Loaded stale cache")
        session.process.send_signal(signal.SIGTERM)
        session.finish(128 + signal.SIGTERM)
    check(cache.read_bytes() == good_cache, "signal cancellation replaced the last valid cache")
    no_temporaries()

    configure(mutate=True)
    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Refresh failed:", "client.realm changed during indexing", "Library: 2")
        session.send("q")
        session.finish()
    check(cache.read_bytes() == good_cache, "concurrent source mutation replaced the cache")

    # No-cache skips both reads and writes, even if a usable cache already exists.
    configure()
    with Session(executable, arguments=[*arguments, "--no-cache"], **environment) as session:
        session.expect("Library indexed.", "Library: 2")
        session.send("q")
        session.finish()
    check(cache.read_bytes() == good_cache, "--no-cache wrote a cache")

    # Corrupt caches are discarded, never treated as an empty successful library.
    cache.write_text("not JSON\n")
    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Library indexed.", "Library: 2")
        session.send("q")
        session.finish()
    check(cache.read_text().startswith("{"), "corrupt cache was not replaced by a validated index")

    cached_lines = cache.read_text().splitlines()
    cache.write_text("\n".join(cached_lines[:-1]) + "\n")
    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Library indexed.", "Library: 2")
        session.send("q")
        session.finish()
    check(len(cache.read_text().splitlines()) == len(cached_lines), "truncated cache was accepted")

    other = root / "other source"
    other.mkdir()
    (other / "client.realm").write_bytes(original)
    with Session(executable, arguments=["--osu-dir", str(other), "--indexer", str(adapter)], **environment) as session:
        session.expect("Library indexed.", "Library: 2")
        session.send("q")
        session.finish()
    check(len(list(cache_home.rglob("*.ndjson"))) == 2, "different source directories need isolated caches")

    # Omitted --osu-dir resolves under a disposable HOME, never the real library.
    home = root / "home"
    default_source = home / ".local/share/osu"
    default_source.mkdir(parents=True)
    (default_source / "client.realm").write_bytes(original)
    with Session(executable, arguments=["--indexer", str(adapter), "--no-cache"],
                 **(environment | {"HOME": str(home)})) as session:
        session.expect("Library indexed.", "Library: 2")
        session.send("q")
        session.finish()
    check((default_source / "client.realm").read_bytes() == original, "default source was modified")
    (default_source / "client.realm").unlink()
    with Session(executable, arguments=[], **(environment | {"HOME": str(home)})) as session:
        session.expect("Refresh failed:", "cannot stat")
        session.send("q")
        session.finish()

    # Missing database: use the last known library, but show the refresh error.
    realm.unlink()
    with Session(executable, arguments=arguments, **environment) as session:
        session.expect("Library: 2", "Refresh failed:", "cannot stat")
        session.send("q")
        session.finish()
    realm.write_bytes(original)

    # An unusable cache location is nonfatal; don't write under the osu! directory.
    with Session(executable, arguments=arguments,
                 **(environment | {"XDG_CACHE_HOME": str(source)})) as session:
        session.expect("Library indexed.", "Cache unavailable:", "Library: 2")
        session.send("q")
        session.finish()
    check(not (source / "osu-music-player").exists(), "cache was created in the original library")

    blocked = root / "not a directory"
    blocked.write_text("block cache creation")
    with Session(executable, arguments=arguments,
                 **(environment | {"XDG_CACHE_HOME": str(blocked)})) as session:
        session.expect("Library indexed.", "Cache not saved:", "Library: 2")
        session.send("q")
        session.finish()

    with Session(executable, arguments=["--osu-dir", str(source), "--indexer", str(root / "absent"),
                                        "--no-cache"], **environment) as session:
        session.expect("Refresh failed:", "cannot launch indexer")
        session.send("q")
        session.finish()

    # Export-file mode reloads on r, but retains the old library after bad input.
    with Session(executable, export, **environment) as session:
        session.expect("Library: 2", "Library reloaded.")
        session.send("\n")
        session.expect("State: playing")
        export.write_text("broken\n")
        session.send("r")
        session.expect("Refresh failed:", "Library: 2", "State: playing")
        write_library(export, [])
        session.send("r")
        session.expect("Library reloaded.", "Library is empty", "State: empty")
        session.send("q")
        session.finish()
    no_temporaries()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-binary", default="./bin/osu-player")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="osu-refresh-test-") as directory:
        exercise(options.player_binary, Path(directory))
    print("refresh/cache PTY tests passed: nonblocking refresh, cache reuse, failures, timeout, cancellation, read-only sources")


if __name__ == "__main__":
    main()
