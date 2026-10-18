"""Opt-in integration test against existing, offline lazer data (never starts the game)."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import time


def fingerprint(root, verification="quick"):
    """Quick mode covers root entries and Realm/config control files, NOT audio content."""
    result = {}

    def record(path, hash_content):
        info = path.lstat()
        if stat.S_ISREG(info.st_mode) and hash_content:
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

    if verification == "full":
        for directory, directories, filenames in os.walk(root, followlinks=False):
            for name in directories + filenames:
                record(Path(directory) / name, hash_content=True)
    else:
        control_files = {"client.realm", "client.realm.lock", "game.ini"}
        for path in root.iterdir():
            record(path, hash_content=path.name in control_files)
        management = root / "client.realm.management"
        if management.is_dir() and not management.is_symlink():
            for directory, directories, filenames in os.walk(management, followlinks=False):
                for name in directories + filenames:
                    record(Path(directory) / name, hash_content=True)
    return result


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def validate_records(stream, root):
    """Validate one record at a time; don't retain the full exported library in Python."""
    header = json.loads(next(stream, "null"))
    require(isinstance(header, dict) and header.get("type") == "header"
            and header.get("schema_version") == 1, "invalid NDJSON header")
    count = 0
    for line in stream:
        record = json.loads(line)
        require(isinstance(record, dict), "NDJSON records must be objects")
        if record.get("type") == "track":
            content_hash = record["audio_hash"]
            require(isinstance(content_hash, str) and len(content_hash) == 64
                    and all(character in "0123456789abcdef" for character in content_hash),
                    "invalid content hash")
            expected = root / "files" / content_hash[:1] / content_hash[:2] / content_hash
            require(Path(record["audio_path"]) == expected and expected.is_file(),
                    "invalid or missing content path")
            count += 1
        else:
            require(record.get("type") == "warning", "unexpected NDJSON record type")
    return count


def positive_timeout(value):
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("timeout must be a finite positive number of seconds")
    return seconds


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", required=True)
    parser.add_argument("--osu-dir", type=Path, required=True)
    parser.add_argument("--verify", choices=("quick", "full"), default="quick",
                        help="quick: verify control files only; full: hash the entire directory twice")
    parser.add_argument("--timeout", type=positive_timeout, default=300,
                        help="timeout per subprocess in seconds (default: 300)")
    options = parser.parse_args()
    root = options.osu_dir.resolve()
    if not (root / "client.realm").is_file():
        raise RuntimeError("existing client.realm is required; this test never creates game data")

    started = time.monotonic()
    coverage = "control files/root entries only; audio content not hashed" if options.verify == "quick" else "all source files"
    print(f"Verification: {options.verify} ({coverage})", flush=True)
    before = fingerprint(root, options.verify)
    try:
        # Disk-backed temporary streams avoid retaining several full copies of a
        # large export and keep subprocess pipes from becoming backpressure traps.
        with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as indexed, \
                tempfile.TemporaryFile(mode="w+", encoding="utf-8") as displayed:
            print("Indexing offline database...", flush=True)
            subprocess.run(
                [options.dotnet, "tools/osu-indexer/bin/Release/net8.0/osu-indexer.dll",
                 "--osu-dir", str(root)],
                check=True, stdout=indexed, timeout=options.timeout,
            )
            indexed.seek(0)
            count = validate_records(indexed, root)
            indexed.seek(0)
            print(f"Validating {count} tracks through C++...", flush=True)
            subprocess.run(
                ["./bin/osu-player", "library", "-"], stdin=indexed,
                check=True, stdout=displayed, timeout=options.timeout,
            )
            displayed.seek(0)
            summary = [displayed.readline() for _ in range(4)]
            require(f"tracks: {count}\n" in summary, "C++ track count differs from adapter")
            print("".join(summary), end="")
            for _ in range(min(count, 5)):
                print(displayed.readline(), end="")
            if count > 5:
                print(f"... {count - 5} more tracks (listing omitted)")

        # Exercise failure handling: no source creation, no partial NDJSON on failure.
        missing = root / "osu-player-test-nonexistent-directory"
        require(not missing.exists(), "failure-test directory already exists")
        failed = subprocess.run(
            [options.dotnet, "tools/osu-indexer/bin/Release/net8.0/osu-indexer.dll",
             "--osu-dir", str(missing)], capture_output=True, text=True, timeout=options.timeout,
        )
        require(failed.returncode != 0 and not failed.stdout and not missing.exists(),
                "failure must not emit partial NDJSON or create source data")
    finally:
        require(before == fingerprint(root, options.verify),
                "verified source files changed during integration test")

    verified = "control files/root entries unchanged (audio not hashed)" if options.verify == "quick" else "all source files unchanged"
    print(f"Integration passed in {time.monotonic() - started:.1f}s; {verified}")


if __name__ == "__main__":
    main()
