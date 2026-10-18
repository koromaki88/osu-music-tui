"""Test an extracted pacman package without installing it or touching real osu! data."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import wave

from tui_integration import Session, exercise_ui, write_library


ROOT = Path(__file__).resolve().parent.parent


def fingerprint(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path,
                        default=ROOT / "artifacts/arch/osu-music-player-0.1.0-1-x86_64.pkg.tar.zst")
    parser.add_argument("--dotnet", default="dotnet")
    options = parser.parse_args()
    package = options.package.resolve()
    with package.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    assert digest == package.with_suffix(package.suffix + ".sha256").read_text().split()[0]
    with tempfile.TemporaryDirectory(prefix="osu-arch-test-") as directory:
        root = Path(directory)
        unpacked = root / "package with spaces"
        unpacked.mkdir()
        subprocess.run(["bsdtar", "-xf", str(package), "-C", str(unpacked)], check=True)
        metadata = (unpacked / ".PKGINFO").read_text()
        assert "depend = dotnet-runtime-8.0" in metadata
        assert "depend = libavcodec.so=" in metadata, "FFmpeg ABI dependency is missing"
        indexer = unpacked / "usr/lib/osu-player/indexer"
        config = json.loads((indexer / "osu-indexer.runtimeconfig.json").read_text())
        assert config["runtimeOptions"]["framework"]["name"] == "Microsoft.NETCore.App"
        assert not (indexer / "libcoreclr.so").exists(), "must use the system runtime"
        assert not (indexer / "runtimes").exists(), "foreign RID assets should not be shipped"
        player = str(unpacked / "usr/bin/osu-player")
        subprocess.run([player, "--help"], check=True, stdout=subprocess.DEVNULL)
        home = root / "home"
        source = home / ".local/share/osu"
        source.parent.mkdir(parents=True)
        subprocess.run([options.dotnet, "run", "--project", str(ROOT / "tests/osu-indexer-tests"),
                        "--configuration", "Release", "--no-build", "--no-restore", "--",
                        "--fixture-output", str(source)], cwd=ROOT, check=True, timeout=120)
        before = fingerprint(source)
        with Session(player, arguments=[], HOME=str(home), XDG_CACHE_HOME=str(home / "cache")) as session:
            session.expect("Library: 2", "Library indexed.")
            session.send("q")
            session.finish()
        assert fingerprint(source) == before, "packaged adapter modified source files"
        audio = root / "audio"
        with wave.open(str(audio), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(22050)
            output.writeframes(b"\0\0" * 22050 * 8)
        content_hash = hashlib.sha256(audio.read_bytes()).hexdigest()
        records = [{"type": "track", "stable_id": str(index), "beatmap_set_id": str(index),
                    "artist": "Artist", "title": title, "audio_hash": content_hash,
                    "audio_filename": "audio.wav", "audio_path": str(audio if index < 2 else root / "missing")}
                   for index, title in enumerate(("Alpha 日本", "Bêta", "Z Broken\x1b[2J\nRow"))]
        exported = root / "library.ndjson"
        write_library(exported, records)
        exercise_ui(player, exported)
    print("Arch package tests passed: metadata/ABI dependencies, packaged indexer, default directory, dummy playback/PTY")


if __name__ == "__main__":
    main()
