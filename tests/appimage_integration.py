"""Relocated AppImage tests with synthetic Realm, dummy audio, and no dotnet on PATH."""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import wave
from unittest.mock import patch

from tui_integration import Session, exercise_ui, write_library


ROOT = Path(__file__).resolve().parent.parent


def fingerprint(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appimage", type=Path, default=ROOT / "artifacts/dist/osu-player-x86_64.AppImage")
    parser.add_argument("--dotnet", default="dotnet")
    options = parser.parse_args()
    image = options.appimage.resolve()
    dotnet = shutil.which(options.dotnet) or str(Path(options.dotnet).resolve())
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        parser.error("packaging tests require ffmpeg to generate disposable MP3/Ogg fixtures")
    expected = image.with_suffix(image.suffix + ".sha256").read_text().split()[0]
    with image.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == expected, "AppImage checksum mismatch"
    with tempfile.TemporaryDirectory(prefix="osu-appimage-test-") as directory:
        root = Path(directory)
        source = root / "synthetic library"
        subprocess.run([dotnet, "run", "--project", str(ROOT / "tests/osu-indexer-tests"),
                        "--configuration", "Release", "--no-build", "--no-restore", "--",
                        "--fixture-output", str(source)], check=True, cwd=ROOT, timeout=120)
        before = fingerprint(source)
        moved = root / "relocated with spaces"
        moved.mkdir()
        packaged = moved / "music player.AppImage"
        shutil.copy2(image, packaged)
        home = root / "home"
        home.mkdir()
        empty_bin = root / "empty-bin"
        empty_bin.mkdir()
        environment = dict(PATH=str(empty_bin), HOME=str(home), XDG_CACHE_HOME=str(home / "cache"),
                           DOTNET_ROOT=str(root / "no-dotnet"), DOTNET_MULTILEVEL_LOOKUP="0",
                           APPIMAGE_EXTRACT_AND_RUN="1", SDL_AUDIODRIVER="dummy", LD_LIBRARY_PATH="")
        # All application executions below happen outside the source tree, with
        # no dotnet/compiler/tools on PATH and an unusable system DOTNET_ROOT.
        with patch.dict(os.environ, environment), contextlib.chdir(moved):
            help_result = subprocess.run([packaged, "--help"], capture_output=True, text=True, timeout=30)
            assert help_result.returncode == 0 and "usage:" in help_result.stdout, help_result.stderr
            with Session(str(packaged), arguments=["--osu-dir", str(source), "--no-cache"]) as session:
                session.expect("Library: 2", "Library indexed.")
                session.send("q")
                session.finish()

            subprocess.run([packaged, "--appimage-extract"], check=True, stdout=subprocess.DEVNULL, timeout=30)
            appdir = moved / "squashfs-root"
            launcher = str(appdir / "AppRun")
            runtime = json.loads((appdir / "usr/lib/osu-player/indexer/osu-indexer.runtimeconfig.json").read_text())
            assert "framework" not in runtime["runtimeOptions"], "indexer must be self-contained"
            assert runtime["runtimeOptions"]["tfm"] == "net10.0"
            with Session(launcher, arguments=["--osu-dir", str(source), "--no-cache"]) as session:
                session.expect("Library: 2", "Library indexed.")
                session.send("r")
                session.expect("Library indexed.")
                session.send("q")
                session.finish()

            audio = root / "audio"
            with wave.open(str(audio), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(22050)
                output.writeframes(b"\0\0" * 22050 * 8)
            digest = hashlib.sha256(audio.read_bytes()).hexdigest()
            records = [{"type": "track", "stable_id": str(index), "beatmap_set_id": str(index),
                        "artist": "Artist", "title": title, "audio_hash": digest,
                        "audio_filename": "audio.wav", "audio_path": str(audio if index < 2 else root / "missing")}
                       for index, title in enumerate(("Alpha 日本", "Bêta", "Z Broken\x1b[2J\nRow"))]
            exported = root / "playback.ndjson"
            write_library(exported, records)
            exercise_ui(launcher, exported)
            for codec, container in (("libmp3lame", "mp3"), ("libvorbis", "ogg")):
                encoded = root / f"generated.{container}"
                subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-i", str(audio),
                                "-c:a", codec, str(encoded)], check=True, timeout=30)
                content = encoded.read_bytes()
                content_hash = hashlib.sha256(content).hexdigest()
                stored = root / "generated/files" / content_hash[:1] / content_hash[:2] / content_hash
                stored.parent.mkdir(parents=True, exist_ok=True)
                stored.write_bytes(content)
                result = subprocess.run([launcher, "play", content_hash, str(root / "generated")],
                                        input="pause\nseek 0.5\nvolume 0.2\nresume\nquit\n",
                                        text=True, capture_output=True, timeout=15)
                assert result.returncode == 0 and "volume: 0.2" in result.stdout, result.stderr
        assert fingerprint(source) == before, "packaged adapter changed synthetic source files"
    print("AppImage tests passed: relocation, real synthetic Realm indexing, no installed dotnet, WAV/MP3/Ogg dummy playback, PTY controls")


if __name__ == "__main__":
    main()
