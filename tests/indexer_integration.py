"""Opt-in integration test against existing, offline lazer data (never starts the game)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


def fingerprint(root):
    result = {}
    for directory, directories, filenames in os.walk(root, followlinks=False):
        for name in directories + filenames:
            path = Path(directory) / name
            info = path.lstat()
            if stat.S_ISREG(info.st_mode):
                with path.open("rb") as stream:
                    digest = hashlib.file_digest(stream, "sha256").hexdigest()
            elif stat.S_ISLNK(info.st_mode):
                digest = os.readlink(path)
            else:
                # In particular, do not attempt to read Realm notification FIFOs.
                digest = None
            result[str(path.relative_to(root))] = (
                info.st_mode, info.st_size, info.st_mtime_ns, digest
            )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", required=True)
    parser.add_argument("--osu-dir", type=Path, required=True)
    options = parser.parse_args()
    root = options.osu_dir.resolve()
    if not (root / "client.realm").is_file():
        raise RuntimeError("existing client.realm is required; this test never creates game data")

    before = fingerprint(root)
    try:
        indexed = subprocess.run(
            [options.dotnet, "tools/osu-indexer/bin/Release/net8.0/osu-indexer.dll",
             "--osu-dir", str(root)],
            check=True, capture_output=True, text=True, timeout=30,
        )
        records = [json.loads(line) for line in indexed.stdout.splitlines()]
        assert records[0]["type"] == "header" and records[0]["schema_version"] == 1
        tracks = [record for record in records if record["type"] == "track"]
        for track in tracks:
            content_hash = track["audio_hash"]
            expected = root / "files" / content_hash[:1] / content_hash[:2] / content_hash
            assert Path(track["audio_path"]) == expected and expected.is_file()

        displayed = subprocess.run(
            ["./bin/osu-player", "library", "-"], input=indexed.stdout,
            check=True, capture_output=True, text=True, timeout=30,
        )
        assert f"tracks: {len(tracks)}\n" in displayed.stdout
        print(displayed.stdout, end="")

        # Exercise failure handling: no source creation, no partial NDJSON on failure.
        missing = root / "osu-player-test-nonexistent-directory"
        assert not missing.exists()
        failed = subprocess.run(
            [options.dotnet, "tools/osu-indexer/bin/Release/net8.0/osu-indexer.dll",
             "--osu-dir", str(missing)], capture_output=True, text=True, timeout=30,
        )
        assert failed.returncode != 0 and not failed.stdout and not missing.exists()
    finally:
        assert before == fingerprint(root), "source directory changed during integration test"

    print("indexer -> C++ library integration passed; source files unchanged")


if __name__ == "__main__":
    main()
