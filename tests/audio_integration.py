"""Generate disposable audio; exercise the decoder/device with SDL's dummy output."""

import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", default="./bin/audio-player-tests")
    parser.add_argument("--player-binary", default="./bin/osu-player")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="osu-audio-test-") as directory:
        root = Path(directory)
        source = root / "tone.wav"
        samples = array.array("h", (int(3000 * math.sin(2 * math.pi * 440 * i / 22050))
                                    for i in range(22050 * 2)))
        if sys.byteorder != "little":
            samples.byteswap()
        with wave.open(str(source), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(22050)
            output.writeframes(samples.tobytes())
        fixtures = [source]
        for codec, container in (("libmp3lame", "mp3"), ("libvorbis", "ogg")):
            # Deliberately no extension: the decoder must sniff content.
            fixture = root / f"extensionless-{container}"
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(source),
                            "-c:a", codec, "-f", container, str(fixture)], check=True, timeout=30)
            fixtures.append(fixture)
        environment = dict(os.environ, SDL_AUDIODRIVER="dummy")
        subprocess.run([options.test_binary, *(str(path) for path in fixtures)],
                       env=environment, check=True, timeout=30)

        # Exercise the real CLI against a generated lazer-style hashed MP3.
        content_hash = hashlib.sha256(fixtures[1].read_bytes()).hexdigest()
        stored = root / "files" / content_hash[:1] / content_hash[:2] / content_hash
        stored.parent.mkdir(parents=True)
        shutil.copyfile(fixtures[1], stored)
        played = subprocess.run(
            [options.player_binary, "play", content_hash, str(root)],
            input="pause\nseek 0.5\nvolume 0.2\nresume\nquit\n", text=True,
            capture_output=True, env=environment, check=True, timeout=15,
        )
        if "paused" not in played.stdout or "volume: 0.2" not in played.stdout:
            raise RuntimeError("CLI did not apply transport commands")

        invalid_device = subprocess.run(
            [options.player_binary, "play", content_hash, str(root)],
            input="quit\n", text=True, capture_output=True, timeout=15,
            env=dict(environment, SDL_AUDIODRIVER="osu-player-nonexistent-driver"),
        )
        if invalid_device.returncode == 0 or "audio output" not in invalid_device.stderr:
            raise RuntimeError("invalid audio device must report an actionable failure")

        corrupt = root / "corrupt-audio"
        corrupt.write_bytes(b"not audio")
        # A decoder failure must be an exception, not a crash or a successful load.
        rejected = subprocess.run([options.test_binary, str(corrupt)], env=environment,
                                  text=True, capture_output=True, timeout=15)
        if rejected.returncode != 1 or "open audio" not in rejected.stderr:
            raise RuntimeError("corrupt audio must be rejected cleanly")

        def track_record(identifier, title, path):
            return {"type": "track", "stable_id": identifier, "beatmap_set_id": identifier,
                    "title": title, "artist": "Test Artist", "audio_filename": "audio.mp3",
                    "audio_hash": content_hash, "audio_path": str(path)}

        library = root / "library.ndjson"
        records = [{"type": "header", "schema_version": 1},
                   track_record("broken", "0 Broken", corrupt),
                   track_record("first", "A First", stored),
                   track_record("second", "B Second", stored)]
        library.write_text("\n".join(json.dumps(record) for record in records) + "\n", encoding="utf-8")
        queued = subprocess.run(
            [options.player_binary, "queue", str(library)],
            input="list First\nnext\nprevious\nshuffle on\nrepeat all\ntrack 3\nquit\n",
            env=environment, text=True, capture_output=True, check=True, timeout=15,
        )
        if ("track error [broken]" not in queued.stderr
                or "A First" not in queued.stdout or "B Second" not in queued.stdout
                or "shuffle: on" not in queued.stdout or "repeat: all" not in queued.stdout):
            raise RuntimeError("queue CLI did not skip the bad track or apply queue controls")

        completed = subprocess.run(
            [options.player_binary, "queue", str(library)], input="seek 1.8\n",
            env=environment, text=True, capture_output=True, check=True, timeout=15,
        )
        if "B Second" not in completed.stdout or "Playback finished." not in completed.stdout:
            raise RuntimeError("queue must automatically advance at EOF then stop at its end")

        # A systemic failure must not be reported once per track.
        no_device = subprocess.run(
            [options.player_binary, "queue", str(library)], input="quit\n",
            env=dict(environment, SDL_AUDIODRIVER="osu-player-nonexistent-driver"),
            text=True, capture_output=True, timeout=15,
        )
        if no_device.returncode != 1 or "audio output" not in no_device.stderr:
            raise RuntimeError("queue must abort on output device failure")

        print("audio CLI and error-path tests passed (dummy output; no audible playback)")
        print("queue CLI, failed-track skipping, and automatic EOF advancement passed")


if __name__ == "__main__":
    main()
